#include <jni.h>
#include <android/log.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>

#include "lwip/init.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "lwip/netif.h"
#include "lwip/ip6.h"
#include "lwip/ip6_addr.h"
#include "lwip/pbuf.h"
#include "lwip/timeouts.h"
#include "lwip/prot/tcp.h"
#include "lwip_hooks.h"

#define LOG_TAG "UserspaceIPv6"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define MAX_CONN 8
#define FRAME_MAX 1600

static struct netif ncm_netif;
static int netif_added = 0;
static int stack_ready = 0;
static u8_t host_mac[6];
static u8_t peer_mac[6];
static int peer_mac_known = 0;
static u8_t peer_ip6_src[16];
static int peer_ip6_known = 0;
static struct tcp_pcb *listener_pcb = NULL;

static JavaVM *g_jvm = NULL;
static jobject g_callback = NULL;
static jmethodID g_on_frame_output = NULL;
static jmethodID g_on_accept = NULL;
static jmethodID g_on_data = NULL;
static jmethodID g_on_closed = NULL;
static jmethodID g_on_udp_data = NULL;

struct conn { struct tcp_pcb *pcb; int id; int closed; };
static struct conn conns[MAX_CONN];
static int next_conn_id = 1;
static pthread_mutex_t stack_mutex = PTHREAD_MUTEX_INITIALIZER;

/* diagnostics */
static unsigned long n_in = 0, n_tcp = 0, n_syn = 0, n_out = 0, n_drop = 0;

static err_t carplay_netif_init(struct netif *netif);
static err_t carplay_output_ip6(struct netif *netif, struct pbuf *p, const ip6_addr_t *ipaddr);
static err_t carplay_linkoutput(struct netif *netif, struct pbuf *p);

