#ifndef ESP_UDP_H
#define ESP_UDP_H

#include "udp.h"

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <thread>

class EspUdp : public Udp {
public:
    EspUdp();
    ~EspUdp();

    bool Connect(const std::string& host, int port) override;
    void Disconnect() override;
    int Send(const std::string& data) override;

private:
    int udp_fd_;
    std::thread receive_thread_;

    void ReceiveTask();
};

#endif // ESP_UDP_H
