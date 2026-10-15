#include "http_client.h"
#include "network_interface.h"
#include <esp_log.h>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <chrono>
#include <algorithm>
#include <cctype>

static const char *TAG = "HttpClient";

// Status, header, chunk-size and trailer lines longer than this are rejected so
// a peer that never sends a newline cannot grow rx_buffer_ without bound.
static constexpr size_t kMaxLineLength = 8192;

static std::string ToLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

HttpClient::HttpClient(NetworkInterface* network, int connect_id) : network_(network), connect_id_(connect_id) {
    event_group_handle_ = xEventGroupCreate();
}

HttpClient::~HttpClient() {
    Close();
    // Destroying the transport joins its receive task, whose callbacks use the
    // event group and the members declared after tcp_ (destroyed before it).
    tcp_.reset();
    vEventGroupDelete(event_group_handle_);
}

void HttpClient::SetTimeout(int timeout_ms) {
    timeout_ms_ = timeout_ms;
}

void HttpClient::SetHeader(const std::string& key, const std::string& value) {
    headers_[ToLower(key)] = HeaderEntry(key, value);
}

void HttpClient::SetContent(std::string&& content) {
    content_ = std::move(content);
}

void HttpClient::SetKeepAlive(bool enable) {
    keep_alive_ = enable;
}

bool HttpClient::IsConnectionReusable(const std::string& host, int port) const {
    std::lock_guard<std::mutex> lock(mutex_);
    // Reuse only an open keep-alive connection to the same endpoint whose
    // previous response was fully parsed. Close-delimited bodies never reach
    // COMPLETE while connected, so they are never reused.
    return connected_ &&
           connected_host_ == host &&
           connected_port_ == port &&
           !connection_error_ &&
           keep_alive_ &&
           server_keep_alive_ &&
           parse_state_ == ParseState::COMPLETE;
}

bool HttpClient::ParseUrl(const std::string& url) {
    // protocol://host[:port][/path]
    size_t protocol_end = url.find("://");
    if (protocol_end == std::string::npos) {
        ESP_LOGE(TAG, "Invalid URL format: %s", url.c_str());
        return false;
    }

    protocol_ = ToLower(url.substr(0, protocol_end));

    size_t host_start = protocol_end + 3;
    size_t path_start = url.find("/", host_start);
    size_t port_start = url.find(":", host_start);

    if (protocol_ == "https") {
        port_ = 443;
    } else {
        port_ = 80;
    }

    if (path_start == std::string::npos) {
        path_ = "/";
        if (port_start != std::string::npos) {
            host_ = url.substr(host_start, port_start - host_start);
            std::string port_str = url.substr(port_start + 1);
            char* endptr;
            long port = strtol(port_str.c_str(), &endptr, 10);
            if (endptr != port_str.c_str() && *endptr == '\0' && port > 0 && port <= 65535) {
                port_ = static_cast<int>(port);
            } else {
                ESP_LOGE(TAG, "Invalid port: %s", port_str.c_str());
                return false;
            }
        } else {
            host_ = url.substr(host_start);
        }
    } else {
        path_ = url.substr(path_start);
        if (port_start != std::string::npos && port_start < path_start) {
            host_ = url.substr(host_start, port_start - host_start);
            std::string port_str = url.substr(port_start + 1, path_start - port_start - 1);
            char* endptr;
            long port = strtol(port_str.c_str(), &endptr, 10);
            if (endptr != port_str.c_str() && *endptr == '\0' && port > 0 && port <= 65535) {
                port_ = static_cast<int>(port);
            } else {
                ESP_LOGE(TAG, "Invalid port: %s", port_str.c_str());
                return false;
            }
        } else {
            host_ = url.substr(host_start, path_start - host_start);
        }
    }

    ESP_LOGD(TAG, "Parsed URL: protocol=%s, host=%s, port=%d, path=%s",
             protocol_.c_str(), host_.c_str(), port_, path_.c_str());
    return true;
}

