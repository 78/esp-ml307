#ifndef TCP_H
#define TCP_H

#include "network_error.h"

#include <functional>
#include <string>

class Tcp {
public:
    virtual ~Tcp() = default;
    virtual NetworkResult<> Connect(const std::string& host, int port) = 0;
    virtual void Disconnect() = 0;
    virtual int Send(const std::string& data) = 0;

    virtual void OnStream(std::function<void(const std::string& data)> callback) {
        stream_callback_ = callback;
    }

    virtual void OnDisconnected(std::function<void()> callback) {
        disconnect_callback_ = callback;
    }

    bool connected() const { return connected_; }

protected:
    NetworkResult<> Fail(NetworkError err) {
        last_error_ = err;
        return std::unexpected(err);
    }

    std::function<void(const std::string& data)> stream_callback_;
    std::function<void()> disconnect_callback_;
    NetworkError last_error_{};
    bool connected_ = false;
};

#endif  // TCP_H
