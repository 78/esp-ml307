#ifndef _ESP_SSL_H_
#define _ESP_SSL_H_

#include "tcp.h"
#include <esp_tls.h>
#include <thread>

class EspSsl : public Tcp {
public:
    EspSsl();
    ~EspSsl();

    bool Connect(const std::string& host, int port) override;
    void Disconnect() override;
    int Send(const std::string& data) override;

private:
    esp_tls_t* tls_client_ = nullptr;
    std::thread receive_thread_;

    void ReceiveTask();
};

#endif // _ESP_SSL_H_