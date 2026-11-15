/*
 * libandroid-sysvshm.so — shmget/shmat/shmdt/shmctl para o guest glibc do PlasmaDroid.
 *
 * Base: SysVSHM do Winlator (https://github.com/pelya/android-shmem), reescrito para falar com o
 * broker sysvshm_server (ver sysvshm_proto.h). Mudanças principais:
 *   - shmat() aceita shmid criado por OUTRO processo (o broker devolve o fd): é o que o MIT-SHM
 *     do Xwayland precisa;
 *   - shmget() com chave (ftok), IPC_CREAT/IPC_EXCL;
 *   - shmctl(IPC_STAT) funciona para qualquer shmid; IPC_RMID é adiado até o último shmdt;
 *   - cada shmat é um mapeamento próprio (SysV permite anexar o mesmo id várias vezes);
 *   - sem estado global de socket: uma conexão por operação (seguro com fork e threads);
 *   - tamanho de 64 bits no protocolo (o original truncava em 32 bits) e getenv() nulo tratado;
 *   - usa o <sys/shm.h> do glibc do guest: o layout de struct shmid_ds é o do aarch64 real.
 *
 * Uso: LD_PRELOAD=/usr/local/lib/libandroid-sysvshm.so ANDROID_SYSVSHM_SERVER=/tmp/.plasma-shm/sock app
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include "sysvshm_proto.h"

#define EXPORT __attribute__((visibility("default")))
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

struct mapping {
    int    id;
    void  *addr;
    size_t size;
};

static struct mapping *maps;
static int map_count, map_cap;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static void atfork_child(void) { pthread_mutex_init(&lock, NULL); }
__attribute__((constructor)) static void init_lib(void) { pthread_atfork(NULL, NULL, atfork_child); }

/* ---------- RPC ---------- */

static const char *server_path(void) {
    const char *p = getenv(SYSVSHM_ENV_SERVER);
    return (p && *p) ? p : SYSVSHM_DEFAULT_SOCKET;
}

static int connect_server(void) {
    const char *path = server_path();
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(sa.sun_path)) { errno = ENAMETOOLONG; return -1; }
    strcpy(sa.sun_path, path);

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    for (;;) {
        if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0) return fd;
        if (errno == EINTR) continue;
        int e = errno; close(fd); errno = e;
        return -1;
    }
}

static int write_full(int fd, const void *buf, size_t n) {
    size_t sent = 0;
    while (sent < n) {
        ssize_t k = send(fd, (const char *)buf + sent, n - sent, MSG_NOSIGNAL);
        if (k < 0) { if (errno == EINTR) continue; return -1; }
        sent += (size_t)k;
    }
    return 0;
}

/* Envia o pedido e lê a resposta. Se fd_out != NULL, recebe o descritor anexado à resposta. */
static int rpc(const struct sysvshm_req *q, struct sysvshm_resp *r, int *fd_out) {
    if (fd_out) *fd_out = -1;
    int s = connect_server();
    if (s < 0) { if (errno == ENOENT || errno == ECONNREFUSED) errno = ENOSYS; return -1; }

    if (write_full(s, q, sizeof(*q)) < 0) { int e = errno; close(s); errno = e; return -1; }

    union { struct cmsghdr h; char buf[CMSG_SPACE(sizeof(int))]; } ctl;
    memset(&ctl, 0, sizeof(ctl));
    struct iovec iov = { .iov_base = r, .iov_len = sizeof(*r) };
    struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = ctl.buf, .msg_controllen = sizeof(ctl.buf) };

    ssize_t got;
    do { got = recvmsg(s, &msg, MSG_CMSG_CLOEXEC); } while (got < 0 && errno == EINTR);
    if (got <= 0) { int e = got < 0 ? errno : ECONNRESET; close(s); errno = e; return -1; }

    int fd = -1;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS && c->cmsg_len >= CMSG_LEN(sizeof(int))) {
            memcpy(&fd, CMSG_DATA(c), sizeof(int));
            break;
        }
    }
    /* resposta partida (raro em AF_UNIX/STREAM): completa o restante */
    while ((size_t)got < sizeof(*r)) {
        ssize_t k = read(s, (char *)r + got, sizeof(*r) - (size_t)got);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) { if (fd >= 0) close(fd); close(s); errno = ECONNRESET; return -1; }
        got += k;
    }
    close(s);

    if (fd_out) *fd_out = fd; else if (fd >= 0) close(fd);
    return 0;
}

static int fail_status(int status) { errno = -status; return -1; }

/* ---------- tabela de mapeamentos ---------- */

static int map_add(int id, void *addr, size_t size) {
    if (map_count == map_cap) {
        int cap = map_cap ? map_cap * 2 : 16;
        struct mapping *n = realloc(maps, (size_t)cap * sizeof(*n));
        if (!n) return -1;
        maps = n; map_cap = cap;
    }
    maps[map_count].id = id; maps[map_count].addr = addr; maps[map_count].size = size;
    map_count++;
    return 0;
}

/* ---------- API SysV ---------- */