std::string HttpClient::BuildHttpRequest() {
    // Plain string appends: std::ostringstream would link ~170 KB of iostream/locale code.
    std::string request;

    request += method_ + " " + path_ + " HTTP/1.1\r\n";

    request += "Host: " + host_;
    if ((protocol_ == "http" && port_ != 80) || (protocol_ == "https" && port_ != 443)) {
        request += ":" + std::to_string(port_);
    }
    request += "\r\n";

    for (const auto& [lower_key, header_entry] : headers_) {
        request += header_entry.original_key + ": " + header_entry.value + "\r\n";
    }

    // Framing headers are added only when the caller did not set them.
    bool user_set_content_length = headers_.find("content-length") != headers_.end();
    bool user_set_transfer_encoding = headers_.find("transfer-encoding") != headers_.end();
    bool has_content = content_.has_value() && !content_->empty();
    if (has_content && !user_set_content_length) {
        request += "Content-Length: " + std::to_string(content_->size()) + "\r\n";
    } else if ((method_ == "POST" || method_ == "PUT") && !user_set_content_length && !user_set_transfer_encoding) {
        if (request_chunked_) {
            request += "Transfer-Encoding: chunked\r\n";
        } else {
            request += "Content-Length: 0\r\n";
        }
    }

    if (headers_.find("connection") == headers_.end()) {
        if (keep_alive_) {
            request += "Connection: keep-alive\r\n";
        } else {
            request += "Connection: close\r\n";
        }
    }

    request += "\r\n";
    ESP_LOGD(TAG, "HTTP request headers:\n%s", request.c_str());

    if (has_content) {
        request += *content_;
    }

    return request;
}

NetworkResult<> HttpClient::Open(const std::string& method, const std::string& url) {
    method_ = method;
    url_ = url;

    if (!ParseUrl(url)) {
        return Fail(NetworkError::InvalidArgument());
    }

    const bool can_reuse = protocol_ == connected_protocol_ && IsConnectionReusable(host_, port_);
    
    uint32_t t_connect = xTaskGetTickCount() * portTICK_PERIOD_MS;
    if (can_reuse) {
        ESP_LOGI(TAG, "Reusing existing connection to %s:%d", host_.c_str(), port_);
        ResetRequestState();
    } else {
        if (connected_) {
            ESP_LOGI(TAG, "Closing previous connection (host or port changed, or connection not reusable)");
        }
        // Destroying the old transport joins its receive task, so a late
        // disconnect callback cannot touch the state reset below.
        connected_ = false;
        tcp_.reset();
        ResetRequestState();
        
        if (protocol_ == "https") {
            tcp_ = network_->CreateSsl(connect_id_);
        } else {
            tcp_ = network_->CreateTcp(connect_id_);
        }

        tcp_->OnStream([this](const std::string& data) {
            OnTcpData(data);
        });

        tcp_->OnDisconnected([this]() {
            OnTcpDisconnected();
        });
        
        // Set before Connect() starts the receive task so an immediate peer
        // close is not overwritten afterwards.
        connected_ = true;
        if (auto result = tcp_->Connect(host_, port_); !result) {
            connected_ = false;
            ESP_LOGE(TAG, "TCP connection failed: %s", result.error().ToString().c_str());
            return Fail(result.error());
        }
        uint32_t t_connected = xTaskGetTickCount() * portTICK_PERIOD_MS;
        
        connected_protocol_ = protocol_;
        connected_host_ = host_;
        connected_port_ = port_;
        ESP_LOGI(TAG, "Established new connection to %s:%d protocol=%s cost=%u", host_.c_str(), port_, protocol_.c_str(), t_connected - t_connect);
    }
    
    request_chunked_ = (method_ == "POST" || method_ == "PUT") && !content_.has_value();

    std::string http_request = BuildHttpRequest();
    if (tcp_->Send(http_request) <= 0) {
        ESP_LOGE(TAG, "Send HTTP request failed");
        tcp_->Disconnect();
        connected_ = false;
        return Fail(NetworkError::TransmitFailed());
    }

    // Request content and headers apply to one request only.
    content_ = std::nullopt;
    headers_.clear();

    return {};
}

void HttpClient::Close() {
    if (!connected_) {
        return;
    }

    connected_ = false;
    server_keep_alive_ = false;
    // Release a receive task blocked on body backpressure before joining it.
    WakeReaders();
    tcp_->Disconnect();

    eof_ = true;
    WakeReaders();
    ESP_LOGI(TAG, "HTTP connection closed");
}

