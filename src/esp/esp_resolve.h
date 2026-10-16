#ifndef _ESP_RESOLVE_H_
#define _ESP_RESOLVE_H_

#include "network_error.h"

#include <cstring>
#include <string>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>

// Resolve an IPv4 address with getaddrinfo(). gethostbyname() and inet_ntoa()
// return shared static buffers, so concurrent lookups from other tasks could
// overwrite the result.
inline NetworkResult<sockaddr_in> EspResolveIpv4(const std::string& host, int port) {
    struct addrinfo hints = {};
    hints.ai_family = AF_INET;
    struct addrinfo* result = nullptr;
    int ret = getaddrinfo(host.c_str(), nullptr, &hints, &result);
    if (ret != 0 || result == nullptr) {
        if (result != nullptr) {
            freeaddrinfo(result);
        }
        return std::unexpected(NetworkError::DnsFailed(ret));
    }

    struct sockaddr_in addr = {};
    memcpy(&addr, result->ai_addr, sizeof(addr));
    freeaddrinfo(result);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    return addr;
}

#endif // _ESP_RESOLVE_H_
