/*
 * Protocolo entre o cliente (libandroid-sysvshm.so, glibc, dentro do guest Debian)
 * e o broker (libsysvshm-server.so, Bionic, processo filho do app).
 *
 * Transporte: AF_UNIX/SOCK_STREAM. Mensagens de tamanho fixo, little-endian (arm64).
 * O descritor da memória compartilhada viaja por SCM_RIGHTS junto com a resposta ATTACH.
 */
#ifndef PLASMA_SYSVSHM_PROTO_H
#define PLASMA_SYSVSHM_PROTO_H

#include <stdint.h>

#define SYSVSHM_PROTO_VERSION 2
#define SYSVSHM_DEFAULT_SOCKET "/tmp/.plasma-shm/sock"
#define SYSVSHM_ENV_SERVER "ANDROID_SYSVSHM_SERVER"

enum sysvshm_op {
    SYSVSHM_OP_GET    = 1, /* shmget:  key,size,flags -> status = shmid (>0) ou -errno           */
    SYSVSHM_OP_ATTACH = 2, /* shmat:   id             -> status = 0 + fd (SCM_RIGHTS) + size      */
    SYSVSHM_OP_DETACH = 3, /* shmdt:   id             -> status = 0                               */
    SYSVSHM_OP_RMID   = 4, /* IPC_RMID: id            -> status = 0                               */
    SYSVSHM_OP_STAT   = 5, /* IPC_STAT: id            -> status = 0 + campos de resposta          */
    SYSVSHM_OP_PING   = 6  /* diagnóstico             -> status = SYSVSHM_PROTO_VERSION           */
};

#pragma pack(push, 1)
struct sysvshm_req {
    uint32_t op;
    int32_t  id;
    int32_t  key;
    int32_t  flags;   /* flags do shmget (IPC_CREAT/IPC_EXCL/modo) */
    uint64_t size;
    uint64_t reserved;
};                     /* 32 bytes */

struct sysvshm_resp {
    int32_t  status;   /* >= 0 sucesso, < 0 = -errno */
    int32_t  nattch;
    uint64_t size;     /* tamanho arredondado à página */
    int32_t  key;
    int32_t  cpid;     /* pid do criador */
    int64_t  ctime;    /* segundos desde epoch */
    uint32_t cuid;
    uint32_t reserved;
};                     /* 40 bytes */
#pragma pack(pop)

_Static_assert(sizeof(struct sysvshm_req) == 32, "sysvshm_req");
_Static_assert(sizeof(struct sysvshm_resp) == 40, "sysvshm_resp");

#endif
