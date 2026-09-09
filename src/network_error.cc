#include "network_error.h"
#include "at_error.h"

#include <cerrno>
#include <cstdio>
#include <netdb.h>

#include <esp_err.h>
#ifdef __has_include
#if __has_include(<esp_tls_errors.h>)
#include <esp_tls_errors.h>
#endif
#endif

static bool IsTlsErr(esp_err_t err) {
#ifdef ESP_ERR_ESP_TLS_BASE
    return err >= ESP_ERR_ESP_TLS_BASE && err < ESP_ERR_ESP_TLS_BASE + 0x100;
#else
    return (static_cast<unsigned>(err) & 0xFFFF0000) == 0x80000000;
#endif
}

NetworkError NetworkError::FromErrno(int err) {
    switch (err) {
        case ETIMEDOUT:
            return Timeout(err);
        case ECONNREFUSED:
        case ECONNRESET:
        case ENETUNREACH:
        case EHOSTUNREACH:
        case ENETDOWN:
            return ConnectFailed(err);
        case EPIPE:
            return ServerDisconnected(err);
        default:
            return ConnectFailed(err);
    }
}

NetworkError NetworkError::FromHErrno(int herr) {
    return DnsFailed(herr);
}

NetworkError NetworkError::FromEsp(esp_err_t err) {
    if (err == ESP_ERR_TIMEOUT) {
        return Timeout(err);
    }
#ifdef ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT
    if (err == ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT) {
        return Timeout(err);
    }
#endif
#ifdef ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME
    if (err == ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME) {
        return DnsFailed(err);
    }
#endif
#ifdef ESP_ERR_ESP_TLS_FAILED_CONNECT_TO_HOST
    if (err == ESP_ERR_ESP_TLS_FAILED_CONNECT_TO_HOST) {
        return ConnectFailed(err);
    }
#endif
    if (IsTlsErr(err)) {
        return TlsFailed(err);
    }
    if (err != ESP_OK) {
        return Unknown(err);
    }
    return Unknown();
}

NetworkError NetworkError::FromMl307Http(int code) {
    switch (code) {
        case 1:
            return DnsFailed(code);
        case 2:
            return ConnectFailed(code);
        case 3:
        case 6:
            return Timeout(code);
        case 4:
            return TlsFailed(code);
        case 5:
            return ServerDisconnected(code);
        case 7:
            return ProtocolError(code);
        default:
            return Unknown(code);
    }
}

NetworkError NetworkError::FromMl307Mqtt(int code) {
    switch (code) {
        case 3:
            return AuthRejected(code);
        case 4:
            return ServerDisconnected(code);
        case 5:
            return Timeout(code);
        case 6:
            return NetworkUnavailable(code);
        default:
            return ConnectFailed(code);
    }
}

NetworkError NetworkError::FromMl307Socket(int code) {
    return ConnectFailed(code);
}

NetworkError NetworkError::FromEc801ESocket(int code) {
    return ConnectFailed(code);
}

NetworkError NetworkError::FromEc801EMqttOpen(int code) {
    switch (code) {
        case 1:
            return InvalidArgument(code);
        case 3:
            return NetworkUnavailable(code);
        case 4:
            return DnsFailed(code);
        case 5:
            return ServerDisconnected(code);
        default:
            return ConnectFailed(code);
    }
}

NetworkError NetworkError::FromEc801EMqttConn(int code) {
    switch (code) {
        case 1:
        case 2:
        case 4:
        case 5:
            return AuthRejected(code);
        case 3:
            return NetworkUnavailable(code);
        default:
            return ConnectFailed(code);
    }
}

