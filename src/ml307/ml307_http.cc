#include "ml307_http.h"
#include <esp_log.h>
#include <cstring>
#include <sstream>
#include <chrono>

static const char *TAG = "Ml307Http";

Ml307Http::Ml307Http(std::shared_ptr<AtUart> at_uart) : at_uart_(at_uart) {
    event_group_handle_ = xEventGroupCreate();

    urc_callback_it_ = at_uart_->RegisterUrcCallback([this](const std::string& command, const std::vector<AtArgumentValue>& arguments) {
        if (command == "MHTTPURC") {
            if (arguments[1].int_value == http_id_) {
                auto& type = arguments[0].string_value;
                if (type == "header") {
                    eof_ = false;
                    body_offset_ = 0;
                    body_.clear();
                    status_code_ = arguments[2].int_value;
                    if (arguments.size() >= 5) {
                        ParseResponseHeaders(at_uart_->DecodeHex(arguments[4].string_value));
                    } else {
                        // FIXME: <header> 被分包发送
                        ESP_LOGE(TAG, "Missing header");
                    }
                    xEventGroupSetBits(event_group_handle_, ML307_HTTP_EVENT_HEADERS_RECEIVED);
                } else if (type == "content") {
                    // +MHTTPURC: "content",<httpid>,<content_len>,<sum_len>,<cur_len>,<data>
                    std::string decoded_data;
                    if (arguments.size() >= 6) {
                        at_uart_->DecodeHexAppend(decoded_data, arguments[5].string_value.c_str(), arguments[5].string_value.length());
                    } else {
                        // FIXME: <data> 被分包发送
                        ESP_LOGE(TAG, "Missing content");
                    }

                    std::lock_guard<std::mutex> lock(mutex_);
                    body_.append(decoded_data);

                    // chunked传输时，EOF由cur_len == 0判断，非 chunked传输时，EOF由content_len判断
                    if (response_chunked_) {
                        eof_ = arguments[4].int_value == 0;
                    } else {
                        eof_ = arguments[3].int_value >= arguments[2].int_value;
                    }
                    
                    body_offset_ += arguments[4].int_value;
                    if (arguments[3].int_value > body_offset_) {
                        ESP_LOGE(TAG, "body_offset_: %u, arguments[3].int_value: %d", body_offset_, arguments[3].int_value);
                        Close();
                        return;
                    }
                    cv_.notify_one();  // 使用条件变量通知
                } else if (type == "err") {
                    error_code_ = arguments[2].int_value;
                    xEventGroupSetBits(event_group_handle_, ML307_HTTP_EVENT_ERROR);
                } else if (type == "ind") {
                    xEventGroupSetBits(event_group_handle_, ML307_HTTP_EVENT_IND);
                } else {
                    ESP_LOGE(TAG, "Unknown HTTP event: %s", type.c_str());
                }
            }
        } else if (command == "MHTTPCREATE") {
            http_id_ = arguments[0].int_value;
            xEventGroupSetBits(event_group_handle_, ML307_HTTP_EVENT_INITIALIZED);
        } else if (command == "FIFO_OVERFLOW") {
            xEventGroupSetBits(event_group_handle_, ML307_HTTP_EVENT_ERROR);
            Close();
        }
    });
}

int Ml307Http::Read(char* buffer, size_t buffer_size) {
    std::unique_lock<std::mutex> lock(mutex_);
    
    if (eof_ && body_.empty()) {
        return 0;
    }
    
    // 使用条件变量等待数据
    auto timeout = std::chrono::milliseconds(timeout_ms_);
    bool received = cv_.wait_for(lock, timeout, [this] { 
        return !body_.empty() || eof_; 
    });
    
    if (!received) {
        ESP_LOGE(TAG, "等待HTTP内容接收超时");
        return -1;
    }
    
    size_t bytes_to_read = std::min(body_.size(), buffer_size);
    std::memcpy(buffer, body_.data(), bytes_to_read);
    body_.erase(0, bytes_to_read);
    
    return bytes_to_read;
}

int Ml307Http::Write(const char* buffer, size_t buffer_size) {
    if (buffer_size == 0) { // FIXME: 模组好像不支持发送空数据
        std::string command = "AT+MHTTPCONTENT=" + std::to_string(http_id_) + ",0,2,\"0D0A\"";
        at_uart_->SendCommand(command);
        return 0;
    }
    std::string command = "AT+MHTTPCONTENT=" + std::to_string(http_id_) + ",1," + std::to_string(buffer_size);
    at_uart_->SendCommand(command);
    at_uart_->SendCommand(std::string(buffer, buffer_size));
    return buffer_size;
}

