#ifndef _LWIPOPTS_H
#define _LWIPOPTS_H

#define NO_SYS                      1
#define LWIP_SOCKET                 0
#define LWIP_COMPAT_SOCKETS         0
#define LWIP_NETCONN                0
#define LWIP_NETIF_LINK_CALLBACK    1
#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_NETIF_EXT_STATUS_CALLBACK 1
#define LWIP_IPV4                   1
#define LWIP_TCP                    1
#define LWIP_UDP                    1
#define LWIP_DNS                    1
#define LWIP_IGMP                   1
#define LWIP_DHCP                   1
#define LWIP_ARP                    1

#define TCP_MSS                     1460
#define TCP_WND                     65000
#define TCP_SND_BUF                 65000
#define TCP_SND_QUEUELEN            128
#define MEM_SIZE                    40000
#define MEMP_NUM_TCP_PCB            16
#define MEMP_NUM_TCP_PCB_LISTEN     4
#define MEMP_NUM_TCP_SEG            128
#define MEMP_NUM_SYS_TIMEOUT        10
#define MEMP_NUM_PBUF               64
#define PBUF_POOL_SIZE              32
#define LWIP_DISABLE_TCP_SANITY_CHECKS 1
#define LWIP_ALTCP                  0
#define LWIP_ALTCP_TLS              0


#endif