EXPORT int shmget(key_t key, size_t size, int flags) {
    struct sysvshm_req q; struct sysvshm_resp r;
    memset(&q, 0, sizeof(q));
    q.op = SYSVSHM_OP_GET; q.key = (int32_t)key; q.flags = flags; q.size = size;
    if (rpc(&q, &r, NULL) < 0) return -1;
    if (r.status < 0) return fail_status(r.status);
    return r.status;
}

EXPORT void *shmat(int shmid, const void *shmaddr, int shmflg) {
    struct sysvshm_req q; struct sysvshm_resp r; int fd;
    memset(&q, 0, sizeof(q));
    q.op = SYSVSHM_OP_ATTACH; q.id = shmid;
    if (rpc(&q, &r, &fd) < 0) return (void *)-1;
    if (r.status < 0) { errno = -r.status; return (void *)-1; }
    if (fd < 0) { errno = EIO; return (void *)-1; }

    int prot = PROT_READ;
    if (!(shmflg & SHM_RDONLY)) prot |= PROT_WRITE;
    if (shmflg & SHM_EXEC) prot |= PROT_EXEC;

    void *hint = NULL; int mflags = MAP_SHARED;
    if (shmaddr) {
        uintptr_t a = (uintptr_t)shmaddr;
        if (shmflg & SHM_RND) a &= ~((uintptr_t)getpagesize() - 1);
        hint = (void *)a;
        mflags |= (shmflg & SHM_REMAP) ? MAP_FIXED : MAP_FIXED_NOREPLACE;
    }

    void *addr = mmap(hint, (size_t)r.size, prot, mflags, fd, 0);
    int merr = errno;
    close(fd);

    pthread_mutex_lock(&lock);
    int added = addr == MAP_FAILED ? -1 : map_add(shmid, addr, (size_t)r.size);
    pthread_mutex_unlock(&lock);

    if (addr == MAP_FAILED || added < 0) {
        if (addr != MAP_FAILED) { munmap(addr, (size_t)r.size); merr = ENOMEM; }
        struct sysvshm_req d; memset(&d, 0, sizeof(d));
        d.op = SYSVSHM_OP_DETACH; d.id = shmid;
        rpc(&d, &r, NULL);
        errno = merr;
        return (void *)-1;
    }
    return addr;
}

EXPORT int shmdt(const void *shmaddr) {
    int id = -1; size_t size = 0;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < map_count; i++) {
        if (maps[i].addr == shmaddr) {
            id = maps[i].id; size = maps[i].size;
            maps[i] = maps[--map_count];
            break;
        }
    }
    pthread_mutex_unlock(&lock);
    if (id < 0) { errno = EINVAL; return -1; }

    munmap((void *)shmaddr, size);
    struct sysvshm_req q; struct sysvshm_resp r;
    memset(&q, 0, sizeof(q));
    q.op = SYSVSHM_OP_DETACH; q.id = id;
    rpc(&q, &r, NULL); /* se o broker sumiu, não há o que liberar */
    return 0;
}

EXPORT int shmctl(int shmid, int cmd, struct shmid_ds *buf) {
    struct sysvshm_req q; struct sysvshm_resp r;
    memset(&q, 0, sizeof(q));
    q.id = shmid;

    switch (cmd) {
    case IPC_RMID:
        q.op = SYSVSHM_OP_RMID;
        if (rpc(&q, &r, NULL) < 0) return -1;
        return r.status < 0 ? fail_status(r.status) : 0;

    case IPC_STAT:
        if (!buf) { errno = EFAULT; return -1; }
        q.op = SYSVSHM_OP_STAT;
        if (rpc(&q, &r, NULL) < 0) return -1;
        if (r.status < 0) return fail_status(r.status);
        memset(buf, 0, sizeof(*buf));
        buf->shm_segsz = (size_t)r.size;
        buf->shm_nattch = (shmatt_t)r.nattch;
        buf->shm_cpid = (pid_t)r.cpid;
        buf->shm_lpid = getpid();
        buf->shm_ctime = (time_t)r.ctime;
        buf->shm_perm.__key = (key_t)r.key;
        buf->shm_perm.uid = geteuid();
        buf->shm_perm.gid = getegid();
        buf->shm_perm.cuid = (uid_t)r.cuid;
        buf->shm_perm.cgid = getegid();
        buf->shm_perm.mode = 0600;
        return 0;

    case IPC_SET:  /* o broker é do app: dono/modo não mudam */
    case SHM_LOCK:
    case SHM_UNLOCK:
        return 0;

    default:
        errno = EINVAL;
        return -1;
    }
}

/* Nomes usados por libandroid-shmem (bionic); mantidos por compatibilidade com o original. */
EXPORT int libandroid_shmget(key_t key, size_t size, int flags) { return shmget(key, size, flags); }
EXPORT void *libandroid_shmat(int shmid, const void *shmaddr, int shmflg) { return shmat(shmid, shmaddr, shmflg); }
EXPORT int libandroid_shmdt(const void *shmaddr) { return shmdt(shmaddr); }
EXPORT int libandroid_shmctl(int shmid, int cmd, struct shmid_ds *buf) { return shmctl(shmid, cmd, buf); }
