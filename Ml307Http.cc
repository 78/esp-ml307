#include "Ml307Http.h"
#include "esp_log.h"
#include <cstring>
#include <sstream>

static const char *TAG = "Ml307Http";

Ml307Http::Ml307Http(Ml307AtModem& modem) : modem_(modem) {
    event_group_handle_ = xEventGroupCreate();

    command_callback_it_ = modem_.RegisterCommandResponseCallback([this](const std::string& command, const std::vector<AtArgumentValue>& arguments) {
        if (command == "MHTTPURC") {
            if (arguments[1].int_value == http_id_) {
                auto& type = arguments[0].string_value;
                if (type == "header") {
                    body_.clear();
                    status_code_ = arguments[2].int_value;
                    ParseResponseHeaders(modem_.DecodeHex(arguments[4].string_value));
                    xEventGroupSetBits(event_group_handle_, HTTP_EVENT_HEADERS_RECEIVED);
                } else if (type == "content") {
                    // +MHTTPURC: "content",<httpid>,<content_len>,<sum_len>,<cur_len>,<data>
                    content_length_ = arguments[2].int_value;
                    auto decoded_data = modem_.DecodeHex(arguments[5].string_value);

                    if (data_callback_) {
                        data_callback_(decoded_data);
                    } else {
                        body_.append(decoded_data);
                    }
                    if (content_length_ == arguments[3].int_value) {
                        xEventGroupSetBits(event_group_handle_, HTTP_EVENT_CONTENT_RECEIVED);
                    }
                } else if (type == "err") {
                    error_code_ = arguments[2].int_value;
                    xEventGroupSetBits(event_group_handle_, HTTP_EVENT_ERROR);
                }
            }
        } else if (command == "MHTTPCREATE") {
            http_id_ = arguments[0].int_value;
            xEventGroupSetBits(event_group_handle_, HTTP_EVENT_INITIALIZED);
        } else if (command == "FIFO_OVERFLOW") {
            xEventGroupSetBits(event_group_handle_, HTTP_EVENT_ERROR);
            Close();
        }
    });
}

Ml307Http::~Ml307Http() {
    modem_.UnregisterCommandResponseCallback(command_callback_it_);
    vEventGroupDelete(event_group_handle_);
}

void Ml307Http::SetHeader(const std::string key, const std::string value) {
    headers_[key] = value;
}

void Ml307Http::SetContent(const std::string content) {
    content_ = std::move(content);
}

void Ml307Http::OnData(std::function<void(const std::string& data)> callback) {
    data_callback_ = std::move(callback);
}

void Ml307Http::ParseResponseHeaders(const std::string& headers) {
    std::istringstream iss(headers);
    std::string line;
    while (std::getline(iss, line)) {
        std::istringstream line_iss(line);
        std::string key, value;
        std::getline(line_iss, key, ':');
        std::getline(line_iss, value);
        response_headers_[key] = value;
    }
}

bool Ml307Http::Open(const std::string method, const std::string url) {
    method_ = method;
    url_ = url;
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
    char command[256];
    sprintf(command, "AT+MHTTPCREATE=\"%s://%s\"", protocol_.c_str(), host_.c_str());
    if (!modem_.Command(command)) {
        ESP_LOGE(TAG, "创建HTTP连接失败");
        return false;
    }

    auto bits = xEventGroupWaitBits(event_group_handle_, HTTP_EVENT_INITIALIZED, pdTRUE, pdFALSE, pdMS_TO_TICKS(HTTP_CONNECT_TIMEOUT_MS));
    if (!(bits & HTTP_EVENT_INITIALIZED)) {
        ESP_LOGE(TAG, "等待HTTP连接创建超时");
        return false;
    }
    ESP_LOGI(TAG, "HTTP 连接已创建，ID: %d", http_id_);

    if (protocol_ == "https") {
        sprintf(command, "AT+MHTTPCFG=\"ssl\",%d,1,0", http_id_);
        modem_.Command(command);
    }

    // Set HEX encoding
    sprintf(command, "AT+MHTTPCFG=\"encoding\",%d,1,1", http_id_);
    modem_.Command(command);

    // Set headers
    for (const auto& header : headers_) {
        auto encoded_header = modem_.EncodeHex(header.first + ": " + header.second);
        sprintf(command, "AT+MHTTPCFG=\"header\",%d,%s", http_id_, encoded_header.c_str());
        modem_.Command(command);
    }

    if (!content_.empty() && method_ == "POST") {
        ESP_LOGI(TAG, "content: %s", content_.c_str());
        modem_.Command("AT+MHTTPCONTENT=" + std::to_string(http_id_) + ",0,0," + modem_.EncodeHex(content_));
    }

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
    sprintf(command, "AT+MHTTPREQUEST=%d,%d,0,", http_id_, method_value);
    modem_.Command(std::string(command) + modem_.EncodeHex(path_));

    // Wait for headers
    bits = xEventGroupWaitBits(event_group_handle_, HTTP_EVENT_HEADERS_RECEIVED | HTTP_EVENT_ERROR, pdTRUE, pdFALSE, pdMS_TO_TICKS(HTTP_CONNECT_TIMEOUT_MS));
    if (bits & HTTP_EVENT_ERROR) {
        ESP_LOGE(TAG, "HTTP请求错误: %s", ErrorCodeToString(error_code_).c_str());
        return false;
    }
    if (!(bits & HTTP_EVENT_HEADERS_RECEIVED)) {
        ESP_LOGE(TAG, "等待HTTP头部接收超时");
        return false;
    }

    if (status_code_ >= 400) {
        ESP_LOGE(TAG, "HTTP请求失败，状态码: %d", status_code_);
        return false;
    }

    // Wait for content
    bits = xEventGroupWaitBits(event_group_handle_, HTTP_EVENT_CONTENT_RECEIVED, pdTRUE, pdFALSE, portMAX_DELAY);
    if (!(bits & HTTP_EVENT_CONTENT_RECEIVED)) {
        ESP_LOGE(TAG, "等待HTTP内容接收超时");
        return false;
    }

    ESP_LOGI(TAG, "HTTP请求成功，状态码: %d，内容长度: %d", status_code_, body_.size());
    return true;
}

void Ml307Http::Close() {
    char command[32];
    sprintf(command, "AT+MHTTPDEL=%d", http_id_);
    modem_.Command(command);
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
