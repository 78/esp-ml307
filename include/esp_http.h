#ifndef ESP_HTTP_H
#define ESP_HTTP_H

#include "http.h"
#include <esp_http_client.h>

#include <string>
#include <map>
#include <optional>

class EspHttp : public Http {
public:
    EspHttp();
    virtual ~EspHttp();

    void SetTimeout(int timeout_ms) override;
    void SetHeader(const std::string& key, const std::string& value) override;
    void SetContent(std::string&& content) override;
    bool Open(const std::string& method, const std::string& url) override;
    void Close() override;
    int Read(char* buffer, size_t buffer_size) override;
    int Write(const char* buffer, size_t buffer_size) override;

    int GetStatusCode() override;
    std::string GetResponseHeader(const std::string& key) const override;
    size_t GetBodyLength() override;
    std::string ReadAll() override;

private:
    esp_http_client_handle_t client_;
    std::map<std::string, std::string> headers_;
    std::optional<std::string> content_ = std::nullopt;
    int status_code_ = -1;
    int64_t content_length_ = -1;
    int timeout_ms_ = 30000;
    bool chunked_ = false;

    bool FetchHeaders();
};

#endif // ESP_HTTP_H