static JNIEnv *attach_env(int *attached) {
    JNIEnv *env = NULL; *attached = 0;
    if (!g_jvm) return NULL;
    if ((*g_jvm)->GetEnv(g_jvm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
        if ((*g_jvm)->AttachCurrentThread(g_jvm, &env, NULL) != JNI_OK) return NULL;
        *attached = 1;
    }
    return env;
}
static void detach_env(int attached) { if (attached) (*g_jvm)->DetachCurrentThread(g_jvm); }

static void emit_frame(const u8_t *data, int len) {
    if (!g_callback || !g_on_frame_output) return;
    if (len <= 0 || len > FRAME_MAX) return;
    int att; JNIEnv *env = attach_env(&att);
    if (!env) return;
    jbyteArray arr = (*env)->NewByteArray(env, len);
    if (arr) {
        (*env)->SetByteArrayRegion(env, arr, 0, len, (const jbyte *)data);
        (*env)->CallVoidMethod(env, g_callback, g_on_frame_output, arr);
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
        (*env)->DeleteLocalRef(env, arr);
    }
    detach_env(att);
}

/* linkoutput: p is already a complete Ethernet frame. Never frees p. */
static err_t carplay_linkoutput(struct netif *netif, struct pbuf *p) {
    LWIP_UNUSED_ARG(netif);
    u8_t frame[FRAME_MAX];
    u16_t n = pbuf_copy_partial(p, frame, sizeof(frame), 0);
    if (n > 0) { n_out++; emit_frame(frame, (int)n); }
    return ERR_OK;
}

/* output_ip6: build the Ethernet header in a flat buffer.
   IMPORTANT: we must NOT take ownership of p (no pbuf_cat / no pbuf_free):
   lwIP frees it after netif->input returns. */
static err_t carplay_output_ip6(struct netif *netif, struct pbuf *p, const ip6_addr_t *ipaddr) {
    u8_t dst[6];
    if (ip6_addr_ismulticast(ipaddr)) {
        u32_t last = lwip_ntohl(ipaddr->addr[3]);
        dst[0]=0x33; dst[1]=0x33;
        dst[2]=(u8_t)(last>>24); dst[3]=(u8_t)(last>>16);
        dst[4]=(u8_t)(last>>8);  dst[5]=(u8_t)last;
    } else if (peer_mac_known) {
        memcpy(dst, peer_mac, 6);
    } else {
        n_drop++;
        return ERR_OK;
    }

    u8_t frame[FRAME_MAX];
    memcpy(frame, dst, 6);
    memcpy(frame + 6, host_mac, 6);
    frame[12] = 0x86; frame[13] = 0xDD;

    u16_t n = pbuf_copy_partial(p, frame + 14, sizeof(frame) - 14, 0);
    if (n == 0) return ERR_OK;
    n_out++;
    emit_frame(frame, 14 + (int)n);
    LWIP_UNUSED_ARG(netif);
    return ERR_OK;
}

static err_t carplay_netif_init(struct netif *netif) {
    netif->name[0]='c'; netif->name[1]='p';
    netif->output_ip6 = carplay_output_ip6;
    netif->linkoutput = carplay_linkoutput;
    netif->mtu = 1500;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHERNET | NETIF_FLAG_LINK_UP;
    memcpy(netif->hwaddr, host_mac, 6);
    netif->hwaddr_len = 6;
    netif_ip6_addr_set_parts(netif, 0, PP_HTONL(0xFE800000UL), 0, 0, PP_HTONL(0x00000002UL));
    netif_ip6_addr_set_state(netif, 0, IP6_ADDR_PREFERRED);
    return ERR_OK;
}

/* ---- TCP callbacks (called with stack_mutex held) ---- */

static void notify_closed_id(int id) {
    if (!g_callback || !g_on_closed) return;
    int att; JNIEnv *env = attach_env(&att);
    if (!env) return;
    (*env)->CallVoidMethod(env, g_callback, g_on_closed, (jint)id);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    detach_env(att);
}

static void close_conn_locked(struct conn *c) {
    if (!c || c->closed) return;
    struct tcp_pcb *pcb = c->pcb;
    int id = c->id;
    c->closed = 1; c->pcb = NULL; c->id = 0;
    if (pcb) {
        tcp_arg(pcb, NULL); tcp_recv(pcb, NULL); tcp_sent(pcb, NULL);
        tcp_err(pcb, NULL); tcp_poll(pcb, NULL, 0);
        tcp_abort(pcb);
    }
    notify_closed_id(id);
}

static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    struct conn *c = (struct conn *)arg;
    LWIP_UNUSED_ARG(err);
    if (!c || c->closed) { if (p) pbuf_free(p); return ERR_ABRT; }
    if (p == NULL) { close_conn_locked(c); return ERR_OK; }

    u8_t buf[4096];
    u16_t total = p->tot_len;
    u16_t n = pbuf_copy_partial(p, buf, sizeof(buf), 0);
    tcp_recved(pcb, total);
    pbuf_free(p);

    if (n > 0 && g_callback && g_on_data) {
        int att; JNIEnv *env = attach_env(&att);
        if (env) {
            jbyteArray arr = (*env)->NewByteArray(env, n);
            if (arr) {
                (*env)->SetByteArrayRegion(env, arr, 0, n, (jbyte *)buf);
                (*env)->CallVoidMethod(env, g_callback, g_on_data, (jint)c->id, arr);
                if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
                (*env)->DeleteLocalRef(env, arr);
            }
            detach_env(att);
        }
    }
    return ERR_OK;
}

static err_t on_sent(void *arg, struct tcp_pcb *pcb, u16_t len) {
    LWIP_UNUSED_ARG(arg); LWIP_UNUSED_ARG(pcb); LWIP_UNUSED_ARG(len);
    return ERR_OK;
}

static void on_err(void *arg, err_t err) {
    struct conn *c = (struct conn *)arg;
    LWIP_UNUSED_ARG(err);
    if (!c) return;
    int id = c->id;
    c->pcb = NULL; c->closed = 1; c->id = 0;   /* lwIP already freed the pcb */
    notify_closed_id(id);
}

