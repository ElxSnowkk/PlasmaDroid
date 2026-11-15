#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "sysvshm_proto.h"

#ifndef IPC_CREAT
#define IPC_CREAT 01000
#define IPC_EXCL  02000
#endif

#define MAX_SEGMENTS 8192
#define MAX_CLIENTS  256
#define REAP_PERIOD_MS 5000
#define MAX_SEG_SIZE (1ULL << 31)

struct attach { pid_t pid; int count; };

struct segment {
    int      id;
    int      key;
    int      fd;
    size_t   size;
    int      removed;
    pid_t    cpid;
    uid_t    cuid;
    time_t   ctime;
    int      nattach_len, nattach_cap;
    struct attach *attaches;
};

static struct segment **segs;
static int seg_count;
static int next_id = 1;
static uid_t my_uid;
static volatile sig_atomic_t quit_flag;

typedef int (*ashm_create_fn)(const char *, size_t);

static int create_region(size_t size) {
    static ashm_create_fn fn;
    static int resolved;
    if (!resolved) {
        resolved = 1;
        void *h = dlopen("libandroid.so", RTLD_NOW);
        if (h) fn = (ashm_create_fn)dlsym(h, "ASharedMemory_create");
    }
    if (fn) {
        int fd = fn("plasma-sysv", size);
        if (fd >= 0) return fd;
    }
    int fd = (int)syscall(SYS_memfd_create, "plasma-sysv", 1u );
    if (fd < 0) return -1;
    if (ftruncate(fd, (off_t)size) < 0) { int e = errno; close(fd); errno = e; return -1; }
    return fd;
}

static struct segment *find_id(int id) {
    for (int i = 0; i < seg_count; i++) if (segs[i]->id == id) return segs[i];
    return NULL;
}

static struct segment *find_key(int key) {
    if (key == 0) return NULL;
    for (int i = 0; i < seg_count; i++) if (segs[i]->key == key && !segs[i]->removed) return segs[i];
    return NULL;
}

static int total_attach(const struct segment *s) {
    int n = 0;
    for (int i = 0; i < s->nattach_len; i++) n += s->attaches[i].count;
    return n;
}

static void seg_free(int index) {
    struct segment *s = segs[index];
    if (s->fd >= 0) close(s->fd);
    free(s->attaches);
    free(s);
    seg_count--;
    memmove(&segs[index], &segs[index + 1], (size_t)(seg_count - index) * sizeof(*segs));
}

static void maybe_collect(int index) {
    struct segment *s = segs[index];
    if (s->removed && total_attach(s) == 0) seg_free(index);
}

static struct attach *attach_slot(struct segment *s, pid_t pid, int create) {
    for (int i = 0; i < s->nattach_len; i++) if (s->attaches[i].pid == pid) return &s->attaches[i];
    if (!create) return NULL;
    if (s->nattach_len == s->nattach_cap) {
        int cap = s->nattach_cap ? s->nattach_cap * 2 : 4;
        struct attach *n = realloc(s->attaches, (size_t)cap * sizeof(*n));
        if (!n) return NULL;
        s->attaches = n; s->nattach_cap = cap;
    }
    struct attach *a = &s->attaches[s->nattach_len++];
    a->pid = pid; a->count = 0;
    return a;
}

static int pid_alive(pid_t pid) {
    return kill(pid, 0) == 0 || errno == EPERM;
}

static void reap_dead(void) {
    for (int i = seg_count - 1; i >= 0; i--) {
        struct segment *s = segs[i];
        for (int j = s->nattach_len - 1; j >= 0; j--) {
            if (!pid_alive(s->attaches[j].pid)) {
                s->attaches[j] = s->attaches[--s->nattach_len];
            }
        }
        maybe_collect(i);
    }
}

static void fill_info(struct sysvshm_resp *r, const struct segment *s) {
    r->size = s->size; r->nattch = total_attach(s); r->key = s->key;
    r->cpid = (int32_t)s->cpid; r->ctime = (int64_t)s->ctime; r->cuid = (uint32_t)s->cuid;
}