void HttpClient::OnTcpData(const std::string& data) {
    // Apply backpressure while more than MAX_BODY_CHUNKS_SIZE of body is
    // unread. mutex_ must not be held here: OnTcpDisconnected() needs it to
    // clear connected_, which is what releases this wait when the peer closes.
    //
    // Only already-queued bytes count, otherwise a single packet of 8 KB or
    // more could never satisfy the predicate even with an empty queue.
    {
        std::unique_lock<std::mutex> read_lock(read_mutex_);
        write_cv_.wait(read_lock, [this] {
            size_t queued_size = 0;
            for (const auto& chunk : body_chunks_) {
                queued_size += chunk.available();
            }
            return queued_size < MAX_BODY_CHUNKS_SIZE || !connected_;
        });
    }

    std::lock_guard<std::mutex> lock(mutex_);
    rx_buffer_.append(data);
    ProcessReceivedData();
}

void HttpClient::OnTcpDisconnected() {
    std::lock_guard<std::mutex> lock(mutex_);
    connected_ = false;
    server_keep_alive_ = false;

    if (connection_error_) {
        // Keep the parse error that was already reported.
    } else if (!headers_received_) {
        // GetStatusCode() waits on event bits rather than cv_, so it must be
        // woken explicitly or it sleeps until timeout_ms_ for a response that
        // can no longer arrive.
        response_error_ = NetworkError::ServerDisconnected();
        connection_error_ = true;
        xEventGroupSetBits(event_group_handle_, EC801E_HTTP_EVENT_ERROR);
        ESP_LOGW(TAG, "Connection closed before response headers were received");
    } else if (!IsDataComplete()) {
        response_error_ = NetworkError::ServerDisconnected();
        connection_error_ = true;
        ESP_LOGE(TAG, "Connection closed prematurely, expected %u bytes but only received %u bytes",
                 content_length_, total_body_received_);
    } else {
        eof_ = true;
    }

    WakeReaders();
}

void HttpClient::ProcessReceivedData() {
    std::string line;
    while (!rx_buffer_.empty() && parse_state_ != ParseState::COMPLETE && !connection_error_) {
        switch (parse_state_) {
            case ParseState::STATUS_LINE:
                if (!TakeLine(&line)) return;
                if (!ParseStatusLine(line)) {
                    SetError();
                    return;
                }
                parse_state_ = ParseState::HEADERS;
                break;

            case ParseState::HEADERS:
                if (!TakeLine(&line)) return;
                if (!line.empty()) {
                    if (!ParseHeaderLine(line)) {
                        SetError();
                        return;
                    }
                    break;
                }
                if (status_code_ >= 100 && status_code_ < 200 && status_code_ != 101) {
                    // Interim response such as 103 Early Hints; the final one follows.
                    response_headers_.clear();
                    parse_state_ = ParseState::STATUS_LINE;
                    break;
                }
                if (!FinishHeaders()) {
                    SetError();
                    return;
                }
                break;

            case ParseState::BODY:
                ParseRegularBody();
                break;

            case ParseState::CHUNK_SIZE:
                if (!TakeLine(&line)) return;
                if (line.empty()) {
                    break;  // Tolerate a stray CRLF between chunks.
                }
                if (!ParseChunkSize(line, &chunk_size_)) {
                    SetError();
                    return;
                }
                chunk_received_ = 0;
                parse_state_ = chunk_size_ == 0 ? ParseState::CHUNK_TRAILER : ParseState::CHUNK_DATA;
                break;

            case ParseState::CHUNK_DATA: {
                size_t available = std::min(rx_buffer_.size(), chunk_size_ - chunk_received_);
                AddBodyData(rx_buffer_.substr(0, available));
                total_body_received_ += available;
                rx_buffer_.erase(0, available);
                chunk_received_ += available;

                if (chunk_received_ == chunk_size_) {
                    // A CRLF split into the next packet is skipped by CHUNK_SIZE.
                    if (rx_buffer_.compare(0, 2, "\r\n") == 0) {
                        rx_buffer_.erase(0, 2);
                    }
                    parse_state_ = ParseState::CHUNK_SIZE;
                }
                break;
            }

            case ParseState::CHUNK_TRAILER:
                if (!TakeLine(&line)) return;
                if (line.empty()) {
                    CompleteResponse();
                }
                break;  // Trailer fields are ignored.

            case ParseState::COMPLETE:
                return;
        }
    }
}

bool HttpClient::TakeLine(std::string* line) {
    const size_t pos = rx_buffer_.find('\n');
    if (pos == std::string::npos || pos > kMaxLineLength) {
        if (rx_buffer_.size() > kMaxLineLength) {
            ESP_LOGE(TAG, "Response line exceeds %u bytes", static_cast<unsigned>(kMaxLineLength));
            SetError();
        }
        return false;
    }

    line->assign(rx_buffer_, 0, pos);
    rx_buffer_.erase(0, pos + 1);
    if (!line->empty() && line->back() == '\r') {
        line->pop_back();
    }
    return true;
}

