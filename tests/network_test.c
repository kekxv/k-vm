/* Exercise the host Ethernet adapter, including its SLIRP output callbacks. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "cutils.h"
#include "virtio.h"
#include "host_network.h"

static uint8_t reply[2048];
static int reply_len;

static int can_receive(EthernetDevice *net)
{
    (void)net;
    return 1;
}

static void receive_packet(EthernetDevice *net, const uint8_t *packet, int len)
{
    (void)net;
    if (len > (int)sizeof(reply))
        return;
    memcpy(reply, packet, len);
    reply_len = len;
}

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "Failed at line %d: %s\n", __LINE__, #condition); \
        return 1; \
    } \
} while (0)

int main(void)
{
    /* Ethernet + ARP: who has 10.0.2.2? Tell 10.0.2.15. */
    const uint8_t request[] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0x02, 0, 0, 0, 0, 1, 0x08, 0x06,
        0, 1, 0x08, 0, 6, 4, 0, 1,
        0x02, 0, 0, 0, 0, 1, 10, 0, 2, 15,
        0, 0, 0, 0, 0, 0, 10, 0, 2, 2,
    };
    const uint8_t guest_mac[] = {0x02, 0, 0, 0, 0, 1};
    const uint8_t gateway_ip[] = {10, 0, 2, 2};
    const uint8_t guest_ip[] = {10, 0, 2, 15};
    EthernetDevice *net = slirp_open();
    CHECK(net != NULL);
    net->device_can_write_packet = can_receive;
    net->device_write_packet = receive_packet;
    net->write_packet(net, request, sizeof(request));
    CHECK(reply_len >= 42);
    CHECK(memcmp(reply, guest_mac, 6) == 0);
    CHECK(reply[12] == 0x08 && reply[13] == 0x06);
    CHECK(reply[20] == 0 && reply[21] == 2); /* ARP reply */
    CHECK(memcmp(reply + 28, gateway_ip, 4) == 0);
    CHECK(memcmp(reply + 32, guest_mac, 6) == 0);
    CHECK(memcmp(reply + 38, guest_ip, 4) == 0);
    CHECK(net->select_fill != NULL && net->select_poll != NULL);
    puts("SLIRP gateway ARP reply received through host Ethernet callbacks");
    return 0;
}