static int do_get(const struct sysvshm_req *q, pid_t peer, uid_t peer_uid, struct sysvshm_resp *r) {
    size_t size = (size_t)q->size;
    if (q->key != 0) {
        struct segment *s = find_key(q->key);
        if (s) {
            if ((q->flags & IPC_CREAT) && (q->flags & IPC_EXCL)) return -EEXIST;
            if (size > s->size) return -EINVAL;
            fill_info(r, s);
            return s->id;
        }
        if (!(q->flags & IPC_CREAT)) return -ENOENT;
    }
    if (size == 0 || size > MAX_SEG_SIZE) return -EINVAL;
    if (seg_count >= MAX_SEGMENTS) { reap_dead(); if (seg_count >= MAX_SEGMENTS) return -ENOSPC; }

    long page = sysconf(_SC_PAGESIZE);
    size = (size + (size_t)page - 1) / (size_t)page * (size_t)page;

    int fd = create_region(size);
    if (fd < 0) return -(errno ? errno : ENOMEM);

    struct segment *s = calloc(1, sizeof(*s));
    struct segment **ns = realloc(segs, (size_t)(seg_count + 1) * sizeof(*segs));
    if (!s || !ns) { free(s); if (ns) segs = ns; close(fd); return -ENOMEM; }
    segs = ns;

    do { s->id = next_id; next_id = next_id >= 0x7fffff00 ? 1 : next_id + 1; } while (find_id(s->id));
    s->key = q->key; s->fd = fd; s->size = size; s->cpid = peer; s->cuid = peer_uid; s->ctime = time(NULL);
    segs[seg_count++] = s;
    fill_info(r, s);
    return s->id;
}

static int do_attach(const struct sysvshm_req *q, pid_t peer, struct sysvshm_resp *r, int *fd_out) {
    struct segment *s = find_id(q->id);
    if (!s) return -EINVAL;
    struct attach *a = attach_slot(s, peer, 1);
    if (!a) return -ENOMEM;
    a->count++;
    fill_info(r, s);
    *fd_out = s->fd;
    return 0;
}

static int do_detach(const struct sysvshm_req *q, pid_t peer) {
    for (int i = 0; i < seg_count; i++) {
        struct segment *s = segs[i];
        if (s->id != q->id) continue;
        struct attach *a = attach_slot(s, peer, 0);
        if (!a && s->nattach_len > 0) a = &s->attaches[0];
        if (a && a->count > 0) {
            a->count--;
            if (a->count == 0) {
                int idx = (int)(a - s->attaches);
                s->attaches[idx] = s->attaches[--s->nattach_len];
            }
        }
        maybe_collect(i);
        return 0;
    }
    return 0;
}

static int do_rmid(const struct sysvshm_req *q) {
    for (int i = 0; i < seg_count; i++) {
        if (segs[i]->id != q->id) continue;
        segs[i]->removed = 1;
        segs[i]->key = 0;
        maybe_collect(i);
        return 0;
    }
    return -EINVAL;
}

static int do_stat(const struct sysvshm_req *q, struct sysvshm_resp *r) {
    struct segment *s = find_id(q->id);
    if (!s) return -EINVAL;
    fill_info(r, s);
    return 0;
}

static int read_full(int fd, void *buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t k = read(fd, (char *)buf + got, n - got);
        if (k == 0) return 0;
        if (k < 0) { if (errno == EINTR) continue; return -1; }
        got += (size_t)k;
    }
    return 1;
}

static int send_resp(int fd, const struct sysvshm_resp *r, int pass_fd) {
    struct iovec iov = { .iov_base = (void *)r, .iov_len = sizeof(*r) };
    union { struct cmsghdr h; char buf[CMSG_SPACE(sizeof(int))]; } ctl;
    struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1 };
    if (pass_fd >= 0) {
        memset(&ctl, 0, sizeof(ctl));
        msg.msg_control = ctl.buf; msg.msg_controllen = sizeof(ctl.buf);
        struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
        c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS; c->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c), &pass_fd, sizeof(int));
    }
    for (;;) {
        ssize_t k = sendmsg(fd, &msg, MSG_NOSIGNAL);
        if (k < 0 && errno == EINTR) continue;
        return k == (ssize_t)sizeof(*r) ? 0 : -1;
    }
}