bool HttpClient::ParseStatusLine(const std::string& line) {
    // HTTP/1.1 200 OK: the status code is the second whitespace-separated token.
    constexpr const char* kSpaces = " \t\r\n";
    const size_t version_start = line.find_first_not_of(kSpaces);
    const size_t version_end = line.find_first_of(kSpaces, version_start);
    const size_t status_start = line.find_first_not_of(kSpaces, version_end);
    if (version_start == std::string::npos || status_start == std::string::npos ||
        line.compare(version_start, 5, "HTTP/") != 0) {
        ESP_LOGE(TAG, "Invalid status line: %s", line.c_str());
        return false;
    }
    const size_t status_end = line.find_first_of(kSpaces, status_start);
    const std::string status_str = line.substr(status_start, status_end - status_start);

    char* endptr;
    long status = strtol(status_str.c_str(), &endptr, 10);

    if (endptr == status_str.c_str() || *endptr != '\0' || status < 100 || status > 999) {
        ESP_LOGE(TAG, "Parse status code failed: %s", status_str.c_str());
        return false;
    }

    status_code_ = static_cast<int>(status);
    ESP_LOGD(TAG, "HTTP status code: %d", status_code_);
    return true;
}

bool HttpClient::ParseHeaderLine(const std::string& line) {
    size_t colon_pos = line.find(':');
    if (colon_pos == std::string::npos) {
        ESP_LOGE(TAG, "Invalid header line: %s", line.c_str());
        return false;
    }

    std::string key = line.substr(0, colon_pos);
    std::string value = line.substr(colon_pos + 1);

    key.erase(0, key.find_first_not_of(" \t"));
    key.erase(key.find_last_not_of(" \t") + 1);
    value.erase(0, value.find_first_not_of(" \t"));
    value.erase(value.find_last_not_of(" \t\r\n") + 1);

    response_headers_[ToLower(key)] = HeaderEntry(key, value);
    return true;
}

bool HttpClient::FinishHeaders() {
    auto conn_it = response_headers_.find("connection");
    if (conn_it != response_headers_.end()) {
        const std::string conn_value = ToLower(conn_it->second.value);
        if (conn_value.find("keep-alive") != std::string::npos) {
            server_keep_alive_ = true;
        } else if (conn_value.find("close") != std::string::npos) {
            server_keep_alive_ = false;
        }
    }

    // RFC 9112 section 6.3: these responses end at the header block whatever
    // their framing headers say.
    const bool no_body = method_ == "HEAD" || (status_code_ >= 100 && status_code_ < 200) ||
                         status_code_ == 204 || status_code_ == 304;
    auto te_it = response_headers_.find("transfer-encoding");
    const bool chunked = !no_body && te_it != response_headers_.end() &&
                         ToLower(te_it->second.value).find("chunked") != std::string::npos;
    auto cl_it = response_headers_.find("content-length");
    const bool has_length = !no_body && !chunked && cl_it != response_headers_.end();

    if (has_length) {
        const std::string& value = cl_it->second.value;
        char* endptr;
        unsigned long length = strtoul(value.c_str(), &endptr, 10);
        if (value.empty() || !std::isdigit(static_cast<unsigned char>(value[0])) || *endptr != '\0') {
            ESP_LOGE(TAG, "Invalid Content-Length: %s", value.c_str());
            return false;
        }
        content_length_ = static_cast<size_t>(length);
    }

    headers_received_ = true;
    if (no_body || (has_length && content_length_ == 0)) {
        CompleteResponse();
    } else if (chunked) {
        response_chunked_ = true;
        parse_state_ = ParseState::CHUNK_SIZE;
    } else {
        // Without Content-Length the body ends when the peer closes.
        parse_state_ = ParseState::BODY;
    }
    xEventGroupSetBits(event_group_handle_, EC801E_HTTP_EVENT_HEADERS_RECEIVED);
    return true;
}

