#ifndef LWIP_ARCH_CC_H
#define LWIP_ARCH_CC_H

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <android/log.h>

#define BYTE_ORDER LITTLE_ENDIAN

static inline void lwip_diag_log(const char *fmt, ...) {
  va_list ap;
  char buf[512];
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  __android_log_write(ANDROID_LOG_INFO, "UserspaceIPv6", buf);
}
#define LWIP_PLATFORM_DIAG(x) do { lwip_diag_log x; } while(0)

#define LWIP_PLATFORM_ASSERT(x) \
  __android_log_print(ANDROID_LOG_ERROR, "UserspaceIPv6", \
                      "lwIP assert: %s (%s:%d)", x, __FILE__, __LINE__)

#define LWIP_RAND() ((u32_t)rand())

#endif /* LWIP_ARCH_CC_H */
