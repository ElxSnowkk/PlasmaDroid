# PRoot 5.5.0 (build "lowmem") dentro do ASDroid

Fonte do PRoot (proot-me/proot, commit 25dc6a3, versão 5.5.0) com `src/GNUmakefile` ajustado para pouca RAM
(sem debug info, `--param=ggc-*` no gcc, `--no-keep-memory` no ld) e o wrapper `build-lowmem.sh`
(limita jobs e memória virtual por processo). Licença GPL-2.0-or-later (ver COPYING): o código-fonte fica neste repositório.

Compilar e instalar no app: `scripts/build-proot.sh` (roda no Termux). Detalhes em docs/PROOT.md.

## Patches ASDroid (tracee/event.c)
- `SIGSYS` do seccomp do Android (filtro por app com SECCOMP_RET_TRAP, `si_code=SYS_SECCOMP`, `si_errno=0`) é interceptado: a syscall bloqueada
  devolve `-ENOSYS` e o guest continua (glibc/musl caem para a syscall antiga). Sem isso o guest morria calado e o PRoot saía com 255.
- Se o vpid 1 morre por sinal, o PRoot sai com `128+sinal` (antes: 255) e avisa em stderr para SIGSYS/SEGV/ILL/BUS/ABRT, mesmo sem `-v`.
- O SIGSYS é tratado escrevendo os registradores direto (PTRACE_SETREGSET), sem `push_regs()`: este restaura SP/argumentos antigos
  (`restore_original_regs`) e matava o guest com SIGBUS no modo seccomp.
- Syscalls que o próprio PRoot anula (`PR_void`, nº -1 em `NT_ARM_SYSTEM_CALL`) quando a tradução falha (ENOENT etc.) também disparam o SIGSYS do filtro
  do Android, só que depois do estágio de saída, quando o PRoot já gravou o errno correto. O handler agora só engole o sinal nesse caso e não sobrescreve o
  resultado com `-ENOSYS` (antes, todo ENOENT virava "Function not implemented": ld.so, busca no PATH do dash, `ls`...).
