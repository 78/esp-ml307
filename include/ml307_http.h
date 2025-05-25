#ifndef ML307_HTTP_TRANSPORT_H
#define ML307_HTTP_TRANSPORT_H

#include "ml307_at_modem.h"
#include "http.h"
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

#include <map>
#include <string>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <optional>

#define ML307_HTTP_EVENT_INITIALIZED (1 << 0)
#define ML307_HTTP_EVENT_ERROR (1 << 2)
#define ML307_HTTP_EVENT_HEADERS_RECEIVED (1 << 3)
#define ML307_HTTP_EVENT_IND (1 << 4)

class Ml307Http : public Http {
public:
    Ml307Http(Ml307AtModem& modem);
    ~Ml307Http();

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
    Ml307AtModem& modem_;
    EventGroupHandle_t event_group_handle_;
    std::mutex mutex_;
    std::condition_variable cv_;

    int http_id_ = -1;
    int status_code_ = -1;
    int error_code_ = -1;
    int timeout_ms_ = 30000;
    std::string rx_buffer_;
    std::list<CommandResponseCallback>::iterator command_callback_it_;
    std::map<std::string, std::string> headers_;
    std::string url_;
    std::string method_;
    std::string protocol_;
    std::string host_;
    std::string path_;
    std::optional<std::string> content_ = std::nullopt;
    std::map<std::string, std::string> response_headers_;
    std::string body_;
    size_t body_offset_ = 0;
    size_t content_length_ = 0;
    bool eof_ = false;
    bool connected_ = false;
    bool chunked_ = false;

    bool FetchHeaders();
    void ParseResponseHeaders(const std::string& headers);
    std::string ErrorCodeToString(int error_code);
};

#endif