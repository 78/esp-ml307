#ifndef _ESP_TCP_H_
#define _ESP_TCP_H_

#include "tcp.h"

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>

#define ESP_TCP_EVENT_RECEIVE_TASK_EXIT 1

// Same default as esp-tls (ESP_TLS_DEFAULT_CONN_TIMEOUT) so http:// and
// https:// fail within the same time when the data path is down.
#define ESP_TCP_CONNECT_TIMEOUT_MS 10000
#define ESP_TCP_SEND_TIMEOUT_MS 10000
// lwIP can lose the wake-up of a blocked recv() when the socket is shut down
// concurrently, so the receive task re-checks connected_ at this interval.
#define ESP_TCP_RECEIVE_POLL_MS 1000

class EspTcp : public Tcp {
public:
    EspTcp();
    ~EspTcp();

    NetworkResult<> Connect(const std::string& host, int port) override;
    void Disconnect() override;
    int Send(const std::string& data) override;

private:
    // Owned by the caller side: only Disconnect() closes it, after the receive
    // task has exited, so the fd number cannot be closed twice.
    int tcp_fd_ = -1;
    EventGroupHandle_t event_group_ = nullptr;
    TaskHandle_t receive_task_handle_ = nullptr;

    void ReceiveTask();
};

#endif // _ESP_TCP_H_
