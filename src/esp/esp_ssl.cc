#include "esp_ssl.h"
#include <esp_log.h>
#include <esp_crt_bundle.h>
#include <cstring>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/socket.h>

static const char *TAG = "EspSsl";

// Wait until fd is readable and/or writable. A negative timeout waits forever.
static int WaitSocket(int fd, bool for_read, bool for_write, int timeout_ms) {
    fd_set read_fds;
    fd_set write_fds;
    FD_ZERO(&read_fds);
    FD_ZERO(&write_fds);
    if (for_read) {
        FD_SET(fd, &read_fds);
    }
    if (for_write) {
        FD_SET(fd, &write_fds);
    }
    struct timeval tv = {};
    if (timeout_ms >= 0) {
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
    }
    return select(fd + 1, for_read ? &read_fds : nullptr, for_write ? &write_fds : nullptr, nullptr,
                  timeout_ms >= 0 ? &tv : nullptr);
}

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

NetworkResult<> EspSsl::Connect(const std::string& host, int port) {
    // Also releases the TLS context and joins the receive task left by a
    // passive disconnect, so the object can be reconnected.
    Disconnect();

    tls_client_ = esp_tls_init();
    if (tls_client_ == nullptr) {
        ESP_LOGE(TAG, "Failed to initialize TLS");
        return Fail(NetworkError::TlsFailed());
    }

    auto destroy_tls = [this]() {
        esp_tls_conn_destroy(tls_client_);
        tls_client_ = nullptr;
        sockfd_ = -1;
    };

    esp_tls_cfg_t cfg = {};
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

    int ret = esp_tls_conn_new_sync(host.c_str(), host.length(), port, &cfg, tls_client_);
    if (ret != 1) {
        NetworkError err = NetworkError::TlsFailed();
        esp_tls_error_handle_t tls_error;
        if (esp_tls_get_error_handle(tls_client_, &tls_error) == ESP_OK) {
            int error_code, error_flags;
            esp_err_t esp_err = esp_tls_get_and_clear_last_error(tls_error, &error_code, &error_flags);
            err = NetworkError::FromEsp(esp_err);
            ESP_LOGE(TAG, "Failed to connect to %s:%d: %s", host.c_str(), port, err.ToString().c_str());
        } else {
            ESP_LOGE(TAG, "Failed to get error handle");
        }
        destroy_tls();
        return Fail(err);
    }

    int sockfd = -1;
    if (esp_tls_get_conn_sockfd(tls_client_, &sockfd) != ESP_OK || sockfd < 0) {
        ESP_LOGE(TAG, "Failed to get socket fd");
        destroy_tls();
        return Fail(NetworkError::TlsFailed());
    }
    int flags = fcntl(sockfd, F_GETFL, 0);
    if (flags < 0 || fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) < 0) {
        int err = errno;
        ESP_LOGE(TAG, "Failed to set socket non-blocking: errno=%d", err);
        destroy_tls();
        return Fail(NetworkError::FromErrno(err));
    }
    sockfd_ = sockfd;

    connected_ = true;
    xEventGroupClearBits(event_group_, ESP_SSL_EVENT_RECEIVE_TASK_EXIT);
    BaseType_t rc = xTaskCreate([](void* arg) {
        EspSsl* ssl = (EspSsl*)arg;
        ssl->ReceiveTask();
        xEventGroupSetBits(ssl->event_group_, ESP_SSL_EVENT_RECEIVE_TASK_EXIT);
        vTaskDelete(NULL);
    }, "ssl_receive", 4096, this, 1, &receive_task_handle_);
    if (rc != pdPASS) {
        ESP_LOGE(TAG, "Failed to create receive task");
        connected_ = false;
        receive_task_handle_ = nullptr;
        destroy_tls();
        return Fail(NetworkError::FromErrno(ENOMEM));
    }
    return {};
}

