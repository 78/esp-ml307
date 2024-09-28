#include "Ml307SslTransport.h"
#include "esp_log.h"
#include <cstring>

static const char *TAG = "Ml307SslTransport";


Ml307SslTransport::Ml307SslTransport(Ml307AtModem& modem, int tcp_id) : modem_(modem), tcp_id_(tcp_id) {
    event_group_handle_ = xEventGroupCreate();

    command_callback_it_ = modem_.RegisterCommandResponseCallback([this](const std::string& command, const std::vector<AtArgumentValue>& arguments) {
        if (command == "MIPOPEN" && arguments.size() == 2) {
            if (arguments[0].int_value == tcp_id_) {
                if (arguments[1].int_value == 0) {
                    connected_ = true;
                    xEventGroupClearBits(event_group_handle_, ML307_SSL_TRANSPORT_DISCONNECTED | ML307_SSL_TRANSPORT_ERROR);
                    xEventGroupSetBits(event_group_handle_, ML307_SSL_TRANSPORT_CONNECTED);
                } else {
                    connected_ = false;
                    xEventGroupSetBits(event_group_handle_, ML307_SSL_TRANSPORT_ERROR);
                }
            }
        } else if (command == "MIPCLOSE" && arguments.size() == 1) {
            if (arguments[0].int_value == tcp_id_) {
                connected_ = false;
                xEventGroupSetBits(event_group_handle_, ML307_SSL_TRANSPORT_DISCONNECTED);
            }
        } else if (command == "MIPSEND" && arguments.size() == 2) {
            if (arguments[0].int_value == tcp_id_) {
                xEventGroupSetBits(event_group_handle_, ML307_SSL_TRANSPORT_SEND_COMPLETE);
            }
        } else if (command == "MIPURC" && arguments.size() == 4) {
            if (arguments[1].int_value == tcp_id_) {
                if (arguments[0].string_value == "rtcp") {
                    std::lock_guard<std::mutex> lock(mutex_);
                    rx_buffer_.append(modem_.DecodeHex(arguments[3].string_value));
                    xEventGroupSetBits(event_group_handle_, ML307_SSL_TRANSPORT_RECEIVE);
                } else if (arguments[0].string_value == "disconn") {
                    connected_ = false;
                    xEventGroupSetBits(event_group_handle_, ML307_SSL_TRANSPORT_DISCONNECTED);
                } else {
                    ESP_LOGE(TAG, "Unknown MIPURC command: %s", arguments[0].string_value.c_str());
                }
            }
        } else if (command == "MIPSTATE" && arguments.size() == 5) {
            if (arguments[0].int_value == tcp_id_) {
                if (arguments[4].string_value == "INITIAL") {
                    connected_ = false;
                } else {
                    connected_ = true;
                }
                xEventGroupSetBits(event_group_handle_, ML307_SSL_TRANSPORT_INITIALIZED);
            }
        } else if (command == "FIFO_OVERFLOW") {
            xEventGroupSetBits(event_group_handle_, ML307_SSL_TRANSPORT_ERROR);
            Disconnect();
        }
    });
}

Ml307SslTransport::~Ml307SslTransport() {
    modem_.UnregisterCommandResponseCallback(command_callback_it_);
}

bool Ml307SslTransport::Connect(const char* host, int port) {
    char command[64];

    // 检查这个 id 是否已经连接
    sprintf(command, "AT+MIPSTATE=%d", tcp_id_);
    modem_.Command(command);
    auto bits = xEventGroupWaitBits(event_group_handle_, ML307_SSL_TRANSPORT_INITIALIZED, pdTRUE, pdFALSE, pdMS_TO_TICKS(SSL_CONNECT_TIMEOUT_MS));
    if (!(bits & ML307_SSL_TRANSPORT_INITIALIZED)) {
        ESP_LOGE(TAG, "Failed to initialize TCP connection");
        return false;
    }

    // 断开之前的连接
    if (connected_) {
        Disconnect();
    }

    // 设置 SSL 配置
    sprintf(command, "AT+MSSLCFG=\"auth\",0,0");
    if (!modem_.Command(command)) {
        ESP_LOGE(TAG, "Failed to set SSL configuration");
        return false;
    }

    sprintf(command, "AT+MIPCFG=\"ssl\",%d,1,0", tcp_id_);
    if (!modem_.Command(command)) {
        ESP_LOGE(TAG, "Failed to set TCP SSL configuration");
        return false;
    }

    // 打开 TCP 连接
    sprintf(command, "AT+MIPOPEN=%d,\"TCP\",\"%s\",%d,,0", tcp_id_, host, port);
    if (!modem_.Command(command)) {
        ESP_LOGE(TAG, "Failed to open TCP connection");
        return false;
    }

    // 使用 HEX 编码
    sprintf(command, "AT+MIPCFG=\"encoding\",%d,1,1", tcp_id_);
    if (!modem_.Command(command)) {
        ESP_LOGE(TAG, "Failed to set HEX encoding");
        return false;
    }

    // 等待连接完成
    bits = xEventGroupWaitBits(event_group_handle_, ML307_SSL_TRANSPORT_CONNECTED | ML307_SSL_TRANSPORT_ERROR, pdTRUE, pdFALSE, SSL_CONNECT_TIMEOUT_MS / portTICK_PERIOD_MS);
    if (bits & ML307_SSL_TRANSPORT_ERROR) {
        ESP_LOGE(TAG, "Failed to connect to %s:%d", host, port);
        return false;
    }
    return true;
}

void Ml307SslTransport::Disconnect() {
    if (!connected_) {
        return;
    }
    connected_ = false;
    std::string command = "AT+MIPCLOSE=" + std::to_string(tcp_id_);
    modem_.Command(command);
}

int Ml307SslTransport::Send(const char* data, size_t length) {
    std::string command = "AT+MIPSEND=" + std::to_string(tcp_id_) + "," + std::to_string(length) + "," + modem_.EncodeHex(std::string(data, length));
    if (!modem_.Command(command)) {
        ESP_LOGE(TAG, "Failed to send data");
        return -1;
    }

    auto bits = xEventGroupWaitBits(event_group_handle_, ML307_SSL_TRANSPORT_SEND_COMPLETE, pdTRUE, pdFALSE, pdMS_TO_TICKS(SSL_CONNECT_TIMEOUT_MS));
    if (bits & ML307_SSL_TRANSPORT_SEND_COMPLETE) {
        return length;
    }
    return -1;
}

int Ml307SslTransport::Receive(char* buffer, size_t bufferSize) {
    while (rx_buffer_.empty()) {
        auto bits = xEventGroupWaitBits(event_group_handle_, ML307_SSL_TRANSPORT_RECEIVE | ML307_SSL_TRANSPORT_DISCONNECTED, pdTRUE, pdFALSE, portMAX_DELAY);
        if (bits & ML307_SSL_TRANSPORT_DISCONNECTED) {
            return 0;
        }
        if (!(bits & ML307_SSL_TRANSPORT_RECEIVE)) {
            ESP_LOGE(TAG, "Failed to receive data");
            return -1;
        }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    size_t length = std::min(bufferSize, rx_buffer_.size());
    memcpy(buffer, rx_buffer_.data(), length);
    rx_buffer_.erase(0, length);
    return length;
}