static err_t on_accept(void *arg, struct tcp_pcb *pcb, err_t err) {
    LWIP_UNUSED_ARG(arg);
    if (err != ERR_OK || !pcb) return ERR_VAL;

    struct conn *slot = NULL;
    for (int i = 0; i < MAX_CONN; i++)
        if (!conns[i].pcb && conns[i].id == 0) { slot = &conns[i]; break; }
    if (!slot) { LOGE("no free conn slot"); tcp_abort(pcb); return ERR_ABRT; }

    slot->pcb = pcb; slot->id = next_conn_id++; slot->closed = 0;
    int id = slot->id;
    u16_t lport = pcb->local_port, rport = pcb->remote_port;

    tcp_arg(pcb, slot);
    tcp_recv(pcb, on_recv);
    tcp_sent(pcb, on_sent);
    tcp_err(pcb, on_err);
    tcp_nagle_disable(pcb);

    LOGI("TCP accepted id=%d local_port=%u remote_port=%u", id, (unsigned)lport, (unsigned)rport);

    if (g_callback && g_on_accept) {
        int att; JNIEnv *env = attach_env(&att);
        if (env) {
            (*env)->CallVoidMethod(env, g_callback, g_on_accept, (jint)id, (jint)lport);
            if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
            detach_env(att);
        }
    }
    return ERR_OK;
}


/* forward declarations for the UDP relay (defined further down) */
static void make_udp(u16_t port);
static void on_udp_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                        const ip_addr_t *addr, u16_t port);

/* ---- dynamic listeners for phone-chosen ports ----
   The iPhone opens extra TCP connections to ports it picks itself and announces
   via AirPlay SETUP (event / data channels). lwIP's TCP hook is NOT called when
   no PCB matches (it sends RST instead), so we intercept at the IPv6 layer:
   LWIP_HOOK_IP6_INPUT runs before any header parsing or TCP demultiplexing.
   Called with stack_mutex held (from nativeInput -> ip6_input). */
#define MAX_EXTRA_LISTENERS 8
static struct tcp_pcb *extra_listeners[MAX_EXTRA_LISTENERS];

static int listener_exists(u16_t port) {
    if (listener_pcb && listener_pcb->local_port == port) return 1;
    for (int i = 0; i < MAX_EXTRA_LISTENERS; i++)
        if (extra_listeners[i] && extra_listeners[i]->local_port == port) return 1;
    return 0;
}

static void make_listener(u16_t port) {
    int slot = -1;
    for (int i = 0; i < MAX_EXTRA_LISTENERS; i++)
        if (!extra_listeners[i]) { slot = i; break; }
    if (slot < 0) { LOGE("no listener slot for port %u", (unsigned)port); return; }

    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_V6);
    if (!pcb) { LOGE("tcp_new failed for port %u", (unsigned)port); return; }
    if (tcp_bind(pcb, IP6_ADDR_ANY, port) != ERR_OK) {
        LOGE("tcp_bind(%u) failed", (unsigned)port);
        tcp_close(pcb);
        return;
    }
    err_t lerr = ERR_OK;
    struct tcp_pcb *l = tcp_listen_with_backlog_and_err(pcb, 4, &lerr);
    if (!l) { LOGE("tcp_listen(%u) failed err=%d", (unsigned)port, (int)lerr); listener_pcb = NULL; return; }
    tcp_accept(l, on_accept);
    extra_listeners[slot] = l;
    LOGI("dynamic listener created on port %u", (unsigned)port);
}

/* Peek a pure SYN out of the raw IPv6 packet and materialise a listener.
   p->payload points at the IPv6 header here. We never consume the packet. */
int carplay_ip6_input_hook(struct pbuf *p, struct netif *inp) {
    LWIP_UNUSED_ARG(inp);
    u8_t hdr[40 + 20];
    u16_t got = pbuf_copy_partial(p, hdr, sizeof(hdr), 0);
    if (got < 40) return 0;

    /* The phone's real link-local address, not the EUI-64 guess: iOS may use a
       different interface identifier, and every car->phone datagram needs it. */
    if (!peer_ip6_known && hdr[8] == 0xfe && (hdr[9] & 0xc0) == 0x80) {
        memcpy(peer_ip6_src, hdr + 8, 16);
        peer_ip6_known = 1;
        LOGI("peer IPv6 learned %02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x",
             hdr[8],hdr[9],hdr[10],hdr[11],hdr[12],hdr[13],hdr[14],hdr[15],
             hdr[16],hdr[17],hdr[18],hdr[19],hdr[20],hdr[21],hdr[22],hdr[23]);
    }

    u8_t nexth = hdr[6];
    /* skip a Hop-by-Hop header (present on some MLD/NS packets) */
    u16_t off = 40;
    if (nexth == 0 && got >= 42) {
        u16_t hlen = (u16_t)(8 * (1 + hdr[41]));
        nexth = hdr[40];
        off = (u16_t)(40 + hlen);
    }
    if (nexth == 17) {                        /* UDP: timing / audio */
        if (got >= (u16_t)(off + 4)) {
            u16_t dport = (u16_t)((hdr[off + 2] << 8) | hdr[off + 3]);
            make_udp(dport);
        }
        return 0;
    }
    if (nexth != 6) return 0;                 /* not TCP */
    if (got < (u16_t)(off + 14)) return 0;    /* TCP flags not available */

    u8_t flags = hdr[off + 13];
    if (!(flags & 0x02) || (flags & 0x10)) return 0;   /* need SYN, no ACK */

    u16_t dport = (u16_t)((hdr[off + 2] << 8) | hdr[off + 3]);
    if (!listener_exists(dport)) make_listener(dport);
    return 0;   /* let lwIP process it normally */
}


