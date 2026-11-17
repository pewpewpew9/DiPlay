LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := xcertplay_i2c
LOCAL_SRC_FILES := linux_i2c_jni.c
include $(BUILD_SHARED_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := local_hotspot_radio
LOCAL_SRC_FILES := local_hotspot_radio.c
LOCAL_CFLAGS := -Wall -Wextra -Werror
include $(BUILD_SHARED_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := speex_echo
LOCAL_SRC_FILES := speex_echo_jni.c \
    speexdsp/mdf.c speexdsp/preprocess.c speexdsp/fftwrap.c \
    speexdsp/kiss_fft.c speexdsp/kiss_fftr.c speexdsp/filterbank.c
LOCAL_C_INCLUDES := $(LOCAL_PATH)/speexdsp $(LOCAL_PATH)/speexdsp/include
LOCAL_CFLAGS := -O2 -DFLOATING_POINT -DUSE_KISS_FFT -DEXPORT= -Wno-unused-parameter
LOCAL_LDLIBS := -lm
include $(BUILD_SHARED_LIBRARY)

# Userspace network stack with lwIP (IPv6-only, minimal)
include $(CLEAR_VARS)
LOCAL_MODULE := userspace_network_stack

LWIP_SRC := $(LOCAL_PATH)/lwip/src

LOCAL_SRC_FILES := \
    userspace_network_stack.c \
    lwip_port/sys_arch.c \
    $(LWIP_SRC)/core/init.c \
    $(LWIP_SRC)/core/def.c \
    $(LWIP_SRC)/core/dns.c \
    $(LWIP_SRC)/core/inet_chksum.c \
    $(LWIP_SRC)/core/ip.c \
    $(LWIP_SRC)/core/mem.c \
    $(LWIP_SRC)/core/memp.c \
    $(LWIP_SRC)/core/netif.c \
    $(LWIP_SRC)/core/pbuf.c \
    $(LWIP_SRC)/core/raw.c \
    $(LWIP_SRC)/core/stats.c \
    $(LWIP_SRC)/core/sys.c \
    $(LWIP_SRC)/core/tcp.c \
    $(LWIP_SRC)/core/tcp_in.c \
    $(LWIP_SRC)/core/tcp_out.c \
    $(LWIP_SRC)/core/timeouts.c \
    $(LWIP_SRC)/core/udp.c \
    $(LWIP_SRC)/core/ipv6/ethip6.c \
    $(LWIP_SRC)/core/ipv6/icmp6.c \
    $(LWIP_SRC)/core/ipv6/inet6.c \
    $(LWIP_SRC)/core/ipv6/ip6_addr.c \
    $(LWIP_SRC)/core/ipv6/ip6.c \
    $(LWIP_SRC)/core/ipv6/nd6.c \
    $(LWIP_SRC)/netif/ethernet.c

LOCAL_C_INCLUDES := \
    $(LOCAL_PATH)/lwip/src/include \
    $(LOCAL_PATH)/lwip_port

LOCAL_CFLAGS := -O2 -Wno-unused-parameter -Wno-unused-variable
LOCAL_LDLIBS := -llog

include $(BUILD_SHARED_LIBRARY)
