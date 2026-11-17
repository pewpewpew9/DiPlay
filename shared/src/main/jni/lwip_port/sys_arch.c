#include "lwip/sys.h"
#include "lwip/opt.h"
#include <time.h>

/* With NO_SYS=1 and SYS_LIGHTWEIGHT_PROT=0, only sys_now() is required */

u32_t sys_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u32_t)((ts.tv_sec * 1000) + (ts.tv_nsec / 1000000));
}

void sys_init(void) {
}
