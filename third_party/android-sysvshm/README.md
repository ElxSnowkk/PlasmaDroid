# android-sysvshm (PlasmaDroid)

SysV shared memory (`shmget/shmat/shmdt/shmctl`) para o guest Debian/glibc, que roda sob PRoot dentro do app.
O Android bloqueia essas syscalls (seccomp), então elas são traduzidas para um broker no app.

Base: SysVSHM do Winlator, que por sua vez vem de https://github.com/pelya/android-shmem. Esta versão reescreve o
protocolo para o caso do desktop (vários processos no guest se falando por shmid, como o MIT-SHM do Xwayland).

| Arquivo | O que é |
|---|---|
| `sysvshm_proto.h` | protocolo cliente <-> broker (mensagens fixas, fd por SCM_RIGHTS) |
| `sysvshm_server.c` | broker, Bionic. Cria a memória com `ASharedMemory_create` (fallback `memfd`) |
| `android_sysvshm.c` | cliente, glibc aarch64, usado por `LD_PRELOAD` |
| `tests/` | teste de integração que roda no host (`scripts/build-sysvshm.sh --host-test`) |

Build: `scripts/build-sysvshm.sh` (chamado pelo `scripts/build-all.sh`). Detalhes em `docs/SYSVSHM.md`.