void HttpClient::ParseRegularBody() {
    size_t take = rx_buffer_.size();
    if (content_length_ > 0) {
        take = std::min(take, content_length_ - total_body_received_);
    }

    if (take == rx_buffer_.size()) {
        AddBodyData(std::move(rx_buffer_));
    } else {
        AddBodyData(rx_buffer_.substr(0, take));
        ESP_LOGW(TAG, "Discarding %u bytes after the response body",
                 static_cast<unsigned>(rx_buffer_.size() - take));
        // The stream position is unknown, so the connection must not be reused.
        server_keep_alive_ = false;
    }
    rx_buffer_.clear();
    total_body_received_ += take;

    if (content_length_ > 0 && total_body_received_ >= content_length_) {
        ESP_LOGD(TAG, "HTTP response body received: %u/%u bytes", total_body_received_, content_length_);
        CompleteResponse();
    }
}

bool HttpClient::ParseChunkSize(const std::string& chunk_size_line, size_t* chunk_size) {
    // Hexadecimal size, optionally followed by ";extension".
    std::string size_str = chunk_size_line.substr(0, chunk_size_line.find(';'));
    size_str.erase(size_str.find_last_not_of(" \t\r\n") + 1);

    char* endptr;
    unsigned long size = strtoul(size_str.c_str(), &endptr, 16);
    if (size_str.empty() || !std::isxdigit(static_cast<unsigned char>(size_str[0])) || *endptr != '\0') {
        ESP_LOGE(TAG, "Parse chunk size failed: %s", size_str.c_str());
        return false;
    }

    *chunk_size = static_cast<size_t>(size);
    return true;
}

void HttpClient::CompleteResponse() {
    parse_state_ = ParseState::COMPLETE;
    eof_ = true;
    xEventGroupSetBits(event_group_handle_, EC801E_HTTP_EVENT_COMPLETE);
    WakeReaders();
}

void HttpClient::SetError() {
    ESP_LOGE(TAG, "HTTP parse error");
    response_error_ = NetworkError::ProtocolError();
    connection_error_ = true;
    server_keep_alive_ = false;
    xEventGroupSetBits(event_group_handle_, EC801E_HTTP_EVENT_ERROR);
    WakeReaders();
}

void HttpClient::WakeReaders() {
    // Readers test their predicates under read_mutex_. Passing through it
    // after a state change means a reader either sees the new state or is
    // already waiting, so the notification cannot be lost in between.
    { std::lock_guard<std::mutex> read_lock(read_mutex_); }
    cv_.notify_all();
    write_cv_.notify_all();
}

NetworkResult<int> HttpClient::Read(char* buffer, size_t buffer_size) {
    if (buffer_size == 0) {
        return 0;
    }

    std::unique_lock<std::mutex> read_lock(read_mutex_);
    auto ready = [this] {
        return connection_error_ || !body_chunks_.empty() || eof_ || !connected_;
    };
    if (!cv_.wait_for(read_lock, std::chrono::milliseconds(timeout_ms_), ready)) {
        ESP_LOGE(TAG, "Wait for HTTP content receive timeout");
        return FailValue<int>(NetworkError::Timeout());
    }

    if (connection_error_) {
        return FailValue<int>(response_error_);
    }

    while (!body_chunks_.empty()) {
        auto& front_chunk = body_chunks_.front();
        size_t bytes_read = front_chunk.read(buffer, buffer_size);
        if (front_chunk.empty()) {
            body_chunks_.pop_front();
        }
        if (bytes_read > 0) {
            write_cv_.notify_one();
            return static_cast<int>(bytes_read);
        }
    }

    // End of body, or the connection was closed locally.
    return 0;
}

NetworkResult<int> HttpClient::Write(const char* buffer, size_t buffer_size) {
    if (!connected_) {
        ESP_LOGE(TAG, "Cannot write: connection closed");
        return FailValue<int>(NetworkError::ServerDisconnected());
    }

    auto send_or_fail = [this](const std::string& data) -> NetworkResult<int> {
        int sent = tcp_->Send(data);
        if (sent < 0) {
            return FailValue<int>(NetworkError::TransmitFailed());
        }
        return sent;
    };

    if (request_chunked_) {
        if (buffer_size == 0) {
            return send_or_fail("0\r\n\r\n");
        }

        char size_line[20];
        const int size_len = snprintf(size_line, sizeof(size_line), "%zx\r\n", buffer_size);
        std::string chunk;
        chunk.reserve(size_len + buffer_size + 2);
        chunk.append(size_line, size_len);
        chunk.append(buffer, buffer_size);
        chunk += "\r\n";
        return send_or_fail(chunk);
    }

    if (buffer_size == 0) {
        return 0;
    }
    return send_or_fail(std::string(buffer, buffer_size));
}

