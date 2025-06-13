#include "esp_http.h"
#include <esp_tls.h>
#include <esp_log.h>
#include <esp_crt_bundle.h>
#include <cstring>

static const char* TAG = "EspHttp";

EspHttp::EspHttp() : client_(nullptr) {}

EspHttp::~EspHttp() {
    Close();
}

void EspHttp::SetHeader(const std::string& key, const std::string& value) {
    headers_[key] = value;
}

void EspHttp::SetContent(std::string&& content) {
    content_ = std::make_optional(std::move(content));
}

void EspHttp::SetTimeout(int timeout_ms) {
    timeout_ms_ = timeout_ms;
}

bool EspHttp::Open(const std::string& method, const std::string& url) {
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.timeout_ms = timeout_ms_;

    ESP_LOGI(TAG, "Opening HTTP connection to %s", url.c_str());

    assert(client_ == nullptr);
    client_ = esp_http_client_init(&config);
    if (!client_) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return false;
    }

    esp_http_client_set_method(client_, 
        method == "GET" ? HTTP_METHOD_GET : 
        method == "POST" ? HTTP_METHOD_POST : 
        method == "PUT" ? HTTP_METHOD_PUT : 
        method == "DELETE" ? HTTP_METHOD_DELETE : HTTP_METHOD_GET);

    for (const auto& header : headers_) {
        esp_http_client_set_header(client_, header.first.c_str(), header.second.c_str());
    }

    bool method_supports_content = (method == "POST" || method == "PUT");
    chunked_ = method_supports_content && !content_.has_value();
    size_t post_content_length = content_.has_value() ? content_.value().length() : 0;
    esp_err_t err = esp_http_client_open(client_, chunked_ ? -1 : post_content_length);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to perform HTTP request: %s", esp_err_to_name(err));
        Close();
        return false;
    }

    if (content_.has_value() && method_supports_content) {
        esp_http_client_write(client_, content_.value().c_str(), content_.value().length());
        content_ = std::nullopt;
    }

    return true;
}

void EspHttp::Close() {
    if (client_) {
        esp_http_client_cleanup(client_);
        client_ = nullptr;
    }
}

int EspHttp::GetStatusCode() {
    if (status_code_ == -1) {
        FetchHeaders();
    }
    return status_code_;
}

std::string EspHttp::GetResponseHeader(const std::string& key) const {
    if (!client_) return "";
    char* value = nullptr;
    esp_http_client_get_header(client_, key.c_str(), &value);
    if (!value) return "";
    std::string result(value);
    free(value);
    return result;
}

size_t EspHttp::GetBodyLength() {
    if (content_length_ == -1) {
        FetchHeaders();
    }
    return content_length_;
}

std::string EspHttp::ReadAll() {
    if (content_length_ == -1) {
        FetchHeaders();
    }
    if (content_length_ > 0) {
        std::string body(content_length_, '\0');
        int bytes_read = Read(body.data(), content_length_);
        assert(bytes_read == static_cast<int>(content_length_));
        return body;
    }
    return {};
}

int EspHttp::Read(char* buffer, size_t buffer_size) {
    if (!client_) return -1;
    return esp_http_client_read(client_, buffer, buffer_size);
}

int EspHttp::Write(const char* buffer, size_t buffer_size) {
    if (!client_) return -1;
    if (chunked_) {
        int ret = 0;
        char chunk_size[16];
        snprintf(chunk_size, sizeof(chunk_size), "%X\r\n", buffer_size);
        ret += esp_http_client_write(client_, chunk_size, strlen(chunk_size));
        ret += esp_http_client_write(client_, buffer, buffer_size);
        ret += esp_http_client_write(client_, "\r\n", 2);
        return ret;
    } else {
        return esp_http_client_write(client_, buffer, buffer_size);
    }
}

bool EspHttp::FetchHeaders() {
    content_length_ = esp_http_client_fetch_headers(client_);
    if (content_length_ < 0) {
        ESP_LOGE(TAG, "Failed to fetch headers");
        return false;
    }
    status_code_ = esp_http_client_get_status_code(client_);
    return true;
}
