/* A52_PHASE446AC_GPI_IPC_SHIM_V1 - logging only, not GPI transport */
#ifndef _LINUX_IPC_LOGGING_H
#define _LINUX_IPC_LOGGING_H
#include <linux/types.h>
static inline void *ipc_log_context_create(int pages,const char *name,int flags)
{
    return NULL;
}
static inline void ipc_log_string(void *ctx,const char *fmt,...) { }
/* A52_PHASE446AD_IPC_LOG_CONTEXT_DESTROY_COMPAT_V1: no allocation in the IPC logging stub. */
static inline void ipc_log_context_destroy(void *context)
{
    (void)context;
}

#endif