/* ---- UDP relays (CarPlay timing / audio) ----
   Same trick as TCP: the phone picks its own ports, so we create a UDP pcb
   on demand from the IPv6 input hook and relay datagrams to 127.0.0.1. */
#define MAX_UDP 8
struct udprelay { struct udp_pcb *pcb; u16_t port; };
static struct udprelay udps[MAX_UDP];

static void on_udp_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                        const ip_addr_t *addr, u16_t port) {
    struct udprelay *u = (struct udprelay *)arg;
    LWIP_UNUSED_ARG(pcb);
    if (p == NULL) return;
    if (u && g_callback && g_on_udp_data) {
        u8_t buf[2048];
        u16_t n = pbuf_copy_partial(p, buf, sizeof(buf), 0);
        if (n > 0) {
            int att; JNIEnv *env = attach_env(&att);
            if (env) {
                jbyteArray arr = (*env)->NewByteArray(env, n);
                if (arr) {
                    (*env)->SetByteArrayRegion(env, arr, 0, n, (jbyte *)buf);
                    /* remember the phone's source port so replies go back correctly */
                    (*env)->CallVoidMethod(env, g_callback, g_on_udp_data,
                                           (jint)u->port, (jint)port, arr);
                    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
                    (*env)->DeleteLocalRef(env, arr);
                }
                detach_env(att);
            }
        }
    }
    pbuf_free(p);   /* the recv callback owns the pbuf */
    LWIP_UNUSED_ARG(addr);
}

static void make_udp(u16_t port) {
    for (int i = 0; i < MAX_UDP; i++) if (udps[i].pcb && udps[i].port == port) return;
    int slot = -1;
    for (int i = 0; i < MAX_UDP; i++) if (!udps[i].pcb) { slot = i; break; }
    if (slot < 0) { LOGE("no udp slot for %u", (unsigned)port); return; }

    struct udp_pcb *pcb = udp_new_ip_type(IPADDR_TYPE_V6);
    if (!pcb) return;
    if (udp_bind(pcb, IP6_ADDR_ANY, port) != ERR_OK) { udp_remove(pcb); return; }
    udps[slot].pcb = pcb; udps[slot].port = port;
    udp_recv(pcb, on_udp_recv, &udps[slot]);
    LOGI("dynamic UDP listener on port %u", (unsigned)port);
}

/* ---------- JNI ---------- */

