#ifndef LWIP_LWIPOPTS_H
#define LWIP_LWIPOPTS_H

#define LWIP_HOOK_FILENAME "lwip_hooks.h"

/* NO_SYS=1: we drive lwIP from our own Kotlin threads under a mutex */
#define NO_SYS                      1
#define LWIP_NETCONN                0
#define LWIP_SOCKET                 0
#define LWIP_TCPIP_CORE_LOCKING     0
#define SYS_LIGHTWEIGHT_PROT        0
#define LWIP_ASSERT_CORE_LOCKED()

/* Memory model: 64-bit aligned, libc malloc for the heap */
#define MEM_ALIGNMENT               8
#define MEM_LIBC_MALLOC             1
#define MEMP_MEM_MALLOC             0
#define MEM_SIZE                    (256 * 1024)

/* IPv6 only */
#define LWIP_IPV4                   0
#define LWIP_IPV6                   1
#define LWIP_IPV6_AUTOCONFIG        0
#define LWIP_IPV6_SCOPES            0
#define LWIP_IPV6_SEND_ROUTER_SOLICIT 0
#define LWIP_IPV6_DUP_DETECT_ATTEMPTS 0
#define LWIP_IPV6_MLD               0
#define LWIP_IPV6_DHCP6             0

/* CRITICAL: no IPv6 reassembly.
   ip6_reass_tmr() asserts sizeof(ip6_reass_helper) <= IP6_FRAG_HLEN,
   which is false on 64-bit and calls abort(). CarPlay/NCM never fragments. */
#define LWIP_IPV6_REASS             0
#define LWIP_IPV6_FRAG              0
#define IPV6_FRAG_COPYHEADER        1

/* Ethernet + NDP */
#define LWIP_ETHERNET               1
#define LWIP_ARP                    0
#define LWIP_ND6_QUEUEING           1
#define MEMP_NUM_ND6_QUEUE          20
#define LWIP_ND6_NUM_NEIGHBORS      8
#define LWIP_ND6_NUM_DESTINATIONS   8

/* TCP */
#define LWIP_TCP                    1
#define TCP_MSS                     1460
#define TCP_SND_BUF                 (8 * TCP_MSS)
#define TCP_WND                     (8 * TCP_MSS)
#define TCP_SNDLOWAT                (TCP_SND_BUF / 4)
#define TCP_SNDQUEUELOWAT           ((TCP_SND_BUF / TCP_MSS) / 2)
#define TCP_LISTEN_BACKLOG          1
#define TCP_DEFAULT_LISTEN_BACKLOG  4
#define LWIP_TCP_KEEPALIVE          1
#define LWIP_WND_SCALE              1
#define TCP_RCV_SCALE               2
#define MEMP_NUM_TCP_PCB            12
#define MEMP_NUM_TCP_PCB_LISTEN     4
#define MEMP_NUM_TCP_SEG            64

/* No UDP/DNS needed for wired CarPlay */
#define LWIP_UDP                    1
#define LWIP_DNS                    0
#define LWIP_RAW                    0
#define LWIP_ICMP                   0
#define LWIP_IGMP                   0
#define LWIP_DHCP                   0
#define LWIP_AUTOIP                 0

/* UDP (CarPlay timing + audio streams) */
#define MEMP_NUM_UDP_PCB            12
#define CHECKSUM_GEN_UDP            1
#define CHECKSUM_CHECK_UDP          1

/* Buffers */
#define PBUF_POOL_SIZE              48
#define PBUF_POOL_BUFSIZE           1600
#define MEMP_NUM_PBUF               32
#define MEMP_NUM_SYS_TIMEOUT        16

/* Checksums in software */
#define CHECKSUM_GEN_IP6            1
#define CHECKSUM_CHECK_IP6          1
#define CHECKSUM_GEN_TCP            1
#define CHECKSUM_CHECK_TCP          1
#define CHECKSUM_GEN_ICMP6          1
#define CHECKSUM_CHECK_ICMP6        1

/* Single netif, no loopback needed */
#define LWIP_SINGLE_NETIF           1
#define LWIP_HAVE_LOOPIF            0
#define LWIP_NETIF_LOOPBACK         0
#define LWIP_NETIF_HOSTNAME         0
#define LWIP_NETIF_STATUS_CALLBACK  0
#define LWIP_NETIF_LINK_CALLBACK    0
#define LWIP_STATS                  0

#endif /* LWIP_LWIPOPTS_H */
