/*
 * TinyEMU
 * 
 * Copyright (c) 2016-2018 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#endif
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#ifndef _WIN32
#include <sys/ioctl.h>
#include <net/if.h>
#endif
#ifdef __linux__
#include <linux/if_tun.h>
#endif

#include "cutils.h"
#include "virtio.h"
#include "host_network.h"
#include "slirp/libslirp.h"

#ifdef __linux__

typedef struct {
    int fd;
    BOOL select_filled;
} TunState;

static void tun_write_packet(EthernetDevice *net,
                             const uint8_t *buf, int len)
{
    TunState *s = net->opaque;
    write(s->fd, buf, len);
}

static void tun_select_fill(EthernetDevice *net, int *pfd_max,
                            fd_set *rfds, fd_set *wfds, fd_set *efds,
                            int *pdelay)
{
    TunState *s = net->opaque;
    int net_fd = s->fd;

    s->select_filled = net->device_can_write_packet(net);
    if (s->select_filled) {
        FD_SET(net_fd, rfds);
        *pfd_max = max_int(*pfd_max, net_fd);
    }
}

static void tun_select_poll(EthernetDevice *net, 
                            fd_set *rfds, fd_set *wfds, fd_set *efds,
                            int select_ret)
{
    TunState *s = net->opaque;
    int net_fd = s->fd;
    uint8_t buf[2048];
    int ret;
    
    if (select_ret <= 0)
        return;
    if (s->select_filled && FD_ISSET(net_fd, rfds)) {
        ret = read(net_fd, buf, sizeof(buf));
        if (ret > 0)
            net->device_write_packet(net, buf, ret);
    }
    
}

/* configure with:
# bridge configuration (connect tap0 to bridge interface br0)
   ip link add br0 type bridge
   ip tuntap add dev tap0 mode tap [user x] [group x]
   ip link set tap0 master br0
   ip link set dev br0 up
   ip link set dev tap0 up

# NAT configuration (eth1 is the interface connected to internet)
   ifconfig br0 192.168.3.1
   echo 1 > /proc/sys/net/ipv4/ip_forward
   iptables -D FORWARD 1
   iptables -t nat -A POSTROUTING -o eth1 -j MASQUERADE

   In the VM:
   ifconfig eth0 192.168.3.2
   route add -net 0.0.0.0 netmask 0.0.0.0 gw 192.168.3.1
*/
EthernetDevice *tun_open(const char *ifname)
{
    struct ifreq ifr;
    int fd, ret;
    EthernetDevice *net;
    TunState *s;
    
    fd = open("/dev/net/tun", O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "Error: could not open /dev/net/tun\n");
        return NULL;
    }
    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
    pstrcpy(ifr.ifr_name, sizeof(ifr.ifr_name), ifname);
    ret = ioctl(fd, TUNSETIFF, (void *) &ifr);
    if (ret != 0) {
        fprintf(stderr, "Error: could not configure /dev/net/tun\n");
        close(fd);
        return NULL;
    }
    fcntl(fd, F_SETFL, O_NONBLOCK);

    net = mallocz(sizeof(*net));
    net->mac_addr[0] = 0x02;
    net->mac_addr[1] = 0x00;
    net->mac_addr[2] = 0x00;
    net->mac_addr[3] = 0x00;
    net->mac_addr[4] = 0x00;
    net->mac_addr[5] = 0x01;
    s = mallocz(sizeof(*s));
    s->fd = fd;
    net->opaque = s;
    net->write_packet = tun_write_packet;
    net->select_fill = tun_select_fill;
    net->select_poll = tun_select_poll;
    return net;
}

#else
EthernetDevice *tun_open(const char *ifname)
{
    (void)ifname;
    fprintf(stderr, "k-vm: TAP is available on Linux only; use --network user on this host\n");
    return NULL;
}
#endif /* __linux__ */

#ifdef CONFIG_SLIRP

/*******************************************************/
/* slirp */

static Slirp *slirp_state;

static void slirp_write_packet(EthernetDevice *net,
                               const uint8_t *buf, int len)
{
    Slirp *slirp_state = net->opaque;
    slirp_input(slirp_state, buf, len);
}

int slirp_can_output(void *opaque)
{
    EthernetDevice *net = opaque;
    return net->device_can_write_packet(net);
}

void slirp_output(void *opaque, const uint8_t *pkt, int pkt_len)
{
    EthernetDevice *net = opaque;
    return net->device_write_packet(net, pkt, pkt_len);
}

static void slirp_select_fill1(EthernetDevice *net, int *pfd_max,
                               fd_set *rfds, fd_set *wfds, fd_set *efds,
                               int *pdelay)
{
    Slirp *slirp_state = net->opaque;
    slirp_select_fill(slirp_state, pfd_max, rfds, wfds, efds);
}

static void slirp_select_poll1(EthernetDevice *net, 
                               fd_set *rfds, fd_set *wfds, fd_set *efds,
                               int select_ret)
{
    Slirp *slirp_state = net->opaque;
    slirp_select_poll(slirp_state, rfds, wfds, efds, (select_ret <= 0));
}

#ifdef _WIN32
static void host_winsock_cleanup(void) { WSACleanup(); }
#endif
EthernetDevice *slirp_open(void)
{
    EthernetDevice *net;
    struct in_addr net_addr  = { .s_addr = htonl(0x0a000200) }; /* 10.0.2.0 */
    struct in_addr mask = { .s_addr = htonl(0xffffff00) }; /* 255.255.255.0 */
    struct in_addr host = { .s_addr = htonl(0x0a000202) }; /* 10.0.2.2 */
    struct in_addr dhcp = { .s_addr = htonl(0x0a00020f) }; /* 10.0.2.15 */
    struct in_addr dns  = { .s_addr = htonl(0x0a000203) }; /* 10.0.2.3 */
    const char *bootfile = NULL;
    const char *vhostname = NULL;
    int restricted = 0;
    
    if (slirp_state) {
        fprintf(stderr, "Only a single slirp instance is allowed\n");
        return NULL;
    }
#ifdef _WIN32
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data)) {
        fprintf(stderr, "k-vm: Winsock initialization failed\n");
        return NULL;
    }
    atexit(host_winsock_cleanup);
#endif
    net = mallocz(sizeof(*net));

    slirp_state = slirp_init(restricted, net_addr, mask, host, vhostname,
                             "", bootfile, dhcp, dns, net);
    
    net->mac_addr[0] = 0x02;
    net->mac_addr[1] = 0x00;
    net->mac_addr[2] = 0x00;
    net->mac_addr[3] = 0x00;
    net->mac_addr[4] = 0x00;
    net->mac_addr[5] = 0x01;
    net->opaque = slirp_state;
    net->write_packet = slirp_write_packet;
    net->select_fill = slirp_select_fill1;
    net->select_poll = slirp_select_poll1;
    
    return net;
}

#endif /* CONFIG_SLIRP */