void EspSsl::Disconnect() {
    bool was_connected = connected_.exchange(false);
    if (tls_client_ == nullptr) {
        return;
    }

    // Use shutdown(), not close(). close() hands the fd number back to lwIP
    // immediately, and esp_tls_conn_destroy() below closes the same number
    // again. shutdown() still wakes ReceiveTask, but keeps the fd so
    // esp_tls_conn_destroy() closes it exactly once.
    if (sockfd_ >= 0) {
        shutdown(sockfd_, SHUT_RDWR);
    }

    if (receive_task_handle_ != nullptr) {
        auto bits = xEventGroupWaitBits(event_group_, ESP_SSL_EVENT_RECEIVE_TASK_EXIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(10000));
        if (!(bits & ESP_SSL_EVENT_RECEIVE_TASK_EXIT)) {
            ESP_LOGE(TAG, "Failed to wait for receive task exit");
        }
        receive_task_handle_ = nullptr;
    }

    {
        // Waits for a TLS call in a concurrent Send() to return
        std::lock_guard<std::mutex> lock(tls_mutex_);
        esp_tls_conn_destroy(tls_client_);
        tls_client_ = nullptr;
        sockfd_ = -1;
    }

    if (was_connected && disconnect_callback_) {
        disconnect_callback_();
    }
}

int EspSsl::Send(const std::string& data) {
    if (!connected_) {
        ESP_LOGE(TAG, "Not connected");
        return -1;
    }

    size_t total_sent = 0;
    size_t data_size = data.size();
    const char* data_ptr = data.data();
    const TickType_t timeout_ticks = pdMS_TO_TICKS(ESP_SSL_SEND_TIMEOUT_MS);
    TickType_t last_progress = xTaskGetTickCount();

    while (total_sent < data_size) {
        int ret;
        int sockfd;
        {
            std::lock_guard<std::mutex> lock(tls_mutex_);
            if (tls_client_ == nullptr || !connected_) {
                ESP_LOGE(TAG, "Not connected");
                return -1;
            }
            // After WANT_WRITE mbedTLS requires the same arguments on retry,
            // which holds because total_sent only advances on success.
            ret = esp_tls_conn_write(tls_client_, data_ptr + total_sent, data_size - total_sent);
            sockfd = sockfd_;
        }

        if (ret > 0) {
            total_sent += ret;
            last_progress = xTaskGetTickCount();
            continue;
        }

        if (ret != ESP_TLS_ERR_SSL_WANT_WRITE && ret != ESP_TLS_ERR_SSL_WANT_READ) {
            ESP_LOGE(TAG, "SSL send failed: ret=%d, errno=%d", ret, errno);
            return ret;
        }

        TickType_t elapsed = xTaskGetTickCount() - last_progress;
        if (elapsed >= timeout_ticks) {
            ESP_LOGE(TAG, "SSL send timeout, sent %u of %u bytes", (unsigned)total_sent, (unsigned)data_size);
            return -1;
        }
        if (ret == ESP_TLS_ERR_SSL_WANT_WRITE) {
            WaitSocket(sockfd, false, true, (timeout_ticks - elapsed) * portTICK_PERIOD_MS);
        } else {
            // The record mbedTLS waits for is read by ReceiveTask, so poll
            // instead of competing with it in select().
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    return total_sent;
}

void EspSsl::ReceiveTask() {
    std::string data;
    bool want_write = false;
    while (connected_) {
        bool pending;
        {
            std::lock_guard<std::mutex> lock(tls_mutex_);
            pending = esp_tls_get_bytes_avail(tls_client_) > 0;
        }
        // Decrypted bytes may remain in mbedTLS when the record is larger than
        // the read buffer; the socket would not signal readability for them.
        if (!pending) {
            // Woken by incoming data, peer close or reset, or shutdown() in Disconnect()
            int ret = WaitSocket(sockfd_, true, want_write, -1);
            if (ret < 0 && errno != EINTR) {
                ESP_LOGE(TAG, "select failed: errno=%d", errno);
            }
            if (!connected_) {
                break;
            }
        }

        data.resize(1500);
        int ret;
        {
            std::lock_guard<std::mutex> lock(tls_mutex_);
            ret = esp_tls_conn_read(tls_client_, data.data(), data.size());
        }

        if (ret == ESP_TLS_ERR_SSL_WANT_READ || ret == ESP_TLS_ERR_SSL_WANT_WRITE) {
            want_write = ret == ESP_TLS_ERR_SSL_WANT_WRITE;
            continue;
        }
        want_write = false;

        if (ret <= 0) {
            // Passive disconnect: only report it, Disconnect() releases the TLS
            // context. exchange() keeps the callback to one call when
            // Disconnect() races.
            if (connected_.exchange(false)) {
                if (ret < 0) {
                    ESP_LOGE(TAG, "SSL receive failed: %d", ret);
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
