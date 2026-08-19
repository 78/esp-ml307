#include "esp_ssl.h"
#include <esp_log.h>
#include <esp_crt_bundle.h>
#include <cstring>
#include <unistd.h>
#include <errno.h>
#include <sys/select.h>

static const char *TAG = "EspSsl";

EspSsl::EspSsl() {
    event_group_ = xEventGroupCreate();
}

EspSsl::~EspSsl() {
    Disconnect();

    if (event_group_ != nullptr) {
        vEventGroupDelete(event_group_);
        event_group_ = nullptr;
    }
}

bool EspSsl::Connect(const std::string& host, int port) {
    if (tls_client_ != nullptr) {
        ESP_LOGE(TAG, "tls client has been initialized");
        return false;
    }

    tls_client_ = esp_tls_init();
    if (tls_client_ == nullptr) {
        ESP_LOGE(TAG, "Failed to initialize TLS");
        return false;
    }

    esp_tls_cfg_t cfg = {};
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

    // Non-blocking TLS connect with timeout to avoid blocking the main loop
    // when the server is unreachable.
    TickType_t start_ticks = xTaskGetTickCount();
    int ret;
    while (true) {
        ret = esp_tls_conn_new_async(host.c_str(), host.length(), port, &cfg, tls_client_);
        if (ret == 1) {
            break;  // Connected successfully
        } else if (ret < 0) {
            break;  // Connection failed
        }

        // ret == 0: connection still in progress — poll the socket with timeout
        int sockfd = -1;
        esp_tls_get_conn_sockfd(tls_client_, &sockfd);
        if (sockfd < 0) {
            ESP_LOGE(TAG, "Failed to get socket fd during connect");
            ret = -1;
            break;
        }

        TickType_t elapsed_ms = (xTaskGetTickCount() - start_ticks) * portTICK_PERIOD_MS;
        if (elapsed_ms >= ESP_SSL_CONNECT_TIMEOUT_MS) {
            ESP_LOGE(TAG, "Connect timeout to %s:%d", host.c_str(), port);
            last_error_ = ETIMEDOUT;
            esp_tls_conn_destroy(tls_client_);
            tls_client_ = nullptr;
            return false;
        }

        uint32_t remaining_ms = ESP_SSL_CONNECT_TIMEOUT_MS - elapsed_ms;
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(sockfd, &wfds);
        struct timeval tv;
        tv.tv_sec = remaining_ms / 1000;
        tv.tv_usec = (remaining_ms % 1000) * 1000;
        int sel = select(sockfd + 1, NULL, &wfds, NULL, &tv);
        if (sel < 0) {
            ESP_LOGE(TAG, "select() failed during connect: errno=%d", errno);
            ret = -1;
            break;
        }
        // If sel == 0 (timeout on select), loop will check overall timeout next iteration
    }

    if (ret != 1) {
        esp_tls_error_handle_t last_error;
        if (esp_tls_get_error_handle(tls_client_, &last_error) == ESP_OK) {
            int error_code, error_flags;
            esp_err_t err = esp_tls_get_and_clear_last_error(last_error, &error_code, &error_flags);
            last_error_ = err;
            ESP_LOGE(TAG, "Failed to connect to %s:%d, code=0x%x", host.c_str(), port, err);
        } else {
            last_error_ = -1;
            ESP_LOGE(TAG, "Failed to get error handle");
        }
        esp_tls_conn_destroy(tls_client_);
        tls_client_ = nullptr;
        return false;
    }

    connected_ = true;

    xEventGroupClearBits(event_group_, ESP_SSL_EVENT_RECEIVE_TASK_EXIT);
    xTaskCreate([](void* arg) {
        EspSsl* ssl = (EspSsl*)arg;
        ssl->ReceiveTask();
        xEventGroupSetBits(ssl->event_group_, ESP_SSL_EVENT_RECEIVE_TASK_EXIT);
        vTaskDelete(NULL);
    }, "ssl_receive", 4096, this, 1, &receive_task_handle_);
    return true;
}

void EspSsl::Disconnect() {
    connected_ = false;
    
    // Close socket if it is open
    if (tls_client_ != nullptr) {
        int sockfd;
        ESP_ERROR_CHECK(esp_tls_get_conn_sockfd(tls_client_, &sockfd));
        if (sockfd >= 0) {
            close(sockfd);
        }
    
        auto bits = xEventGroupWaitBits(event_group_, ESP_SSL_EVENT_RECEIVE_TASK_EXIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(10000));
        if (!(bits & ESP_SSL_EVENT_RECEIVE_TASK_EXIT)) {
            ESP_LOGE(TAG, "Failed to wait for receive task exit");
        }

        esp_tls_conn_destroy(tls_client_);
        tls_client_ = nullptr;
    }
}

/* CONFIG_MBEDTLS_SSL_RENEGOTIATION should be disabled in sdkconfig.
 * Otherwise, invalid memory access may be triggered.
 */
int EspSsl::Send(const std::string& data) {
    if (!connected_) {
        ESP_LOGE(TAG, "Not connected");
        return -1;
    }

    size_t total_sent = 0;
    size_t data_size = data.size();
    const char* data_ptr = data.data();
    
    while (total_sent < data_size) {
        int ret = esp_tls_conn_write(tls_client_, data_ptr + total_sent, data_size - total_sent);

        if (ret == ESP_TLS_ERR_SSL_WANT_WRITE) {
            continue;
        }

        if (ret <= 0) {
            ESP_LOGE(TAG, "SSL send failed: ret=%d, errno=%d", ret, errno);
            return ret;
        }
        
        total_sent += ret;
    }
    
    return total_sent;
}

void EspSsl::ReceiveTask() {
    std::string data;
    while (connected_) {
        data.resize(1500);
        int ret = esp_tls_conn_read(tls_client_, data.data(), data.size());

        if (ret == ESP_TLS_ERR_SSL_WANT_READ) {
            continue;
        }

        if (ret <= 0) {
            if (ret < 0) {
                ESP_LOGE(TAG, "SSL receive failed: %d", ret);
            }
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

int EspSsl::GetLastError() {
    return last_error_;
}