JNIEXPORT jint JNICALL
Java_com_shilapi_xcertplay_network_UserspaceIpv6NcmBridge_nativeInit(
    JNIEnv *env, jobject thiz, jbyteArray hostMacArr, jint port) {
    LWIP_UNUSED_ARG(thiz);
    (*env)->GetJavaVM(env, &g_jvm);
    if ((*env)->GetArrayLength(env, hostMacArr) != 6) return -1;
    (*env)->GetByteArrayRegion(env, hostMacArr, 0, 6, (jbyte *)host_mac);

    pthread_mutex_lock(&stack_mutex);

    if (!stack_ready) { lwip_init(); stack_ready = 1; }

    /* clean any previous attachment before re-adding */
    for (int i = 0; i < MAX_CONN; i++)
        if (conns[i].pcb && !conns[i].closed) close_conn_locked(&conns[i]);
    memset(conns, 0, sizeof(conns));
    if (listener_pcb) { tcp_close(listener_pcb); listener_pcb = NULL; }
    for (int i = 0; i < MAX_EXTRA_LISTENERS; i++)
        if (extra_listeners[i]) { tcp_close(extra_listeners[i]); extra_listeners[i] = NULL; }
    for (int i = 0; i < MAX_EXTRA_LISTENERS; i++)
        if (extra_listeners[i]) { tcp_close(extra_listeners[i]); extra_listeners[i] = NULL; }
    if (netif_added) {
        netif_set_down(&ncm_netif);
        netif_remove(&ncm_netif);
        netif_added = 0;
    }
    for (int i = 0; i < MAX_UDP; i++)
        if (udps[i].pcb) { udp_remove(udps[i].pcb); udps[i].pcb = NULL; udps[i].port = 0; }
    peer_mac_known = 0; peer_ip6_known = 0;
    n_in = n_tcp = n_syn = n_out = n_drop = 0;

    if (!netif_add(&ncm_netif, NULL, carplay_netif_init, ip6_input)) {
        pthread_mutex_unlock(&stack_mutex);
        LOGE("netif_add failed"); return -2;
    }
    netif_added = 1;
    netif_set_default(&ncm_netif);
    netif_set_link_up(&ncm_netif);
    netif_set_up(&ncm_netif);

    listener_pcb = tcp_new_ip_type(IPADDR_TYPE_V6);
    if (!listener_pcb) { pthread_mutex_unlock(&stack_mutex); return -3; }
    if (tcp_bind(listener_pcb, IP6_ADDR_ANY, (u16_t)port) != ERR_OK) {
        LOGE("tcp_bind(%d) failed", (int)port);
        tcp_close(listener_pcb); listener_pcb = NULL;
        pthread_mutex_unlock(&stack_mutex);
        return -4;
    }
    /* NOTE: tcp_listen frees the original pcb and returns a smaller LISTEN pcb */
    struct tcp_pcb *l = tcp_listen_with_backlog(listener_pcb, 4);
    if (!l) { LOGE("tcp_listen failed"); listener_pcb = NULL; pthread_mutex_unlock(&stack_mutex); return -5; }
    listener_pcb = l;
    tcp_accept(listener_pcb, on_accept);

    pthread_mutex_unlock(&stack_mutex);
    LOGI("lwIP up, listening on port %d", (int)port);
    return 0;
}

JNIEXPORT void JNICALL
Java_com_shilapi_xcertplay_network_UserspaceIpv6NcmBridge_nativeInput(
    JNIEnv *env, jobject thiz, jbyteArray frameArr) {
    LWIP_UNUSED_ARG(thiz);
    jsize len = (*env)->GetArrayLength(env, frameArr);
    if (len < 14 || len > FRAME_MAX) return;
    u8_t buf[FRAME_MAX];
    (*env)->GetByteArrayRegion(env, frameArr, 0, len, (jbyte *)buf);
    if ((((u16_t)buf[12] << 8) | buf[13]) != 0x86DD) return;

    if (!peer_mac_known) {
        memcpy(peer_mac, buf + 6, 6); peer_mac_known = 1;
        LOGI("peer MAC %02x:%02x:%02x:%02x:%02x:%02x",
             peer_mac[0],peer_mac[1],peer_mac[2],peer_mac[3],peer_mac[4],peer_mac[5]);
    }

    u16_t payload = (u16_t)(len - 14);
    if (payload < 40) return;

    /* cheap IPv6/TCP peek for diagnostics only */
    u8_t nexth = buf[14 + 6];
    if (nexth == 6 && payload >= 40 + 14) {
        n_tcp++;
        u8_t flags = buf[14 + 40 + 13];
        u16_t dport = ((u16_t)buf[14+40+2] << 8) | buf[14+40+3];
        if ((flags & 0x02) && !(flags & 0x10)) {
            n_syn++;
            LOGI("SYN -> port %u (tcp=%lu in=%lu)", (unsigned)dport, n_tcp, n_in);
        }
    }
    n_in++;
    if (n_in <= 5 || n_in % 200 == 0)
        LOGI("in#%lu bytes=%u next=%u out=%lu drop=%lu", n_in, (unsigned)payload, (unsigned)nexth, n_out, n_drop);

    struct pbuf *p = pbuf_alloc(PBUF_RAW, payload, PBUF_POOL);
    if (!p) { n_drop++; return; }
    if (pbuf_take(p, buf + 14, payload) != ERR_OK) { pbuf_free(p); n_drop++; return; }

    pthread_mutex_lock(&stack_mutex);
    err_t r = ERR_OK;
    if (netif_added && ncm_netif.input) r = ncm_netif.input(p, &ncm_netif);
    else { r = ERR_CONN; }
    pthread_mutex_unlock(&stack_mutex);

    if (r != ERR_OK) { pbuf_free(p); n_drop++; }   /* lwIP did not consume it */
}

