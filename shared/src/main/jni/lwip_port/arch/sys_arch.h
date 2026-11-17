#ifndef LWIP_ARCH_SYS_ARCH_H
#define LWIP_ARCH_SYS_ARCH_H

#include <stdint.h>

typedef uint32_t sys_prot_t;

/* With NO_SYS=1 these types are not used but must be defined */
typedef void *sys_sem_t;
typedef void *sys_mutex_t;
typedef void *sys_mbox_t;
typedef int sys_thread_t;

#define SYS_MBOX_NULL NULL
#define SYS_SEM_NULL  NULL
#define SYS_MUTEX_NULL NULL

#endif /* LWIP_ARCH_SYS_ARCH_H */
