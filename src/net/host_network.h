/* Linux TAP and SLIRP host backends for the VirtIO Ethernet interface. */
#ifndef K_VM_HOST_NETWORK_H
#define K_VM_HOST_NETWORK_H

#include "cutils.h"
#include "virtio.h"
#ifndef _WIN32
EthernetDevice *tun_open(const char *ifname);
#endif
#ifdef CONFIG_SLIRP
EthernetDevice *slirp_open(void);
#endif

#endif
