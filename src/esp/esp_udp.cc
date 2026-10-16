#include "esp_udp.h"
#include "esp_resolve.h"

#include <esp_log.h>
#include <unistd.h>
#include <cstring>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <errno.h>

static const char *TAG = "EspUdp";

EspUdp::EspUdp() : udp_fd_(-1) {
    event_group_ = xEventGroupCreate();
}

EspUdp::~EspUdp() {
    Disconnect();

    if (event_group_ != nullptr) {
        vEventGroupDelete(event_group_);
        event_group_ = nullptr;
    }
}

NetworkResult<> EspUdp::Connect(const std::string& host, int port) {
    // Also releases the fd left open when the receive task stopped on its own;
    // overwriting udp_fd_ would leak the socket.
    Disconnect();

    auto server_addr = EspResolveIpv4(host, port);
    if (!server_addr) {
        ESP_LOGE(TAG, "Failed to resolve %s", host.c_str());
        return Fail(server_addr.error());
    }

    udp_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_fd_ < 0) {
        ESP_LOGE(TAG, "Failed to create socket");
        return Fail(NetworkError::FromErrno(errno));
    }

    struct timeval receive_timeout = {
        .tv_sec = ESP_UDP_RECEIVE_POLL_MS / 1000,
        .tv_usec = (ESP_UDP_RECEIVE_POLL_MS % 1000) * 1000,
    };
    int ret = setsockopt(udp_fd_, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout, sizeof(receive_timeout));
    if (ret == 0) {
        ret = connect(udp_fd_, reinterpret_cast<const struct sockaddr*>(&*server_addr), sizeof(*server_addr));
    }
    if (ret < 0) {
        auto err = NetworkError::FromErrno(errno);
        ESP_LOGE(TAG, "Failed to connect to %s:%d: %s", host.c_str(), port, err.ToString().c_str());
        close(udp_fd_);
        udp_fd_ = -1;
        return Fail(err);
    }

    connected_ = true;

    xEventGroupClearBits(event_group_, ESP_UDP_EVENT_RECEIVE_TASK_EXIT);
    BaseType_t rc = xTaskCreate([](void* arg) {
        EspUdp* udp = (EspUdp*)arg;
        udp->ReceiveTask();
        xEventGroupSetBits(udp->event_group_, ESP_UDP_EVENT_RECEIVE_TASK_EXIT);
        vTaskDelete(NULL);
    }, "udp_receive", 3072, this, 1, &receive_task_handle_);
    if (rc != pdPASS) {
        ESP_LOGE(TAG, "Failed to create receive task");
        connected_ = false;
        receive_task_handle_ = nullptr;
        close(udp_fd_);
        udp_fd_ = -1;
        return Fail(NetworkError::FromErrno(ENOMEM));
    }
    return {};
}

void EspUdp::Disconnect() {
    connected_ = false;

    // lwIP does not support shutdown() on UDP sockets, so close() wakes the
    // blocked recv(). With LWIP_NETCONN_FULLDUPLEX the fd number stays
    // reserved until that recv() returns.
    if (udp_fd_ != -1) {
        close(udp_fd_);
        udp_fd_ = -1;
    }

    if (receive_task_handle_ != nullptr) {
        auto bits = xEventGroupWaitBits(event_group_, ESP_UDP_EVENT_RECEIVE_TASK_EXIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(10000));
        if (!(bits & ESP_UDP_EVENT_RECEIVE_TASK_EXIT)) {
            ESP_LOGE(TAG, "Failed to wait for receive task exit");
        }
        receive_task_handle_ = nullptr;
    }
}

int EspUdp::Send(const std::string& data) {
    if (!connected_) {
        ESP_LOGE(TAG, "Not connected");
        return -1;
    }

    int ret = send(udp_fd_, data.data(), data.size(), 0);
    if (ret <= 0) {
        ESP_LOGE(TAG, "Send failed: ret=%d, errno=%d", ret, errno);
    }
    return ret;
}

void EspUdp::ReceiveTask() {
    std::string data;
    while (connected_) {
        data.resize(1500);
        int ret = recv(udp_fd_, data.data(), data.size(), 0);
        if (ret < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        }
        if (ret < 0) {
            if (connected_) {
                ESP_LOGE(TAG, "UDP receive failed: errno=%d", errno);
            }
            // The fd stays open until Disconnect() or the next Connect()
            connected_ = false;
            break;
        }
        if (ret == 0) {
            // Empty datagrams are valid and do not close a UDP socket
            continue;
        }

        if (message_callback_) {
            data.resize(ret);
            message_callback_(data);
        }
    }
}
