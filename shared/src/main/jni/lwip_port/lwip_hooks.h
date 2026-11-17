#ifndef LWIP_HOOKS_H
#define LWIP_HOOKS_H

#include "lwip/opt.h"
#include "lwip/pbuf.h"
#include "lwip/netif.h"

/* Implemented in userspace_network_stack.c.
   Returns 1 if the packet was consumed (we free it), 0 to let lwIP process it. */
int carplay_ip6_input_hook(struct pbuf *p, struct netif *inp);

#define LWIP_HOOK_IP6_INPUT(p, inp) carplay_ip6_input_hook((p), (inp))

#endif /* LWIP_HOOKS_H */
