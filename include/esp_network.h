#ifndef ESP_NETWORK_H
#define ESP_NETWORK_H

#include "network_interface.h"

class EspNetwork : public NetworkInterface {
public:
    EspNetwork();
    ~EspNetwork();

    Http* CreateHttp(int connect_id) override;
    Tcp* CreateTcp(int connect_id) override;
    Tcp* CreateSsl(int connect_id) override;
    Udp* CreateUdp(int connect_id) override;
    Mqtt* CreateMqtt(int connect_id) override;
    WebSocket* CreateWebSocket(int connect_id) override;
};

#endif // ESP_NETWORK_H
