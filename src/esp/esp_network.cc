#include "esp_network.h"

#include "esp_tcp.h"
#include "esp_ssl.h"
#include "esp_udp.h"
#include "esp_mqtt.h"
#include "http_client.h"
#include "web_socket.h"


EspNetwork::EspNetwork() {

}

EspNetwork::~EspNetwork() {

}

Http* EspNetwork::CreateHttp(int connect_id) {
    return new HttpClient(this, connect_id);
}

Tcp* EspNetwork::CreateTcp(int connect_id) {
    return new EspTcp();
}

Tcp* EspNetwork::CreateSsl(int connect_id) {
    return new EspSsl();
}

Udp* EspNetwork::CreateUdp(int connect_id) {
    return new EspUdp();
}

Mqtt* EspNetwork::CreateMqtt(int connect_id) {
    return new EspMqtt();
}

WebSocket* EspNetwork::CreateWebSocket(int connect_id) {
    return new WebSocket(this, connect_id);
}