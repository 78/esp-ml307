#include "esp_tcp.h"

#include <esp_log.h>
#include <unistd.h>
#include <cstring>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netdb.h>
#include <errno.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "EspTcp";

EspTcp::EspTcp() : tcp_fd_(-1) {
}

EspTcp::~EspTcp() {
    Disconnect();
}

bool EspTcp::Connect(const std::string& host, int port) {
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

    tcp_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_fd_ < 0) {
        ESP_LOGE(TAG, "Failed to create socket");
        return false;
    }

    int ret = connect(tcp_fd_, (struct sockaddr*)&server_addr, sizeof(server_addr));
    if (ret < 0) {
        ESP_LOGE(TAG, "Failed to connect to %s:%d", host.c_str(), port);
        close(tcp_fd_);
        tcp_fd_ = -1;
        return false;
    }

    connected_ = true;
    receive_thread_ = std::thread(&EspTcp::ReceiveTask, this);
    return true;
}

void EspTcp::Disconnect() {
    if (tcp_fd_ != -1) {
        close(tcp_fd_);
        tcp_fd_ = -1;
    }

    connected_ = false;

    if (receive_thread_.joinable()) {
        receive_thread_.join();
    }
}

int EspTcp::Send(const std::string& data) {
    size_t total_sent = 0;
    size_t data_size = data.size();
    const char* data_ptr = data.data();
    int retry_count = 0;
    const int max_retries = 1000; // 最大重试次数，避免无限循环
    
    while (total_sent < data_size && retry_count < max_retries) {
        int ret = send(tcp_fd_, data_ptr + total_sent, data_size - total_sent, 0);
        
        if (ret > 0) {
            total_sent += ret;
            retry_count = 0; // 成功发送后重置重试计数
            continue;
        }
        
        if (ret == 0) {
            // 连接已关闭
            connected_ = false;
            ESP_LOGE(TAG, "Connection closed during send");
            if (disconnect_callback_) {
                disconnect_callback_();
            }
            return total_sent;
        }
        
        // ret < 0, 检查错误类型
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // 暂时性错误，短暂等待后重试
            retry_count++;
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        
        // 其他错误，认为连接失败
        connected_ = false;
        ESP_LOGE(TAG, "Send failed: ret=%d, errno=%d", ret, errno);
        if (disconnect_callback_) {
            disconnect_callback_();
        }
        return total_sent;
    }
    
    if (retry_count >= max_retries) {
        ESP_LOGW(TAG, "Exceed max retries");
    }
    
    return total_sent;
}

void EspTcp::ReceiveTask() {
    std::string data;
    while (connected_) {
        data.resize(1500);
        int ret = recv(tcp_fd_, data.data(), data.size(), 0);
        if (ret <= 0) {
            connected_ = false;
            // 接收失败或连接断开时调用断连回调
            if (disconnect_callback_) {
                disconnect_callback_();
            }
            break;
        }

        if (stream_callback_) {
            data.resize(ret);
            stream_callback_(data);
        }
    }
}