Ml307Http::~Ml307Http() {
    if (connected_) {
        Close();
    }

    at_uart_->UnregisterUrcCallback(urc_callback_it_);
    vEventGroupDelete(event_group_handle_);
}

void Ml307Http::SetHeader(const std::string& key, const std::string& value) {
    headers_[key] = value;
}

void Ml307Http::SetContent(std::string&& content) {
    content_ = std::make_optional(std::move(content));
}

void Ml307Http::SetTimeout(int timeout_ms) {
    timeout_ms_ = timeout_ms;
}

void Ml307Http::ParseResponseHeaders(const std::string& headers) {
    std::istringstream iss(headers);
    std::string line;
    while (std::getline(iss, line)) {
        std::istringstream line_iss(line);
        std::string key, value;
        std::getline(line_iss, key, ':');
        std::getline(line_iss, value);
    
        // 去除前后空格
        key.erase(0, key.find_first_not_of(" \t"));
        key.erase(key.find_last_not_of(" \t") + 1);
        value.erase(0, value.find_first_not_of(" \t"));
        value.erase(value.find_last_not_of(" \t\r\n") + 1);
        
        response_headers_[key] = value;

        // 检查是否为chunked传输编码
        if (key == "Transfer-Encoding" && value.find("chunked") != std::string::npos) {
            response_chunked_ = true;
            ESP_LOGI(TAG, "Found chunked transfer encoding");
        }
    }
}

bool Ml307Http::Open(const std::string& method, const std::string& url) {
    method_ = method;
    url_ = url;
    
    // 判断是否为需要发送内容的HTTP方法
    bool method_supports_content = (method_ == "POST" || method_ == "PUT");
    
    // 解析URL
    size_t protocol_end = url.find("://");
    if (protocol_end != std::string::npos) {
        protocol_ = url.substr(0, protocol_end);
        size_t host_start = protocol_end + 3;
        size_t path_start = url.find("/", host_start);
        if (path_start != std::string::npos) {
            host_ = url.substr(host_start, path_start - host_start);
            path_ = url.substr(path_start);
        } else {
            host_ = url.substr(host_start);
            path_ = "/";
        }
    } else {
        // URL格式不正确
        ESP_LOGE(TAG, "无效的URL格式");
        return false;
    }

    // 创建HTTP连接
    std::string command = "AT+MHTTPCREATE=\"" + protocol_ + "://" + host_ + "\"";
    if (!at_uart_->SendCommand(command)) {
        ESP_LOGE(TAG, "创建HTTP连接失败");
        return false;
    }

    auto bits = xEventGroupWaitBits(event_group_handle_, ML307_HTTP_EVENT_INITIALIZED, pdTRUE, pdFALSE, pdMS_TO_TICKS(timeout_ms_));
    if (!(bits & ML307_HTTP_EVENT_INITIALIZED)) {
        ESP_LOGE(TAG, "等待HTTP连接创建超时");
        return false;
    }
    connected_ = true;
    request_chunked_ = method_supports_content && !content_.has_value();
    ESP_LOGI(TAG, "HTTP 连接已创建，ID: %d", http_id_);

    if (protocol_ == "https") {
        command = "AT+MHTTPCFG=\"ssl\"," + std::to_string(http_id_) + ",1,0";
        at_uart_->SendCommand(command);
    }

    if (request_chunked_) {
        command = "AT+MHTTPCFG=\"chunked\"," + std::to_string(http_id_) + ",1";
        at_uart_->SendCommand(command);
    }

    // Set HEX encoding OFF
    command = "AT+MHTTPCFG=\"encoding\"," + std::to_string(http_id_) + ",0,0";
    at_uart_->SendCommand(command);

    // Set timeout (seconds): connect timeout, response timeout, input timeout
    // sprintf(command, "AT+MHTTPCFG=\"timeout\",%d,%d,%d,%d", http_id_, timeout_ms_ / 1000, timeout_ms_ / 1000, timeout_ms_ / 1000);
    // modem_.Command(command);

    // Set headers
    for (auto it = headers_.begin(); it != headers_.end(); it++) {
        auto line = it->first + ": " + it->second;
        bool is_last = std::next(it) == headers_.end();
        command = "AT+MHTTPHEADER=" + std::to_string(http_id_) + "," + std::to_string(is_last ? 0 : 1) + "," + std::to_string(line.size()) + ",\"" + line + "\"";
        at_uart_->SendCommand(command);
    }

    if (method_supports_content && content_.has_value()) {
        command = "AT+MHTTPCONTENT=" + std::to_string(http_id_) + ",0," + std::to_string(content_.value().size());
        at_uart_->SendCommand(command);
        at_uart_->SendCommand(content_.value());
        content_ = std::nullopt;
    }

    // Set HEX encoding ON
    command = "AT+MHTTPCFG=\"encoding\"," + std::to_string(http_id_) + ",1,1";
    at_uart_->SendCommand(command);

    // Send request
    // method to value: 1. GET 2. POST 3. PUT 4. DELETE 5. HEAD
    const char* methods[6] = {"UNKNOWN", "GET", "POST", "PUT", "DELETE", "HEAD"};
    int method_value = 1;
    for (int i = 0; i < 6; i++) {
        if (strcmp(methods[i], method_.c_str()) == 0) {
            method_value = i;
            break;
        }
    }
    command = "AT+MHTTPREQUEST=" + std::to_string(http_id_) + "," + std::to_string(method_value) + ",0,";
    if (!at_uart_->SendCommand(command + at_uart_->EncodeHex(path_))) {
        ESP_LOGE(TAG, "发送HTTP请求失败");
        return false;
    }

    if (request_chunked_) {
        auto bits = xEventGroupWaitBits(event_group_handle_, ML307_HTTP_EVENT_IND, pdTRUE, pdFALSE, pdMS_TO_TICKS(timeout_ms_));
        if (!(bits & ML307_HTTP_EVENT_IND)) {
            ESP_LOGE(TAG, "等待HTTP IND超时");
            return false;
        }
    }
    return true;
}

