#ifndef UDP_H
#define UDP_H

#include "network_error.h"

#include <functional>
#include <string>

class Udp {
public:
    virtual ~Udp() = default;
    virtual NetworkResult<> Connect(const std::string& host, int port) = 0;
    virtual void Disconnect() = 0;
    virtual int Send(const std::string& data) = 0;

    virtual void OnMessage(std::function<void(const std::string& data)> callback) {
        message_callback_ = std::move(callback);
    }
    bool connected() const { return connected_; }

protected:
    NetworkResult<> Fail(NetworkError err) {
        last_error_ = err;
        return std::unexpected(err);
    }

    std::function<void(const std::string& data)> message_callback_;
    NetworkError last_error_{};
    bool connected_ = false;
};

#endif  // UDP_H
