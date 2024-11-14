#include "esp_udp.h"

#include <esp_log.h>
#include <unistd.h>
#include <cstring>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netdb.h>

static const char *TAG = "EspUdp";

EspUdp::EspUdp() : udp_fd_(-1) {
}

EspUdp::~EspUdp() {
    Disconnect();
}

bool EspUdp::Connect(const std::string& host, int port) {
    struct sockaddr_in server_addr;
    bzero(&server_addr, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    // host is domain
    struct hostent *server = gethostbyname(host.c_str());
    if (server == NULL) {
        ESP_LOGE(TAG, "Failed to get host by name");
        return false;
    }
    memcpy(&server_addr.sin_addr, server->h_addr, server->h_length);

    udp_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_fd_ < 0) {
        ESP_LOGE(TAG, "Failed to create socket");
        return false;
    }

    int ret = connect(udp_fd_, (struct sockaddr*)&server_addr, sizeof(server_addr));
    if (ret < 0) {
        ESP_LOGE(TAG, "Failed to connect to %s:%d", host.c_str(), port);
        close(udp_fd_);
        udp_fd_ = -1;
        return false;
    }

    connected_ = true;
    receive_thread_ = std::thread(&EspUdp::ReceiveTask, this);
    return true;
}

void EspUdp::Disconnect() {
    if (udp_fd_ != -1) {
        close(udp_fd_);
        udp_fd_ = -1;
    }
    connected_ = false;
    if (receive_thread_.joinable()) {
        receive_thread_.join();
    }
}

int EspUdp::Send(const std::string& data) {
    int ret = send(udp_fd_, data.data(), data.size(), 0);
    if (ret <= 0) {
        connected_ = false;
        ESP_LOGE(TAG, "Send failed: ret=%d", ret);
    }
    return ret;
}

void EspUdp::ReceiveTask() {
    while (true) {
        std::string data;
        data.resize(1500);
        int ret = recv(udp_fd_, data.data(), data.size(), 0);
        if (ret <= 0) {
            break;
        }
        data.resize(ret);
        if (message_callback_) {
            message_callback_(data);
        }
    }
}