JNIEXPORT jint JNICALL
Java_com_shilapi_xcertplay_network_UserspaceIpv6NcmBridge_nativeWrite(
    JNIEnv *env, jobject thiz, jint id, jbyteArray data) {
    LWIP_UNUSED_ARG(thiz);
    jsize len = (*env)->GetArrayLength(env, data);
    if (len <= 0 || len > 4096) return -1;
    u8_t buf[4096];
    (*env)->GetByteArrayRegion(env, data, 0, len, (jbyte *)buf);

    pthread_mutex_lock(&stack_mutex);
    struct conn *c = NULL;
    for (int i = 0; i < MAX_CONN; i++)
        if (conns[i].id == id && conns[i].pcb && !conns[i].closed) { c = &conns[i]; break; }
    if (!c) { pthread_mutex_unlock(&stack_mutex); return -2; }
    u16_t space = tcp_sndbuf(c->pcb);
    u16_t n = (u16_t)((len < space) ? len : space);
    if (n == 0) { pthread_mutex_unlock(&stack_mutex); return 0; }
    err_t e = tcp_write(c->pcb, buf, n, TCP_WRITE_FLAG_COPY);
    if (e == ERR_OK) tcp_output(c->pcb);
    pthread_mutex_unlock(&stack_mutex);
    return (e == ERR_OK) ? (jint)n : -3;
}

JNIEXPORT void JNICALL
Java_com_shilapi_xcertplay_network_UserspaceIpv6NcmBridge_nativeCloseConn(
    JNIEnv *env, jobject thiz, jint id) {
    LWIP_UNUSED_ARG(env); LWIP_UNUSED_ARG(thiz);
    pthread_mutex_lock(&stack_mutex);
    for (int i = 0; i < MAX_CONN; i++)
        if (conns[i].id == id && !conns[i].closed) { close_conn_locked(&conns[i]); break; }
    pthread_mutex_unlock(&stack_mutex);
}

JNIEXPORT void JNICALL
Java_com_shilapi_xcertplay_network_UserspaceIpv6NcmBridge_nativeTick(
    JNIEnv *env, jobject thiz) {
    LWIP_UNUSED_ARG(env); LWIP_UNUSED_ARG(thiz);
    if (!stack_ready) return;
    pthread_mutex_lock(&stack_mutex);
    sys_check_timeouts();
    pthread_mutex_unlock(&stack_mutex);
}

JNIEXPORT void JNICALL
Java_com_shilapi_xcertplay_network_UserspaceIpv6NcmBridge_nativeSetCallback(
    JNIEnv *env, jobject thiz, jobject callback) {
    LWIP_UNUSED_ARG(thiz);
    if (g_callback) { (*env)->DeleteGlobalRef(env, g_callback); g_callback = NULL; }
    g_on_frame_output = g_on_accept = g_on_data = g_on_closed = g_on_udp_data = NULL;
    if (!callback) return;
    g_callback = (*env)->NewGlobalRef(env, callback);
    jclass cls = (*env)->GetObjectClass(env, callback);
    g_on_frame_output = (*env)->GetMethodID(env, cls, "onFrameOutput", "([B)V");
    g_on_accept       = (*env)->GetMethodID(env, cls, "onAccept", "(II)V");
    g_on_data         = (*env)->GetMethodID(env, cls, "onData", "(I[B)V");
    g_on_closed       = (*env)->GetMethodID(env, cls, "onClosed", "(I)V");
    g_on_udp_data     = (*env)->GetMethodID(env, cls, "onUdpData", "(II[B)V");
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    (*env)->DeleteLocalRef(env, cls);
}

