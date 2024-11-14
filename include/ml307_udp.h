#ifndef ML307_UDP_H
#define ML307_UDP_H

#include "udp.h"
#include "ml307_at_modem.h"

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

#define ML307_UDP_CONNECTED BIT0
#define ML307_UDP_DISCONNECTED BIT1
#define ML307_UDP_ERROR BIT2
#define ML307_UDP_RECEIVE BIT3
#define ML307_UDP_SEND_COMPLETE BIT4
#define ML307_UDP_INITIALIZED BIT5

#define UDP_CONNECT_TIMEOUT_MS 10000

class Ml307Udp : public Udp {
public:
    Ml307Udp(Ml307AtModem& modem, int udp_id);
    ~Ml307Udp();

    bool Connect(const std::string& host, int port) override;
    void Disconnect() override;
    int Send(const std::string& data) override;

private:
    Ml307AtModem& modem_;
    int udp_id_;
    EventGroupHandle_t event_group_handle_;
    std::list<CommandResponseCallback>::iterator command_callback_it_;
};

#endif // ML307_UDP_H
