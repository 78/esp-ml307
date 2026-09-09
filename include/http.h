#ifndef HTTP_H
#define HTTP_H

#include "network_error.h"

#include <functional>
#include <map>
#include <string>

class Http {
public:
    virtual ~Http() = default;

    virtual void SetTimeout(int timeout_ms) = 0;
    virtual void SetHeader(const std::string& key, const std::string& value) = 0;
    virtual void SetContent(std::string&& content) = 0;
    virtual void SetKeepAlive(bool enable) = 0;
    virtual NetworkResult<> Open(const std::string& method, const std::string& url) = 0;
    virtual void Close() = 0;
    virtual NetworkResult<int> Read(char* buffer, size_t buffer_size) = 0;
    virtual NetworkResult<int> Write(const char* buffer, size_t buffer_size) = 0;
    virtual NetworkResult<int> GetStatusCode() = 0;
    virtual std::string GetResponseHeader(const std::string& key) const = 0;
    virtual size_t GetBodyLength() = 0;
    virtual std::string ReadAll() = 0;

protected:
    NetworkResult<> Fail(NetworkError err) {
        last_error_ = err;
        return std::unexpected(err);
    }

    template <typename T>
    NetworkResult<T> FailValue(NetworkError err) {
        last_error_ = err;
        return std::unexpected(err);
    }

    NetworkError last_error_{};
};

#endif  // HTTP_H