#ifndef _ESP_SSL_H_
#define _ESP_SSL_H_

#include "tcp.h"
#include <esp_tls.h>

#include <mutex>

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>

#define ESP_SSL_EVENT_RECEIVE_TASK_EXIT 1

// Maximum time Send() waits without making progress
#define ESP_SSL_SEND_TIMEOUT_MS 10000

class EspSsl : public Tcp {
public:
    EspSsl();
    ~EspSsl();

    NetworkResult<> Connect(const std::string& host, int port) override;
    void Disconnect() override;
    int Send(const std::string& data) override;

private:
    esp_tls_t* tls_client_ = nullptr;
    // Socket of tls_client_, set before the receive task starts and cleared
    // after it exits. esp_tls_conn_destroy() closes it.
    int sockfd_ = -1;
    // The mbedTLS context is not thread-safe: ReceiveTask() and Send() must not
    // run esp_tls_conn_read()/esp_tls_conn_write() at the same time (TLS 1.3
    // post-handshake messages move both through the handshake state machine).
    // The socket is non-blocking after the handshake, so the lock is only held
    // for the TLS call and waiting happens in select() outside of it.
    std::mutex tls_mutex_;
    EventGroupHandle_t event_group_ = nullptr;
    TaskHandle_t receive_task_handle_ = nullptr;

    void ReceiveTask();
};

#endif // _ESP_SSL_H_
