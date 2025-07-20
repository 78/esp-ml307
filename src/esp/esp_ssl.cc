#include "esp_ssl.h"
#include <esp_log.h>
#include <esp_crt_bundle.h>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "EspSsl";

EspSsl::EspSsl() {
    tls_client_ = esp_tls_init();
}

EspSsl::~EspSsl() {
    Disconnect();
}

bool EspSsl::Connect(const std::string& host, int port) {
    // 确保先断开已有连接
    if (connected_) {
        Disconnect();
    }
    
    if (!tls_client_) {
        tls_client_ = esp_tls_init();
        if (!tls_client_) {
            ESP_LOGE(TAG, "Failed to initialize TLS");
            return false;
        }
    }

    esp_tls_cfg_t cfg = {};
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

    int ret = esp_tls_conn_new_sync(host.c_str(), host.length(), port, &cfg, tls_client_);
    if (ret != 1) {
        ESP_LOGE(TAG, "Failed to connect to %s:%d", host.c_str(), port);
        return false;
    }

    connected_ = true;
    receive_thread_ = std::thread(&EspSsl::ReceiveTask, this);
    return true;
}

void EspSsl::Disconnect() {
    connected_ = false;
    
    if (receive_thread_.joinable()) {
        receive_thread_.join();
    }
    
    if (tls_client_) {
        esp_tls_conn_destroy(tls_client_);
        tls_client_ = nullptr;
    }
}

int EspSsl::Send(const std::string& data) {
    size_t total_sent = 0;
    size_t data_size = data.size();
    const char* data_ptr = data.data();
    int retry_count = 0;
    const int max_retries = 100; // 最大重试次数，避免无限循环
    
    while (total_sent < data_size && retry_count < max_retries) {
        int ret = esp_tls_conn_write(tls_client_, data_ptr + total_sent, data_size - total_sent);
        
        if (ret > 0) {
            total_sent += ret;
            retry_count = 0; // 成功发送后重置重试计数
            continue;
        }
        
        if (ret == ESP_TLS_ERR_SSL_WANT_WRITE) {
            // TLS 需要更多时间处理，短暂等待后重试
            retry_count++;
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        
        // 其他错误，认为连接失败
        connected_ = false;
        ESP_LOGE(TAG, "SSL send failed: %d", ret);
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

void EspSsl::ReceiveTask() {
    std::string data;
    while (connected_) {
        data.resize(1500);
        int ret = esp_tls_conn_read(tls_client_, data.data(), data.size());
        if (ret == ESP_TLS_ERR_SSL_WANT_READ) {
            // 需要更多数据，继续等待
            continue;
        }
        
        if (ret < 0) {
            connected_ = false;
            ESP_LOGE(TAG, "SSL receive failed: %d", ret);
            // 接收失败时调用断连回调
            if (disconnect_callback_) {
                disconnect_callback_();
            }
            break;
        } else if (ret == 0) {
            // 连接正常关闭
            connected_ = false;
            break;
        }
        
        if (stream_callback_) {
            data.resize(ret);
            stream_callback_(data);
        }
    }
}