JNIEXPORT void JNICALL
Java_com_shilapi_xcertplay_network_UserspaceIpv6NcmBridge_nativeShutdown(
    JNIEnv *env, jobject thiz) {
    LWIP_UNUSED_ARG(thiz);
    pthread_mutex_lock(&stack_mutex);
    for (int i = 0; i < MAX_CONN; i++)
        if (conns[i].pcb && !conns[i].closed) close_conn_locked(&conns[i]);
    memset(conns, 0, sizeof(conns));
    if (listener_pcb) { tcp_close(listener_pcb); listener_pcb = NULL; }
    for (int i = 0; i < MAX_EXTRA_LISTENERS; i++)
        if (extra_listeners[i]) { tcp_close(extra_listeners[i]); extra_listeners[i] = NULL; }
    for (int i = 0; i < MAX_EXTRA_LISTENERS; i++)
        if (extra_listeners[i]) { tcp_close(extra_listeners[i]); extra_listeners[i] = NULL; }
    if (netif_added) {
        netif_set_down(&ncm_netif);
        netif_remove(&ncm_netif);
        netif_added = 0;
    }
    for (int i = 0; i < MAX_UDP; i++)
        if (udps[i].pcb) { udp_remove(udps[i].pcb); udps[i].pcb = NULL; udps[i].port = 0; }
    peer_mac_known = 0; peer_ip6_known = 0;
    pthread_mutex_unlock(&stack_mutex);
    if (g_callback) { (*env)->DeleteGlobalRef(env, g_callback); g_callback = NULL; }
    g_on_frame_output = g_on_accept = g_on_data = g_on_closed = g_on_udp_data = NULL;
}

JNIEXPORT jint JNICALL
Java_com_shilapi_xcertplay_network_UserspaceIpv6NcmBridge_nativeUdpSend(
    JNIEnv *env, jobject thiz, jint localPort, jint remotePort, jbyteArray data) {
    LWIP_UNUSED_ARG(thiz);
    jsize len = (*env)->GetArrayLength(env, data);
    if (len <= 0 || len > 2048) return -1;
    u8_t buf[2048];
    (*env)->GetByteArrayRegion(env, data, 0, len, (jbyte *)buf);

    pthread_mutex_lock(&stack_mutex);
    struct udp_pcb *pcb = NULL;
    for (int i = 0; i < MAX_UDP; i++)
        if (udps[i].pcb && udps[i].port == (u16_t)localPort) { pcb = udps[i].pcb; break; }
    if (!pcb || !peer_mac_known) { pthread_mutex_unlock(&stack_mutex); return -2; }

    /* destination = the phone's link-local address derived from its MAC (EUI-64) */
    ip_addr_t dst;
    ip6_addr_t d6;
    ip6_addr_set_zero(&d6);
    d6.addr[0] = PP_HTONL(0xFE800000UL);
    d6.addr[2] = PP_HTONL(((u32_t)(peer_mac[0]^0x02) << 24) | ((u32_t)peer_mac[1] << 16) |
                          ((u32_t)peer_mac[2] << 8) | 0x00FFUL);
    d6.addr[3] = PP_HTONL((0xFEUL << 24) | ((u32_t)peer_mac[3] << 16) |
                          ((u32_t)peer_mac[4] << 8) | (u32_t)peer_mac[5]);
    if (peer_ip6_known) memcpy(d6.addr, peer_ip6_src, 16);
    ip_addr_copy_from_ip6(dst, d6);

    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)len, PBUF_RAM);
    if (!p) { pthread_mutex_unlock(&stack_mutex); return -3; }
    pbuf_take(p, buf, (u16_t)len);
    err_t e = udp_sendto(pcb, p, &dst, (u16_t)remotePort);
    pbuf_free(p);   /* udp_sendto does not take ownership */
    pthread_mutex_unlock(&stack_mutex);
    return (e == ERR_OK) ? (jint)len : -4;
}
