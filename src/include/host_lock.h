/* 9P lock types are protocol values, not host fcntl constants. */
#ifndef K_VM_HOST_LOCK_H
#define K_VM_HOST_LOCK_H
#include <fcntl.h>
/* Include fs.h before this header (upstream fs.h has no include guard). */
static inline int host_lock_type(unsigned type)
{
    switch (type) {
    case P9_LOCK_TYPE_RDLCK: return F_RDLCK;
    case P9_LOCK_TYPE_WRLCK: return F_WRLCK;
    case P9_LOCK_TYPE_UNLCK: return F_UNLCK;
    default: return -1;
    }
}
static inline unsigned p9_lock_type(int type)
{
    switch (type) {
    case F_RDLCK: return P9_LOCK_TYPE_RDLCK;
    case F_WRLCK: return P9_LOCK_TYPE_WRLCK;
    default: return P9_LOCK_TYPE_UNLCK;
    }
}
#endif