bool Ml307Http::FetchHeaders() {
    // Wait for headers
    auto bits = xEventGroupWaitBits(event_group_handle_, ML307_HTTP_EVENT_HEADERS_RECEIVED | ML307_HTTP_EVENT_ERROR, pdTRUE, pdFALSE, pdMS_TO_TICKS(timeout_ms_));
    if (bits & ML307_HTTP_EVENT_ERROR) {
        ESP_LOGE(TAG, "HTTP请求错误: %s", ErrorCodeToString(error_code_).c_str());
        return false;
    }
    if (!(bits & ML307_HTTP_EVENT_HEADERS_RECEIVED)) {
        ESP_LOGE(TAG, "等待HTTP头部接收超时");
        return false;
    }

    auto it = response_headers_.find("Content-Length");
    if (it != response_headers_.end()) {
        content_length_ = std::stoul(it->second);
    }

    ESP_LOGI(TAG, "HTTP请求成功，状态码: %d", status_code_);
    return true;
}

int Ml307Http::GetStatusCode() {
    if (status_code_ == -1) {
        if (!FetchHeaders()) {
            return -1;
        }
    }
    return status_code_;
}

size_t Ml307Http::GetBodyLength() {
    if (status_code_ == -1) {
        if (!FetchHeaders()) {
            return 0;
        }
    }
    return content_length_;
}

std::string Ml307Http::ReadAll() {
    std::unique_lock<std::mutex> lock(mutex_);
    
    auto timeout = std::chrono::milliseconds(timeout_ms_);
    bool received = cv_.wait_for(lock, timeout, [this] { 
        return eof_; 
    });
    
    if (!received) {
        ESP_LOGE(TAG, "等待HTTP内容接收完成超时");
        return body_;
    }
    
    return body_;
}

void Ml307Http::Close() {
    if (!connected_) {
        return;
    }
    std::string command = "AT+MHTTPDEL=" + std::to_string(http_id_);
    at_uart_->SendCommand(command);

    connected_ = false;
    eof_ = true;
    cv_.notify_one();
    ESP_LOGI(TAG, "HTTP连接已关闭，ID: %d", http_id_);
}

std::string Ml307Http::ErrorCodeToString(int error_code) {
    switch (error_code) {
        case 1: return "域名解析失败";
        case 2: return "连接服务器失败";
        case 3: return "连接服务器超时";
        case 4: return "SSL握手失败";
        case 5: return "连接异常断开";
        case 6: return "请求响应超时";
        case 7: return "接收数据解析失败";
        case 8: return "缓存空间不足";
        case 9: return "数据丢包";
        case 10: return "写文件失败";
        case 255: return "未知错误";
        default: return "未定义错误";
    }
}

std::string Ml307Http::GetResponseHeader(const std::string& key) const {
    auto it = response_headers_.find(key);
    if (it != response_headers_.end()) {
        return it->second;
    }
    return "";
}