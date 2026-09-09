#ifndef NETWORK_ERROR_H
#define NETWORK_ERROR_H

#include <expected>
#include <string>

#include <esp_err.h>

enum class NetworkErrc {
    InvalidArgument,
    NotInitialized,
    Timeout,
    DnsFailed,
    ConnectFailed,
    TlsFailed,
    ProtocolError,
    AuthRejected,
    ServerDisconnected,
    NetworkUnavailable,
    TransmitFailed,
    ReceiveFailed,
    AtCommandFailed,
    CmeError,
    HttpFailed,
    Unknown,
};

class NetworkError {
public:
    NetworkErrc code = NetworkErrc::Unknown;
    int native = 0;

    constexpr NetworkError() = default;
    constexpr NetworkError(NetworkErrc c, int n = 0) : code(c), native(n) {}

    static NetworkError InvalidArgument(int n = 0) { return {NetworkErrc::InvalidArgument, n}; }
    static NetworkError NotInitialized(int n = 0) { return {NetworkErrc::NotInitialized, n}; }
    static NetworkError Timeout(int n = 0) { return {NetworkErrc::Timeout, n}; }
    static NetworkError DnsFailed(int n = 0) { return {NetworkErrc::DnsFailed, n}; }
    static NetworkError ConnectFailed(int n = 0) { return {NetworkErrc::ConnectFailed, n}; }
    static NetworkError TlsFailed(int n = 0) { return {NetworkErrc::TlsFailed, n}; }
    static NetworkError ProtocolError(int n = 0) { return {NetworkErrc::ProtocolError, n}; }
    static NetworkError AuthRejected(int n = 0) { return {NetworkErrc::AuthRejected, n}; }
    static NetworkError ServerDisconnected(int n = 0) { return {NetworkErrc::ServerDisconnected, n}; }
    static NetworkError NetworkUnavailable(int n = 0) { return {NetworkErrc::NetworkUnavailable, n}; }
    static NetworkError TransmitFailed(int n = 0) { return {NetworkErrc::TransmitFailed, n}; }
    static NetworkError ReceiveFailed(int n = 0) { return {NetworkErrc::ReceiveFailed, n}; }
    static NetworkError AtCommandFailed(int n = 0) { return {NetworkErrc::AtCommandFailed, n}; }
    static NetworkError CmeError(int n = 0) { return {NetworkErrc::CmeError, n}; }
    static NetworkError HttpFailed(int n = 0) { return {NetworkErrc::HttpFailed, n}; }
    static NetworkError Unknown(int n = 0) { return {NetworkErrc::Unknown, n}; }

    static NetworkError FromErrno(int err);
    static NetworkError FromHErrno(int herr);
    static NetworkError FromEsp(esp_err_t err);
    static NetworkError FromMl307Http(int code);
    static NetworkError FromMl307Mqtt(int code);
    static NetworkError FromMl307Socket(int code);
    static NetworkError FromEc801ESocket(int code);
    static NetworkError FromEc801EMqttOpen(int code);
    static NetworkError FromEc801EMqttConn(int code);

    bool empty() const { return code == NetworkErrc::Unknown && native == 0; }

    const char* Name() const;
    const char* Message() const;
    std::string ToString() const;
};

inline bool operator==(const NetworkError& a, const NetworkError& b) {
    return a.code == b.code && a.native == b.native;
}

inline bool operator!=(const NetworkError& a, const NetworkError& b) {
    return !(a == b);
}

template <typename T = void>
using NetworkResult = std::expected<T, NetworkError>;

#endif  // NETWORK_ERROR_H
