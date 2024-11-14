#include "ml307_udp.h"

#include <esp_log.h>

#define TAG "Ml307Udp"


Ml307Udp::Ml307Udp(Ml307AtModem& modem, int udp_id) : modem_(modem), udp_id_(udp_id) {
    event_group_handle_ = xEventGroupCreate();

    command_callback_it_ = modem_.RegisterCommandResponseCallback([this](const std::string& command, const std::vector<AtArgumentValue>& arguments) {
        if (command == "MIPOPEN" && arguments.size() == 2) {
            if (arguments[0].int_value == udp_id_) {
                if (arguments[1].int_value == 0) {
                    connected_ = true;
                    xEventGroupClearBits(event_group_handle_, ML307_UDP_DISCONNECTED | ML307_UDP_ERROR);
                    xEventGroupSetBits(event_group_handle_, ML307_UDP_CONNECTED);
                } else {
                    connected_ = false;
                    xEventGroupSetBits(event_group_handle_, ML307_UDP_ERROR);
                }
            }
        } else if (command == "MIPCLOSE" && arguments.size() == 1) {
            if (arguments[0].int_value == udp_id_) {
                connected_ = false;
                xEventGroupSetBits(event_group_handle_, ML307_UDP_DISCONNECTED);
            }
        } else if (command == "MIPSEND" && arguments.size() == 2) {
            if (arguments[0].int_value == udp_id_) {
                xEventGroupSetBits(event_group_handle_, ML307_UDP_SEND_COMPLETE);
            }
        } else if (command == "MIPURC" && arguments.size() == 4) {
            if (arguments[1].int_value == udp_id_) {
                if (arguments[0].string_value == "rudp") {
                    if (message_callback_) {
                        message_callback_(modem_.DecodeHex(arguments[3].string_value));
                    }
                } else if (arguments[0].string_value == "disconn") {
                    connected_ = false;
                    xEventGroupSetBits(event_group_handle_, ML307_UDP_DISCONNECTED);
                } else {
                    ESP_LOGE(TAG, "Unknown MIPURC command: %s", arguments[0].string_value.c_str());
                }
            }
        } else if (command == "MIPSTATE" && arguments.size() == 5) {
            if (arguments[0].int_value == udp_id_) {
                if (arguments[4].string_value == "INITIAL") {
                    connected_ = false;
                } else {
                    connected_ = true;
                }
                xEventGroupSetBits(event_group_handle_, ML307_UDP_INITIALIZED);
            }
        } else if (command == "FIFO_OVERFLOW") {
            xEventGroupSetBits(event_group_handle_, ML307_UDP_ERROR);
            Disconnect();
        }
    });
}

Ml307Udp::~Ml307Udp() {
    Disconnect();
    modem_.UnregisterCommandResponseCallback(command_callback_it_);
}

bool Ml307Udp::Connect(const std::string& host, int port) {
    char command[64];

    // Clear bits
    xEventGroupClearBits(event_group_handle_, ML307_UDP_CONNECTED | ML307_UDP_DISCONNECTED | ML307_UDP_ERROR);

    // 检查这个 id 是否已经连接
    sprintf(command, "AT+MIPSTATE=%d", udp_id_);
    modem_.Command(command);
    auto bits = xEventGroupWaitBits(event_group_handle_, ML307_UDP_INITIALIZED, pdTRUE, pdFALSE, pdMS_TO_TICKS(UDP_CONNECT_TIMEOUT_MS));
    if (!(bits & ML307_UDP_INITIALIZED)) {
        ESP_LOGE(TAG, "Failed to initialize TCP connection");
        return false;
    }

    // 断开之前的连接
    if (connected_) {
        Disconnect();
    }

    // 打开 TCP 连接
    sprintf(command, "AT+MIPOPEN=%d,\"UDP\",\"%s\",%d,,0", udp_id_, host.c_str(), port);
    if (!modem_.Command(command)) {
        ESP_LOGE(TAG, "Failed to open UDP connection");
        return false;
    }

    // 使用 HEX 编码
    sprintf(command, "AT+MIPCFG=\"encoding\",%d,1,1", udp_id_);
    if (!modem_.Command(command)) {
        ESP_LOGE(TAG, "Failed to set HEX encoding");
        return false;
    }

    // 等待连接完成
    bits = xEventGroupWaitBits(event_group_handle_, ML307_UDP_CONNECTED | ML307_UDP_ERROR, pdTRUE, pdFALSE, UDP_CONNECT_TIMEOUT_MS / portTICK_PERIOD_MS);
    if (bits & ML307_UDP_ERROR) {
        ESP_LOGE(TAG, "Failed to connect to %s:%d", host.c_str(), port);
        return false;
    }
    return true;
}


void Ml307Udp::Disconnect() {
    if (!connected_) {
        return;
    }
    connected_ = false;
    modem_.Command("AT+MIPCLOSE=" + std::to_string(udp_id_));
}

int Ml307Udp::Send(const std::string& data) {
    const size_t MAX_PACKET_SIZE = 1460 / 2;

    if (!connected_) {
        ESP_LOGE(TAG, "未连接");
        return -1;
    }

    if (data.size() > MAX_PACKET_SIZE) {
        ESP_LOGE(TAG, "数据块超过最大限制");
        return -1;
    }

    // 在循环外预先分配command
    std::string command = "AT+MIPSEND=" + std::to_string(udp_id_) + "," + std::to_string(data.size()) + ",";
    
    // 直接在command字符串上进行十六进制编码
    modem_.EncodeHexAppend(command, data.c_str(), data.size());
    
    if (!modem_.Command(command, 100)) {
        ESP_LOGE(TAG, "发送数据块失败");
        return -1;
    }
    return data.size();
}