static int serve_one(int fd) {
    struct sysvshm_req q;
    int rd = read_full(fd, &q, sizeof(q));
    if (rd <= 0) return -1;

    struct ucred cred;
    socklen_t cl = sizeof(cred);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &cl) < 0) return -1;
    if (cred.uid != my_uid) return -1;

    struct sysvshm_resp r;
    memset(&r, 0, sizeof(r));
    int pass = -1;

    switch (q.op) {
    case SYSVSHM_OP_GET:    r.status = do_get(&q, cred.pid, cred.uid, &r); break;
    case SYSVSHM_OP_ATTACH: r.status = do_attach(&q, cred.pid, &r, &pass); break;
    case SYSVSHM_OP_DETACH: r.status = do_detach(&q, cred.pid); break;
    case SYSVSHM_OP_RMID:   r.status = do_rmid(&q); break;
    case SYSVSHM_OP_STAT:   r.status = do_stat(&q, &r); break;
    case SYSVSHM_OP_PING:   r.status = SYSVSHM_PROTO_VERSION; break;
    default:                r.status = -ENOSYS; break;
    }
    if (r.status < 0) { r.size = 0; r.nattch = 0; }
    return send_resp(fd, &r, r.status == 0 ? pass : -1);
}

static void on_signal(int s) { (void)s; quit_flag = 1; }

static int listen_on(const char *path) {
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(sa.sun_path)) { fprintf(stderr, "sysvshm: caminho do socket longo demais (%zu)\n", strlen(path)); return -1; }
    strcpy(sa.sun_path, path);

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) { perror("sysvshm: socket"); return -1; }
    unlink(path);
    mode_t old = umask(0077);
    int rc = bind(fd, (struct sockaddr *)&sa, sizeof(sa));
    umask(old);
    if (rc < 0) { perror("sysvshm: bind"); close(fd); return -1; }
    chmod(path, 0600);
    if (listen(fd, 64) < 0) { perror("sysvshm: listen"); close(fd); return -1; }
    return fd;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "uso: %s <socket> [-f]\n", argv[0]); return 2; }
    const char *path = argv[1];

    my_uid = geteuid();
    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa; memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, NULL); sigaction(SIGINT, &sa, NULL); sigaction(SIGHUP, &sa, NULL);
    const pid_t parent = getppid();
    time_t last_reap = 0;

    int lfd = listen_on(path);
    if (lfd < 0) return 1;
    fprintf(stderr, "sysvshm: pronto em %s (proto v%d)\n", path, SYSVSHM_PROTO_VERSION);

    struct pollfd pfd[1 + MAX_CLIENTS];
    int nclients = 0;
    pfd[0].fd = lfd; pfd[0].events = POLLIN;

    while (!quit_flag) {
        int rc = poll(pfd, (nfds_t)(1 + nclients), REAP_PERIOD_MS);
        if (rc < 0) { if (errno == EINTR) continue; perror("sysvshm: poll"); break; }
        if (rc == 0) {
            reap_dead(); last_reap = time(NULL);
            if (getppid() != parent) { fprintf(stderr, "sysvshm: app encerrou, saindo\n"); break; }
            continue;
        }

        for (int i = nclients; i >= 1; i--) {
            if (!(pfd[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            if (serve_one(pfd[i].fd) < 0) {
                close(pfd[i].fd);
                pfd[i] = pfd[nclients];
                nclients--;
            }
        }
        if (pfd[0].revents & POLLIN) {
            int c = accept4(lfd, NULL, NULL, SOCK_CLOEXEC);
            if (c >= 0) {
                if (nclients >= MAX_CLIENTS) close(c);
                else { nclients++; pfd[nclients].fd = c; pfd[nclients].events = POLLIN; pfd[nclients].revents = 0; }
            }
        }
        if (time(NULL) - last_reap >= 1) { reap_dead(); last_reap = time(NULL); }
    }

    unlink(path);
    return 0;
}