NetworkResult<int> HttpClient::GetStatusCode() {
    if (!headers_received_) {
        auto bits = xEventGroupWaitBits(event_group_handle_,
                                        EC801E_HTTP_EVENT_HEADERS_RECEIVED | EC801E_HTTP_EVENT_ERROR,
                                        pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms_));

        if (bits & EC801E_HTTP_EVENT_ERROR) {
            return FailValue<int>(response_error_.empty() ? NetworkError::ProtocolError() : response_error_);
        }
        if (!(bits & EC801E_HTTP_EVENT_HEADERS_RECEIVED)) {
            ESP_LOGE(TAG, "Wait for HTTP headers receive timeout");
            return FailValue<int>(NetworkError::Timeout());
        }
    }

    return status_code_;
}

std::string HttpClient::GetResponseHeader(const std::string& key) const {
    auto it = response_headers_.find(ToLower(key));
    if (it != response_headers_.end()) {
        return it->second.value;
    }
    return "";
}

size_t HttpClient::GetBodyLength() {
    if (!headers_received_) {
        static_cast<void>(GetStatusCode());  // Waits for the headers.
    }

    if (response_chunked_) {
        return 0;  // Unknown for chunked responses.
    }

    return content_length_;
}

void HttpClient::AddBodyData(const std::string& data) {
    if (data.empty()) return;

    std::lock_guard<std::mutex> read_lock(read_mutex_);
    body_chunks_.emplace_back(data);
    cv_.notify_one();
    write_cv_.notify_one();
}

void HttpClient::AddBodyData(std::string&& data) {
    if (data.empty()) return;

    std::lock_guard<std::mutex> read_lock(read_mutex_);
    body_chunks_.emplace_back(std::move(data));
    cv_.notify_one();
    write_cv_.notify_one();
}

std::string HttpClient::ReadAll() {
    // Must keep draining body_chunks_ through Read(): OnTcpData() blocks on
    // write_cv_ once MAX_BODY_CHUNKS_SIZE (8 KB) is queued, so waiting for eof_
    // without reading would deadlock on responses larger than 8 KB.
    // The Read() timeout applies between chunks, not to the whole body.
    std::string result;
    if (content_length_ > 0) {
        result.reserve(content_length_);
    }
    // Heap scratch buffer: ReadAll() runs on small-stack tasks (OTA, MCP,
    // camera explain) where a 1 KB stack array can overflow.
    std::string buffer(1024, '\0');

    while (true) {
        auto bytes_read = Read(buffer.data(), buffer.size());
        if (!bytes_read) {
            ESP_LOGE(TAG, "Cannot read all data: %s", bytes_read.error().ToString().c_str());
            return "";
        }
        if (*bytes_read == 0) {
            break;
        }
        result.append(buffer.data(), static_cast<size_t>(*bytes_read));
    }

    return result;
}

bool HttpClient::IsDataComplete() const {
    if (response_chunked_) {
        return parse_state_ == ParseState::COMPLETE;
    }

    if (content_length_ > 0) {
        return total_body_received_ >= content_length_;
    }

    // No Content-Length and not chunked: the body is delimited by the close.
    return true;
}

void HttpClient::ResetRequestState() {
    // A reused connection's receive task may deliver stray bytes at any time.
    std::lock_guard<std::mutex> lock(mutex_);
    status_code_ = -1;
    response_headers_.clear();
    {
        std::lock_guard<std::mutex> read_lock(read_mutex_);
        body_chunks_.clear();
    }
    content_length_ = 0;
    total_body_received_ = 0;
    eof_ = false;
    headers_received_ = false;
    response_chunked_ = false;
    request_chunked_ = false;
    connection_error_ = false;
    response_error_ = {};
    parse_state_ = ParseState::STATUS_LINE;
    chunk_size_ = 0;
    chunk_received_ = 0;
    rx_buffer_.clear();

    // content_ and headers_ are kept: Open() sends and then clears them, and
    // callers configure headers before each request.

    xEventGroupClearBits(event_group_handle_,
                         EC801E_HTTP_EVENT_HEADERS_RECEIVED |
                         EC801E_HTTP_EVENT_BODY_RECEIVED |
                         EC801E_HTTP_EVENT_ERROR |
                         EC801E_HTTP_EVENT_COMPLETE);
}