const char* NetworkError::Name() const {
    switch (code) {
        case NetworkErrc::InvalidArgument:
            return "invalid_argument";
        case NetworkErrc::NotInitialized:
            return "not_initialized";
        case NetworkErrc::Timeout:
            return "timeout";
        case NetworkErrc::DnsFailed:
            return "dns_failed";
        case NetworkErrc::ConnectFailed:
            return "connect_failed";
        case NetworkErrc::TlsFailed:
            return "tls_failed";
        case NetworkErrc::ProtocolError:
            return "protocol_error";
        case NetworkErrc::AuthRejected:
            return "auth_rejected";
        case NetworkErrc::ServerDisconnected:
            return "server_disconnected";
        case NetworkErrc::NetworkUnavailable:
            return "network_unavailable";
        case NetworkErrc::TransmitFailed:
            return "transmit_failed";
        case NetworkErrc::ReceiveFailed:
            return "receive_failed";
        case NetworkErrc::AtCommandFailed:
            return "at_command_failed";
        case NetworkErrc::CmeError:
            return "cme_error";
        case NetworkErrc::HttpFailed:
            return "http_failed";
        case NetworkErrc::Unknown:
        default:
            return "unknown";
    }
}

const char* NetworkError::Message() const {
    switch (code) {
        case NetworkErrc::InvalidArgument:
            return "Invalid argument";
        case NetworkErrc::NotInitialized:
            return "Not initialized";
        case NetworkErrc::Timeout:
            return "Connection timed out";
        case NetworkErrc::DnsFailed:
            return "DNS resolution failed";
        case NetworkErrc::ConnectFailed:
            return "Connection failed";
        case NetworkErrc::TlsFailed:
            return "TLS handshake failed";
        case NetworkErrc::ProtocolError:
            return "Protocol error";
        case NetworkErrc::AuthRejected:
            return "Authentication rejected";
        case NetworkErrc::ServerDisconnected:
            return "Server disconnected";
        case NetworkErrc::NetworkUnavailable:
            return "Network unavailable";
        case NetworkErrc::TransmitFailed:
            return "Transmit failed";
        case NetworkErrc::ReceiveFailed:
            return "Receive failed";
        case NetworkErrc::AtCommandFailed:
            return "AT command failed";
        case NetworkErrc::CmeError:
            return "CME error";
        case NetworkErrc::HttpFailed:
            return "HTTP request failed";
        case NetworkErrc::Unknown:
        default:
            return "Unknown network error";
    }
}

std::string NetworkError::ToString() const {
    char buf[96];
    if (code == NetworkErrc::HttpFailed && native > 0) {
        snprintf(buf, sizeof(buf), "%s %d (%s)", Message(), native, Name());
    } else if (native != 0) {
        snprintf(buf, sizeof(buf), "%s (%s, native=%d)", Message(), Name(), native);
    } else {
        snprintf(buf, sizeof(buf), "%s (%s)", Message(), Name());
    }
    return buf;
}

NetworkError AtError::ToNetworkError() const {
    switch (code) {
        case AtErrc::Timeout:
            return NetworkError::Timeout();
        case AtErrc::CmeError:
            return NetworkError::CmeError(cme);
        case AtErrc::TransmitFailed:
            return NetworkError::TransmitFailed(esp);
        case AtErrc::NotInitialized:
            return NetworkError::NotInitialized();
        case AtErrc::CommandError:
        default:
            return NetworkError::AtCommandFailed(cme);
    }
}

const char* AtError::Message() const {
    switch (code) {
        case AtErrc::Timeout:
            return "AT command timed out";
        case AtErrc::CommandError:
            return "AT command returned ERROR";
        case AtErrc::CmeError:
            return "AT CME error";
        case AtErrc::TransmitFailed:
            return "AT UART transmit failed";
        case AtErrc::NotInitialized:
            return "AT UART not initialized";
        default:
            return "AT command failed";
    }
}

std::string AtError::ToString() const {
    char buf[80];
    if (code == AtErrc::CmeError && cme != 0) {
        snprintf(buf, sizeof(buf), "%s (cme=%d)", Message(), cme);
    } else if (esp != ESP_OK) {
        snprintf(buf, sizeof(buf), "%s (%s)", Message(), esp_err_to_name(esp));
    } else {
        snprintf(buf, sizeof(buf), "%s", Message());
    }
    return buf;
}
