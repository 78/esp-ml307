#ifndef ML307_HTTP_TRANSPORT_H
#define ML307_HTTP_TRANSPORT_H

#include "Ml307AtModem.h"
#include "Transport.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include <map>
#include <string>
#include <functional>

#define HTTP_CONNECT_TIMEOUT_MS 15000

#define HTTP_EVENT_INITIALIZED (1 << 0)
#define HTTP_EVENT_ERROR (1 << 2)
#define HTTP_EVENT_HEADERS_RECEIVED (1 << 3)
#define HTTP_EVENT_CONTENT_RECEIVED (1 << 4)


class Ml307Http {
public:
    Ml307Http(Ml307AtModem& modem);
    ~Ml307Http();

    void SetHeader(const std::string key, const std::string value);
    void SetContent(const std::string content);
    void OnData(std::function<void(const std::string& data)> callback);
    bool Open(const std::string method, const std::string url);
    void Close();

    int status_code() const { return status_code_; }
    const std::string& body() const { return body_; }
    const std::map<std::string, std::string>& response_headers() const { return response_headers_; }

private:
    Ml307AtModem& modem_;
    EventGroupHandle_t event_group_handle_;

    int http_id_ = -1;
    int content_length_ = -1;
    int status_code_ = -1;
    int error_code_ = -1;
    std::string rx_buffer_;
    std::list<CommandResponseCallback>::iterator command_callback_it_;
    std::map<std::string, std::string> headers_;
    std::string content_;
    std::string url_;
    std::string method_;
    std::string protocol_;
    std::string host_;
    std::string path_;
    std::map<std::string, std::string> response_headers_;
    std::string body_;
    std::function<void(const std::string& data)> data_callback_;

    void ParseResponseHeaders(const std::string& headers);
    std::string ErrorCodeToString(int error_code);
};

#endif