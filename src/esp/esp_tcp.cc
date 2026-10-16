#include "esp_tcp.h"
#include "esp_resolve.h"

#include <esp_log.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <errno.h>

static const char *TAG = "EspTcp";

static struct timeval MsToTimeval(int timeout_ms) {
    struct timeval tv = {};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    return tv;
}

// Blocking connect() follows CONFIG_LWIP_TCP_SYNMAXRTX and can take minutes
// when SYNs are lost, so connect in non-blocking mode and wait with select().
// Returns 0 or an errno value.
static int ConnectWithTimeout(int fd, const struct sockaddr_in& addr, int timeout_ms) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return errno;
    }

    int err = 0;
    if (connect(fd, reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        err = errno;
        if (err == EINPROGRESS) {
            fd_set write_fds;
            FD_ZERO(&write_fds);
            FD_SET(fd, &write_fds);
            struct timeval tv = MsToTimeval(timeout_ms);
            int ret = select(fd + 1, nullptr, &write_fds, nullptr, &tv);
            if (ret == 0) {
                err = ETIMEDOUT;
            } else if (ret < 0) {
                err = errno;
            } else {
                socklen_t len = sizeof(err);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0) {
                    err = errno;
                }
            }
        }
    }

    if (err == 0 && fcntl(fd, F_SETFL, flags) < 0) {
        err = errno;
    }
    return err;
}

EspTcp::EspTcp() {
    event_group_ = xEventGroupCreate();
}

EspTcp::~EspTcp() {
    Disconnect();

    if (event_group_ != nullptr) {
        vEventGroupDelete(event_group_);
        event_group_ = nullptr;
    }
}

NetworkResult<> EspTcp::Connect(const std::string& host, int port) {
    // Also releases the fd and joins the receive task left by a passive disconnect
    Disconnect();

    auto server_addr = EspResolveIpv4(host, port);
    if (!server_addr) {
        ESP_LOGE(TAG, "Failed to resolve %s", host.c_str());
        return Fail(server_addr.error());
    }
    char ip[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &server_addr->sin_addr, ip, sizeof(ip));
    ESP_LOGI(TAG, "Resolved %s -> %s", host.c_str(), ip);

    tcp_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_fd_ < 0) {
        ESP_LOGE(TAG, "Failed to create socket");
        return Fail(NetworkError::FromErrno(errno));
    }

    int err = ConnectWithTimeout(tcp_fd_, *server_addr, ESP_TCP_CONNECT_TIMEOUT_MS);
    if (err == 0) {
        // Bound blocking send() too, otherwise a stalled peer blocks Send() forever
        struct timeval send_timeout = MsToTimeval(ESP_TCP_SEND_TIMEOUT_MS);
        struct timeval receive_timeout = MsToTimeval(ESP_TCP_RECEIVE_POLL_MS);
        if (setsockopt(tcp_fd_, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout)) < 0 ||
            setsockopt(tcp_fd_, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout, sizeof(receive_timeout)) < 0) {
            err = errno;
        }
    }
    if (err != 0) {
        auto error = NetworkError::FromErrno(err);
        ESP_LOGE(TAG, "Failed to connect to %s:%d: %s", host.c_str(), port, error.ToString().c_str());
        close(tcp_fd_);
        tcp_fd_ = -1;
        return Fail(error);
    }

    connected_ = true;
    xEventGroupClearBits(event_group_, ESP_TCP_EVENT_RECEIVE_TASK_EXIT);
    BaseType_t rc = xTaskCreate([](void* arg) {
        EspTcp* tcp = (EspTcp*)arg;
        tcp->ReceiveTask();
        xEventGroupSetBits(tcp->event_group_, ESP_TCP_EVENT_RECEIVE_TASK_EXIT);
        vTaskDelete(NULL);
    }, "tcp_receive", 4096, this, 1, &receive_task_handle_);
    if (rc != pdPASS) {
        ESP_LOGE(TAG, "Failed to create receive task");
        connected_ = false;
        receive_task_handle_ = nullptr;
        close(tcp_fd_);
        tcp_fd_ = -1;
        return Fail(NetworkError::FromErrno(ENOMEM));
    }
    return {};
}

void EspTcp::Disconnect() {
    bool was_connected = connected_.exchange(false);

    // shutdown() wakes the blocked recv() but keeps the fd number reserved
    // until close() below, after the receive task can no longer use it.
    if (tcp_fd_ != -1) {
        shutdown(tcp_fd_, SHUT_RDWR);
    }

    if (receive_task_handle_ != nullptr) {
        auto bits = xEventGroupWaitBits(event_group_, ESP_TCP_EVENT_RECEIVE_TASK_EXIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(10000));
        if (!(bits & ESP_TCP_EVENT_RECEIVE_TASK_EXIT)) {
            ESP_LOGE(TAG, "Failed to wait for receive task exit");
        }
        receive_task_handle_ = nullptr;
    }

    if (tcp_fd_ != -1) {
        close(tcp_fd_);
        tcp_fd_ = -1;
    }

    if (was_connected && disconnect_callback_) {
        disconnect_callback_();
    }
}

int EspTcp::Send(const std::string& data) {
    if (!connected_) {
        ESP_LOGE(TAG, "Not connected");
        return -1;
    }

    size_t total_sent = 0;
    size_t data_size = data.size();
    const char* data_ptr = data.data();

    while (total_sent < data_size) {
        int ret = send(tcp_fd_, data_ptr + total_sent, data_size - total_sent, 0);

        if (ret <= 0) {
            ESP_LOGE(TAG, "Send failed: ret=%d, errno=%d", ret, errno);
            return ret;
        }

        total_sent += ret;
    }

    return total_sent;
}

void EspTcp::ReceiveTask() {
    std::string data;
    while (connected_) {
        data.resize(1500);
        int ret = recv(tcp_fd_, data.data(), data.size(), 0);
        if (ret < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        }
        if (ret <= 0) {
            // Passive disconnect: only report it, Disconnect() releases the fd.
            // exchange() keeps the callback to one call when Disconnect() races.
            if (connected_.exchange(false)) {
                if (ret < 0) {
                    ESP_LOGE(TAG, "TCP receive failed: %d, errno=%d", ret, errno);
                }
                if (disconnect_callback_) {
                    disconnect_callback_();
                }
            }
            break;
        }

        if (stream_callback_) {
            data.resize(ret);
            stream_callback_(data);
        }
    }
}
