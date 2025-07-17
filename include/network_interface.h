#ifndef NETWORK_INTERFACE_H
#define NETWORK_INTERFACE_H

#include "http.h"
#include "tcp.h"
#include "udp.h"
#include "mqtt.h"
#include "web_socket.h"

class NetworkInterface {
public:
    virtual ~NetworkInterface() = default;

    // 连接创建接口（纯虚函数，由子类实现）
    virtual Http* CreateHttp(int connect_id) = 0;
    virtual Tcp* CreateTcp(int connect_id) = 0;
    virtual Tcp* CreateSsl(int connect_id) = 0;
    virtual Udp* CreateUdp(int connect_id) = 0;
    virtual Mqtt* CreateMqtt(int connect_id) = 0;
    virtual WebSocket* CreateWebSocket(int connect_id) = 0;
};

#endif // NETWORK_INTERFACE_H