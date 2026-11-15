#ifndef PLASMA_SYSVSHM_PROTO_H
#define PLASMA_SYSVSHM_PROTO_H

#include <stdint.h>

#define SYSVSHM_PROTO_VERSION 2
#define SYSVSHM_DEFAULT_SOCKET "/tmp/.plasma-shm/sock"
#define SYSVSHM_ENV_SERVER "ANDROID_SYSVSHM_SERVER"

enum sysvshm_op {
    SYSVSHM_OP_GET    = 1,
    SYSVSHM_OP_ATTACH = 2,
    SYSVSHM_OP_DETACH = 3,
    SYSVSHM_OP_RMID   = 4,
    SYSVSHM_OP_STAT   = 5,
    SYSVSHM_OP_PING   = 6
};

#pragma pack(push, 1)
struct sysvshm_req {
    uint32_t op;
    int32_t  id;
    int32_t  key;
    int32_t  flags;
    uint64_t size;
    uint64_t reserved;
};

struct sysvshm_resp {
    int32_t  status;
    int32_t  nattch;
    uint64_t size;
    int32_t  key;
    int32_t  cpid;
    int64_t  ctime;
    uint32_t cuid;
    uint32_t reserved;
};
#pragma pack(pop)

_Static_assert(sizeof(struct sysvshm_req) == 32, "sysvshm_req");
_Static_assert(sizeof(struct sysvshm_resp) == 40, "sysvshm_resp");

#endif
