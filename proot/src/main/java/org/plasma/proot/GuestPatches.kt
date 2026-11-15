package org.plasma.proot

import java.io.File

/**
 * GERADO por scripts/gen-guest-patches.py a partir de scripts/guest-patches/ — não edite à mão.
 * Instala no guest Debian o comando `plasma` (plasma install <app>, plasma patch ..., plasma run ...) e os
 * patches que fazem apps como o Kdenlive funcionarem sob o PRoot. Idempotente.
 */
object GuestPatches {
    private class Entry(val path: String, val text: String, val exec: Boolean)

    private val ENTRIES = listOf(
        Entry("usr/local/bin/plasma", """#!/bin/bash
# plasma — gerenciador de apps e patches do PlasmaDroid (dentro do guest Debian)
L=/usr/local/lib/plasma
. "§L/lib.sh"

usage() {
  cat <<'TXT'
uso:
  plasma install <app>              instala o app e aplica todos os patches necessários
  plasma list                       lista os apps disponíveis
  plasma patch list                 lista os patches e o estado de cada um
  plasma patch apply <nome|all>     (re)aplica um patch
  plasma run <comando...>           roda um comando com o ambiente do PlasmaDroid (dbus + wayland + vulkan)
TXT
}

case "§{1:-}" in
  install)
    app="§{2:-}"
    [ -n "§app" ] || { usage; exit 1; }
    f="§L/apps/§app.sh"
    if [ ! -f "§f" ]; then
      say "app desconhecido: §app"; echo "disponíveis:"; list_apps; exit 1
    fi
    need_root
    exec bash "§f"
    ;;
  list)
    list_apps
    ;;
  patch)
    case "§{2:-}" in
      list|"") list_patches ;;
      apply)
        need_root
        [ -n "§{3:-}" ] || { usage; exit 1; }
        if [ "§3" = all ]; then apply_all_patches; else apply_patch "§3"; fi
        ;;
      *) usage; exit 1 ;;
    esac
    ;;
  run)
    shift
    [ §# -gt 0 ] || { usage; exit 1; }
    . "§L/env.sh"
    exec "§@"
    ;;
  *)
    usage
    [ -z "§{1:-}" ] || exit 1
    ;;
esac
""".replace('§', '$'), true),
        Entry("usr/local/lib/plasma/lib.sh", """# Funções comuns do "plasma" (carregado com `. lib.sh`). Requer bash.
L=/usr/local/lib/plasma
STATE=/var/lib/plasma
BASE_PATCHES="dpkg-systemd dbus-session gl-vulkan wayland-env"

say() { echo "[plasma] §*"; }
need_root() { [ "§(id -u)" = 0 ] || { say "rode como root"; exit 1; }; }

list_apps() {
  local f n
  for f in "§L"/apps/*.sh; do
    [ -f "§f" ] || continue
    n=§(basename "§f" .sh)
    printf '  %-12s %s\n' "§n" "§(sed -n 's/^# desc: *//p' "§f" | head -1)"
  done
}

list_patches() {
  local f n st
  for f in "§L"/patches/*.sh; do
    [ -f "§f" ] || continue
    n=§(basename "§f" .sh)
    if [ -f "§STATE/patches/§n" ]; then st="aplicado "; else st="pendente "; fi
    printf '  %-14s %s %s\n' "§n" "§st" "§(sed -n 's/^# desc: *//p' "§f" | head -1)"
  done
}

apply_patch() {
  local n="§1" f="§L/patches/§1.sh"
  [ -f "§f" ] || { say "patch inexistente: §n"; return 1; }
  say "patch: §n"
  if bash "§f"; then
    mkdir -p "§STATE/patches"; date -u +%FT%TZ > "§STATE/patches/§n"
  else
    say "patch §n falhou"; return 1
  fi
}

apply_all_patches() {
  local f n rc=0
  for f in "§L"/patches/*.sh; do
    [ -f "§f" ] || continue
    apply_patch "§(basename "§f" .sh)" || rc=1
  done
  return §rc
}

apply_base_patches() {
  local p
  for p in §BASE_PATCHES; do apply_patch "§p" || return 1; done
}

# Neutraliza o postinst dos pacotes que abortaram com status 134
# (systemd: Assertion 'mntid >= 0' failed). Lê o log do dpkg em §1.
# Retorna 0 se tratou algum pacote.
heal() {
  local n=0 pkg base f
  while read -r pkg; do
    [ -n "§pkg" ] || continue
    base="§{pkg%%:*}"
    for f in "/var/lib/dpkg/info/§pkg.postinst" "/var/lib/dpkg/info/§base.postinst"; do
      [ -f "§f" ] || continue
      grep -q 'plasma-stub' "§f" && continue
      mkdir -p "§STATE/postinst-orig"
      cp "§f" "§STATE/postinst-orig/§(basename "§f")"
      printf '#!/bin/sh\n# plasma-stub\nexit 0\n' > "§f"; chmod 755 "§f"
      n=§((n+1)); say "postinst de §pkg ignorado (abort do systemd no PRoot)"
    done
  done < <(awk '/error processing package/ {p=§5} /post-installation script subprocess returned error exit status 134/ && p!="" {print p; p=""}' "§1" | sort -u)
  [ "§n" -gt 0 ]
}

# apt install tolerante: se um postinst abortar (134), ignora e continua.
apt_install() {
  local log=/tmp/plasma-apt.log try rc
  export DEBIAN_FRONTEND=noninteractive
  apt-get update || say "aviso: apt-get update falhou, tentando mesmo assim"
  for try in 1 2 3 4 5 6; do
    apt-get install -y "§@" 2>&1 | tee "§log"; rc=§{PIPESTATUS[0]}
    [ "§rc" -eq 0 ] && return 0
    heal "§log" || break
    dpkg --configure -a 2>&1 | tee "§log" || true
  done
  say "a instalação falhou. Últimas linhas de §log:"; tail -15 "§log"
  return 1
}
""".replace('§', '$'), false),
        Entry("usr/local/lib/plasma/patches/dbus-session.sh", """#!/bin/bash
# desc: barramento D-Bus de sessão que funciona no PRoot (autenticação ANONYMOUS) + comando plasma-dbus
set -u
mkdir -p /usr/local/lib/plasma
cat > /usr/local/lib/plasma/dbus-session.conf <<'XML'
<busconfig>
  <type>session</type>
  <listen>unix:path=/tmp/dbus-plasma</listen>
  <auth>ANONYMOUS</auth>
  <allow_anonymous/>
  <policy context="default">
    <allow send_destination="*" eavesdrop="true"/>
    <allow eavesdrop="true"/>
    <allow own="*"/>
  </policy>
</busconfig>
XML
cat > /usr/local/bin/plasma-dbus <<'SH'
#!/bin/sh
# plasma-dbus — garante o barramento de sessão e imprime o endereço (use: export DBUS_SESSION_BUS_ADDRESS=§(plasma-dbus))
SOCK=/tmp/dbus-plasma
CONF=/usr/local/lib/plasma/dbus-session.conf
ADDR="unix:path=§SOCK"
alive() {
  DBUS_SESSION_BUS_ADDRESS="§ADDR" timeout 3 dbus-send --session --print-reply \
    --dest=org.freedesktop.DBus /org/freedesktop/DBus org.freedesktop.DBus.ListNames >/dev/null 2>&1
}
if ! alive; then
  pkill -f "dbus-daemon --config-file=§CONF" 2>/dev/null
  rm -f "§SOCK"
  # o aviso "Failed to set fd limit" é inofensivo (RLIMIT_NOFILE fixo no Android)
  dbus-daemon --config-file="§CONF" --fork >/dev/null 2>&1
fi
echo "§ADDR"
SH
chmod 755 /usr/local/bin/plasma-dbus
""".replace('§', '$'), true),
        Entry("usr/local/lib/plasma/patches/dpkg-systemd.sh", """#!/bin/bash
# desc: dpkg sem systemd real: stubs de sysusers/tmpfiles, policy-rc.d e usuário messagebus (mount id + audit no PRoot)
set -u
printf '#!/bin/sh\nexit 101\n' > /usr/sbin/policy-rc.d; chmod 755 /usr/sbin/policy-rc.d

# systemd-sysusers/tmpfiles abortam com "Assertion 'mntid >= 0' failed" quando o mount id não está disponível.
# O divert vale também para quando o pacote systemd ainda vai ser instalado.
for b in systemd-sysusers systemd-tmpfiles; do
  p=/usr/bin/§b
  if ! dpkg-divert --list 2>/dev/null | grep -q "diversion of §p to"; then
    dpkg-divert --local --rename --divert "§p.distrib" --add "§p" >/dev/null
  fi
  printf '#!/bin/sh\nexit 0\n' > "§p"; chmod 755 "§p"
done

# useradd/groupadd falham com "Cannot open audit interface" (netlink de auditoria bloqueado pelo Android):
# cria os usuários de sistema direto nos arquivos.
ensure_sys_user() {
  local n="§1" id gid
  getent passwd "§n" >/dev/null && return 0
  gid=§(getent group "§n" | cut -d: -f3)
  if [ -z "§gid" ]; then
    id=100
    while getent passwd "§id" >/dev/null || getent group "§id" >/dev/null; do id=§((id+1)); done
    gid=§id
    echo "§n:x:§gid:" >> /etc/group
    [ -f /etc/gshadow ] && echo "§n:!::" >> /etc/gshadow
  fi
  id=§gid
  while getent passwd "§id" >/dev/null; do id=§((id+1)); done
  echo "§n:x:§id:§gid::/nonexistent:/usr/sbin/nologin" >> /etc/passwd
  [ -f /etc/shadow ] && echo "§n:*:20000:0:99999:7:::" >> /etc/shadow
  return 0
}
ensure_sys_user messagebus
""".replace('§', '$'), true),
        Entry("usr/local/lib/plasma/patches/gl-vulkan.sh", """#!/bin/bash
# desc: OpenGL pelo Incandescent (GL -> GLES do aparelho, com ANGLE -> Vulkan opcional) como único driver GL e padrão do guest; teste por etapas; comando plasma-gl
set -u
mkdir -p /usr/local/lib/plasma /var/lib/plasma
cat > /usr/local/bin/plasma-gl <<'SH'
#!/bin/bash
# plasma-gl — OpenGL dos apps Qt/KDE do guest.
#   incandescent  PADRÃO e único driver GL: libincandescent.so (Bionic, carregada pelo libhybris) traduz GL -> GLES do aparelho;
#                 com o ANGLE ele vai GLES -> Vulkan. O ANGLE é opcional: funciona com ele, sem ele, e se ele faltar ou falhar.
#   llvmpipe      OpenGL por CPU. Só entra quando você pede (PLASMA_GL=llvmpipe) ou, no modo auto, quando o teste prova que o
#                 Incandescent não consegue funcionar neste aparelho (para os apps não ficarem sem GL).
# Ordem do ANGLE (plasma-gl angle auto|on|off; PLASMA_ANGLE=auto|1|0; as Configurações do PlasmaDroid gravam o mesmo arquivo):
#   auto (padrão)  tenta o GLES nativo; se falhar e o ANGLE estiver instalado, tenta o ANGLE
#   on             tenta o ANGLE; se faltar ou falhar, usa o GLES nativo
#   off            nunca usa o ANGLE
# uso: plasma-gl detect [--force] | env [--no-probe] | status | diag | angle [auto|on|off] | default [on|off|status] | info
# Variáveis: PLASMA_GL=incandescent (PADRÃO, sempre)|auto (testa e pode cair no llvmpipe)|llvmpipe  PLASMA_GL_EXCLUSIVE=0 (não bloqueia o Mesa)  PLASMA_GL_WINDOW_TEST=0 (pula o teste de janela)
# Caminhos (sobrescrevíveis): PLASMA_INC_LIB, PLASMA_ANGLE_DIR, PLASMA_HYBRIS_DIR, PLASMA_GL_STATE (padrão /var/lib/plasma)
STATE="§{PLASMA_GL_STATE:-/var/lib/plasma}"
CACHE="§STATE/gl-backend"
LOG="§STATE/gl-probe.log"
INC_STATE="§STATE/incandescent"
ANGLE_FILE="§INC_STATE/angle"
STATUS_FILE="§INC_STATE/status"
GPU_REPORT="§INC_STATE/gpu"
PROFILE_D="§{PLASMA_GL_PROFILE_D:-/etc/profile.d/10-plasma-gl.sh}"
export XDG_RUNTIME_DIR="§{XDG_RUNTIME_DIR:-/run/plasma-wayland}"
export WAYLAND_DISPLAY="§{WAYLAND_DISPLAY:-wayland-0}"

INC_LIB="§{PLASMA_INC_LIB:-/usr/lib/plasmavk/bionic/incandescent/libincandescent.so}"
ANGLE_DIR="§{PLASMA_ANGLE_DIR:-/usr/lib/plasmavk/bionic/angle}"
HYBRIS_DIR="§{PLASMA_HYBRIS_DIR:-/usr/lib/plasmavk}"
FAIL_TTL="§{PLASMA_GL_FAIL_TTL:-3600}"   # um teste que reprovou vale por 1 h (um que passou vale até o ambiente mudar)

# ---------- ANGLE ----------
angle_pref() {
  local v="§{PLASMA_ANGLE:-}"
  [ -n "§v" ] || v=§(head -n1 "§ANGLE_FILE" 2>/dev/null)
  case "§v" in 1|on|true|yes|sim) echo on ;; 0|off|false|no|nao|não) echo off ;; *) echo auto ;; esac
}
angle_libs_ok() { [ -r "§ANGLE_DIR/libEGL_angle.so" ] && [ -r "§ANGLE_DIR/libGLESv2_angle.so" ]; }
angle_label() { # §1 = on|off efetivo
  case "§1" in on) echo "ligado (GLES -> Vulkan)" ;; *) echo "desligado (GLES nativo)" ;; esac
}
fsig() { stat -c '%Y:%s' "§1" 2>/dev/null || echo -; }
# o resultado vale só para esta combinação de biblioteca/ANGLE/ICD: se mudar, testa de novo
fingerprint() { echo "§{VK_DRIVER_FILES:-}~§{PLASMAVK_BIONIC_LIB:-}~§(fsig "§INC_LIB")~pref=§(angle_pref)~§(fsig "§ANGLE_DIR/libEGL_angle.so")~§(fsig "§ANGLE_DIR/libGLESv2_angle.so")"; }

# ---------- ambiente de cada backend ----------
# Cada linha é NOME=valor (um por linha); o `env` valida o nome antes de emitir `export`.
# §1 = backend, §2 = on|off (ANGLE efetivo)
backend_env() {
  local ld p
  case "§1" in
    incandescent)
      echo "LIBEGL=§INC_LIB"
      echo "LIBGLESV2=§INC_LIB"
      # INC_ANGLE_DIR vazio = "não use ANGLE" (o Incandescent usa o GLES nativo); INC_ANGLE=0|1 sobrepõe o config.json
      if [ "§{2:-off}" = on ]; then echo "INC_ANGLE_DIR=§ANGLE_DIR"; echo "INC_ANGLE=1"; else echo "INC_ANGLE_DIR="; echo "INC_ANGLE=0"; fi
      echo "INC_DIR_PATH=§{INC_DIR_PATH:-§INC_STATE}"
      echo "INC_PLUGIN_STATUS=§{INC_PLUGIN_STATUS:-§STATUS_FILE}"
      echo "INC_GPU_REPORT=§{INC_GPU_REPORT:-§GPU_REPORT}"
      echo "INC_RENDERER_TEMPLATE=§{INC_RENDERER_TEMPLATE:-{GPU\} (Incandescent)}"
      ld="§HYBRIS_DIR"
      for p in §{LD_LIBRARY_PATH//:/ }; do [ "§p" = "§HYBRIS_DIR" ] || ld="§ld:§p"; done
      echo "LD_LIBRARY_PATH=§ld"
      if [ "§{PLASMA_GL_EXCLUSIVE:-1}" != 0 ]; then
        # Incandescent como ÚNICO driver GL: o Mesa (DRI/Zink/llvmpipe) e o vendor do glvnd não podem fornecer GL por baixo.
        echo "LIBGL_DRIVERS_PATH=/var/empty/plasma-gl-no-mesa"
        echo "__EGL_VENDOR_LIBRARY_FILENAMES=/var/empty/plasma-gl-no-vendor.json"
      fi
      ;;
    llvmpipe)
      echo "LIBGL_ALWAYS_SOFTWARE=1"
      echo "GALLIUM_DRIVER=llvmpipe"
      # o libEGL do libhybris não pode sombrear o do Debian/Mesa
      ld=""
      for p in §{LD_LIBRARY_PATH//:/ }; do [ "§p" = "§HYBRIS_DIR" ] || ld="§{ld:+§ld:}§p"; done
      [ -n "§ld" ] && echo "LD_LIBRARY_PATH=§ld"
      ;;
  esac
}

# Variáveis que um backend NÃO pode herdar do outro
unset_for() {
  case "§1" in
    incandescent) echo "LIBGL_ALWAYS_SOFTWARE GALLIUM_DRIVER MESA_LOADER_DRIVER_OVERRIDE LIBGL_KOPPER_DISABLE MESA_GL_VERSION_OVERRIDE MESA_GLSL_VERSION_OVERRIDE ZINK_DESCRIPTORS" ;;
    llvmpipe) echo "LIBEGL LIBGLESV2 INC_ANGLE INC_ANGLE_DIR INC_DIR_PATH INC_PLUGIN_STATUS INC_GPU_REPORT INC_RENDERER_TEMPLATE LIBGL_DRIVERS_PATH __EGL_VENDOR_LIBRARY_FILENAMES MESA_LOADER_DRIVER_OVERRIDE LIBGL_KOPPER_DISABLE" ;;
  esac
}

# ---------- testes ----------
# eglinfo com o backend §1 (ANGLE §2). Saída bruta completa em §LOG; guarda o texto em §EGLINFO_OUT.
RUN_RC=0
EGLINFO_OUT=""
run_eglinfo() {
  local tmp sb=""
  tmp=§(mktemp /tmp/plasma-gl.XXXXXX) || return 1
  # lista NOME=valor em array (um item por linha): valores com espaço, como "{GPU} (Incandescent)", não podem sofrer word splitting
  local -a benv=(); mapfile -t benv < <(backend_env "§1" "§2")
  command -v stdbuf >/dev/null 2>&1 && sb="stdbuf -oL -eL"
  mkdir -p "§INC_STATE" 2>/dev/null; rm -f "§GPU_REPORT" 2>/dev/null
  { env "§{benv[@]}" EGL_PLATFORM=wayland EGL_LOG_LEVEL=debug LIBGL_DEBUG=verbose §sb timeout "§{EGL_TMO:-25}" eglinfo -B >"§tmp" 2>&1; RUN_RC=§?; } 2>/dev/null
  mkdir -p "§STATE" 2>/dev/null
  { echo "### §(date '+%F %T') backend=§1 angle=§{2:-off} rc=§RUN_RC"; env | grep -E '^(VK_|MESA_|PLASMAVK_|HYBRIS_|WAYLAND_|XDG_RUNTIME|LIBEGL|LIBGLESV2|INC_|LD_LIBRARY)' | sort; echo "---"; cat "§tmp"; } >> "§LOG" 2>/dev/null
  # o log não cresce sem limite
  [ "§(stat -c %s "§LOG" 2>/dev/null || echo 0)" -gt 400000 ] && tail -c 200000 "§LOG" > "§LOG.tmp" && mv -f "§LOG.tmp" "§LOG"
  EGLINFO_OUT=§(cat "§tmp"); rm -f "§tmp"
}

# bloco "Wayland platform:" do texto em §1 (vazio se não houver)
wayland_block() { printf '%s\n' "§1" | awk '/^[A-Za-z0-9 ]+ platform:/ {p=(§1=="Wayland")} p'; }

# Teste de janela: abre um cliente EGL Wayland de verdade e vê se ele apresenta. Só roda se houver es2gears_wayland/eglgears_wayland.
WINDOW_STATE=skip
window_probe() { # §1 backend, §2 angle
  WINDOW_STATE=skip
  [ "§{PLASMA_GL_WINDOW_TEST:-1}" = 0 ] && return 0
  local t="" c tmp rc
  for c in es2gears_wayland eglgears_wayland; do command -v "§c" >/dev/null 2>&1 && { t="§c"; break; }; done
  [ -n "§t" ] || return 0
  [ -S "§XDG_RUNTIME_DIR/§WAYLAND_DISPLAY" ] || return 0
  tmp=§(mktemp /tmp/plasma-gl.XXXXXX) || return 0
  mkdir -p "§INC_STATE" 2>/dev/null
  local -a benv=(); mapfile -t benv < <(backend_env "§1" "§2")
  { env "§{benv[@]}" EGL_PLATFORM=wayland timeout 6 "§t" >"§tmp" 2>&1; rc=§?; } 2>/dev/null
  { echo "### janela (§t) rc=§rc"; cat "§tmp"; } >> "§LOG" 2>/dev/null
  # rc 124 = ainda desenhando quando o timeout cortou; qualquer outra saída é janela que não abriu ou caiu
  if [ "§rc" -eq 124 ] && ! grep -Eqi 'failed|error|cannot|can.t |unable|bad_native|not supported|segmentation|abort' "§tmp"; then WINDOW_STATE=ok
  else WINDOW_STATE="fail:§t rc=§rc §(grep -Ei 'failed|error|cannot|unable|bad_native' "§tmp" | head -n1 | cut -c1-120)"; fi
  rm -f "§tmp"
  return 0
}

# 0 = Incandescent funcionando; 1 = não serve (motivo em PROBE_WHY); 2 = não dá para testar (eglinfo ausente)
PROBE_WHY=""; PROBE_GPU=""; PROBE_WL=""
probe() { # §1 = on|off
  local ang="§{1:-off}" blk lines r
  PROBE_WHY=""; PROBE_GPU=""; PROBE_WL=""
  command -v eglinfo >/dev/null 2>&1 || return 2
  [ -r "§INC_LIB" ] || { PROBE_WHY="libincandescent.so não encontrada em §INC_LIB (PLASMA_INC_LIB)"; return 1; }
  ls "§HYBRIS_DIR"/libEGL.so* >/dev/null 2>&1 || { PROBE_WHY="libEGL do libhybris ausente em §HYBRIS_DIR (PLASMA_HYBRIS_DIR)"; return 1; }
  if [ "§ang" = on ] && ! angle_libs_ok; then PROBE_WHY="ANGLE ausente em §ANGLE_DIR (libEGL_angle.so, libGLESv2_angle.so)"; return 1; fi
  case "§{VK_DRIVER_FILES:-}§{VK_ICD_FILENAMES:-}" in
    *lvp*|*lavapipe*) [ "§ang" = on ] && { PROBE_WHY="o Vulkan do guest é o lavapipe (CPU): o ANGLE não teria GPU"; return 1; } ;;
  esac
  run_eglinfo incandescent "§ang"
  blk=§(wayland_block "§EGLINFO_OUT")
  if [ -n "§blk" ]; then lines=§(printf '%s\n' "§blk" | grep -i 'renderer'); PROBE_WL=sim
  else lines=§(printf '%s\n' "§EGLINFO_OUT" | grep -i 'renderer'); PROBE_WL=nao-listada; fi
  PROBE_GPU=§(sed -n 's/^GPU=//p' "§GPU_REPORT" 2>/dev/null | head -n1)
  if [ "§RUN_RC" -ge 128 ] && [ -z "§lines" ]; then PROBE_WHY="eglinfo morreu com o sinal §((RUN_RC-128)) antes de listar o renderer (veja §LOG)"; return 1; fi
  if [ "§RUN_RC" -eq 124 ] && [ -z "§lines" ]; then PROBE_WHY="eglinfo travou (timeout) ao criar o contexto (veja §LOG)"; return 1; fi
  if [ -z "§lines" ]; then
    if printf '%s\n' "§EGLINFO_OUT" | grep -qi 'failed to get.*display\|eglGetPlatformDisplay\|no display'; then
      PROBE_WHY="o EGL não criou o display Wayland (a libincandescent.so é antiga? recompile com o egl.cpp novo; veja §LOG)"
    else PROBE_WHY="o EGL Wayland não criou o contexto do Incandescent (veja §LOG)"; fi
    return 1
  fi
  printf '%s\n' "§lines" | grep -qi 'llvmpipe\|lavapipe\|swiftshader\|softpipe\|software' && { PROBE_WHY="o driver GLES caiu em dispositivo de CPU"; return 1; }
  # prova de que foi o Incandescent: o nome do renderer ou o relatório de GPU que só ele grava
  if ! printf '%s\n' "§lines" | grep -qi incandescent && [ -z "§PROBE_GPU" ]; then
    PROBE_WHY="o renderer não é o do Incandescent: §(printf '%s\n' "§lines" | head -n1 | cut -c1-100)"; return 1
  fi
  window_probe incandescent "§ang"
  case "§WINDOW_STATE" in fail:*) PROBE_WHY="contexto ok, mas a janela Wayland não apresentou (§{WINDOW_STATE#fail:}); falta a apresentação EGL->Wayland"; return 1 ;; esac
  return 0
}

write_status() { mkdir -p "§INC_STATE" 2>/dev/null; echo "§*" > "§STATUS_FILE" 2>/dev/null; }

# lê o cache: CB (backend) CFP (impressão) CANG (on|off) CTS (epoch). 0 se existe e é legível.
read_cache() {
  CB=""; CFP=""; CANG=off; CTS=0
  [ -f "§CACHE" ] || return 1
  IFS='|' read -r CB CFP CANG CTS < "§CACHE" 2>/dev/null
  case "§CB" in incandescent|llvmpipe) ;; *) return 1 ;; esac
  case "§CANG" in on|off) ;; *) CANG=off ;; esac
  case "§CTS" in ''|*[!0-9]*) CTS=0 ;; esac
  return 0
}

# Define BACKEND e ANGLE_EFF.
BACKEND=incandescent; ANGLE_EFF=off; VERIFIED=sim
detect() {
  local fp now pref a rc cands=() tried="" last_why=""
  fp=§(fingerprint); now=§(date +%s)
  [ "§{1:-}" = --force ] && rm -f "§CACHE"
  if read_cache && [ "§CFP" = "§fp" ]; then
    if [ "§CB" = incandescent ] || [ §((now - CTS)) -lt "§FAIL_TTL" ]; then BACKEND=§CB; ANGLE_EFF=§CANG; VERIFIED=sim; return 0; fi
  fi
  pref=§(angle_pref)
  case "§pref" in
    off)  cands=(off) ;;
    on)   if angle_libs_ok; then cands=(on off); else cands=(off); echo "[plasma-gl] ANGLE pedido, mas ausente em §ANGLE_DIR: seguindo sem ele" >&2; fi ;;
    *)    if angle_libs_ok; then cands=(off on); else cands=(off); fi ;;
  esac
  mkdir -p "§STATE" 2>/dev/null
  for a in "§{cands[@]}"; do
    probe "§a"; rc=§?
    if [ "§rc" -eq 2 ]; then
      echo "[plasma-gl] eglinfo ausente (apt install mesa-utils): não deu para testar; usando o Incandescent sem teste" >&2
      BACKEND=incandescent; VERIFIED=nao
      ANGLE_EFF=off; [ "§pref" = on ] && angle_libs_ok && ANGLE_EFF=on
      return 0
    fi
    if [ "§rc" -eq 0 ]; then
      BACKEND=incandescent; ANGLE_EFF=§a; VERIFIED=sim
      echo "incandescent|§fp|§a|§now" > "§CACHE"
      write_status "OK: §{PROBE_GPU:-GPU do aparelho} via Incandescent, ANGLE §(angle_label "§a"), Wayland: §PROBE_WL, janela: §{WINDOW_STATE%%:*}"
      echo "[plasma-gl] backend OpenGL: incandescent (§{PROBE_GPU:-GPU}, ANGLE §(angle_label "§a"))" >&2
      return 0
    fi
    last_why="§PROBE_WHY"; tried="§tried §{a}"
    echo "[plasma-gl] incandescent (ANGLE §(angle_label "§a")) não serviu: §PROBE_WHY" >&2
  done
  BACKEND=llvmpipe; ANGLE_EFF=off; VERIFIED=sim
  echo "llvmpipe|§fp|off|§now" > "§CACHE"
  write_status "FALHA: §last_why (testado ANGLE:§{tried}); usando llvmpipe"
  echo "[plasma-gl] o Incandescent não funciona agora (§last_why); usando llvmpipe (CPU). Force com PLASMA_GL=incandescent ou rode: plasma-gl diag" >&2
}

forced_angle() { # ANGLE efetivo quando o backend é forçado (sem teste)
  local p; p=§(angle_pref)
  if [ "§p" = on ] && angle_libs_ok; then echo on; return; fi
  if [ "§p" = auto ] && read_cache && [ "§CB" = incandescent ] && [ "§CANG" = on ] && angle_libs_ok; then echo on; return; fi
  echo off
}

resolve() { # §1 = --no-probe
  local c="§{PLASMA_GL:-incandescent}"
  case "§c" in
    zink|zink-nokopper) echo "[plasma-gl] o Zink foi substituído pelo Incandescent; usando incandescent" >&2; c=incandescent ;;
  esac
  VERIFIED=sim
  case "§c" in
    incandescent) BACKEND=incandescent; ANGLE_EFF=§(forced_angle) ;;
    llvmpipe)     BACKEND=llvmpipe; ANGLE_EFF=off ;;
    *)
      if [ "§{1:-}" = --no-probe ]; then
        if read_cache && [ "§CFP" = "§(fingerprint)" ]; then BACKEND=§CB; ANGLE_EFF=§CANG
        else BACKEND=incandescent; ANGLE_EFF=§(forced_angle); VERIFIED=nao; fi
      else detect >&2; fi ;;
  esac
}

emit_env() {
  local v l name
  mkdir -p "§INC_STATE" 2>/dev/null
  echo "unset §(unset_for "§BACKEND")"
  backend_env "§BACKEND" "§ANGLE_EFF" | while IFS= read -r l; do
    name="§{l%%=*}"
    case "§name" in ''|[0-9]*|*[!A-Za-z0-9_]*) continue ;; esac   # só NOME=valor válido: nada de lixo no eval
    case "§l" in *=*) ;; *) continue ;; esac
    printf 'export %s=%q\n' "§name" "§{l#*=}"
  done
  # sinaliza para os apps/Configurações qual backend está valendo
  echo "export PLASMA_GL_BACKEND=§BACKEND"
}

# Linha "OpenGL" do fastfetch (estilo PC): versão do GL + biblioteca [EGL] + backend em uso. A versão fica em cache.
cmd_info() {
  local how f key ver blk
  case "§BACKEND" in
    incandescent) if [ "§ANGLE_EFF" = on ]; then how="Incandescent → ANGLE → Vulkan"; else how="Incandescent → GLES nativo"; fi ;;
    *) how="llvmpipe (CPU)" ;;
  esac
  f="§INC_STATE/glinfo"; key="§BACKEND|§ANGLE_EFF|§(fsig "§INC_LIB")"; ver=""
  [ -r "§f" ] && [ "§(sed -n 1p "§f" 2>/dev/null)" = "§key" ] && ver=§(sed -n 2p "§f" 2>/dev/null)
  if [ -z "§ver" ] && command -v eglinfo >/dev/null 2>&1; then
    EGL_TMO="§{PLASMA_GL_INFO_TIMEOUT:-10}"
    run_eglinfo "§BACKEND" "§ANGLE_EFF"
    blk=§(wayland_block "§EGLINFO_OUT"); [ -n "§blk" ] || blk="§EGLINFO_OUT"
    ver=§(printf '%s\n' "§blk" | sed -n -e 's/^ *OpenGL compatibility profile version: *//p' -e 's/^ *OpenGL core profile version: *//p' -e 's/^ *OpenGL ES profile version: *//p' | head -n1)
    if [ -n "§ver" ]; then mkdir -p "§INC_STATE" 2>/dev/null; printf '%s\n%s\n' "§key" "§ver" > "§f" 2>/dev/null; fi
  fi
  echo "§{ver:-versão indisponível (instale mesa-utils-bin)} [EGL · §how]"
}

cmd_default() {
  case "§{1:-status}" in
    on)
      mkdir -p "§(dirname "§PROFILE_D")" 2>/dev/null
      cat > "§PROFILE_D" <<'PD'
# plasma-gl: o Incandescent é o OpenGL padrão do guest (não testa nada aqui: usa o último resultado guardado)
if [ "§{PLASMA_GL:-auto}" != llvmpipe ] && [ -z "§{PLASMA_GL_BACKEND:-}" ] && command -v plasma-gl >/dev/null 2>&1; then
  eval "§(plasma-gl env --no-probe 2>/dev/null)"
fi
PD
      chmod 644 "§PROFILE_D"; echo "Incandescent é o OpenGL padrão (§PROFILE_D)" ;;
    off) rm -f "§PROFILE_D"; echo "padrão removido (§PROFILE_D)" ;;
    *) if [ -f "§PROFILE_D" ]; then echo "padrão: ligado (§PROFILE_D)"; else echo "padrão: desligado (rode: plasma-gl default on)"; fi ;;
  esac
}

case "§{1:-env}" in
  detect) shift; detect "§@"; exit §? ;;
  env)
    shift; resolve "§{1:-}"
    emit_env ;;
  status)
    resolve
    echo "backend: §BACKEND§([ "§VERIFIED" = nao ] && echo ' (sem teste)')"
    echo "angle: preferência=§(angle_pref), efetivo=§(angle_label "§ANGLE_EFF"), libs=§(angle_libs_ok && echo presentes || echo ausentes)"
    echo "exclusivo: §([ "§{PLASMA_GL_EXCLUSIVE:-1}" != 0 ] && echo sim || echo não)   padrão do shell: §([ -f "§PROFILE_D" ] && echo ligado || echo desligado)"
    [ -f "§STATUS_FILE" ] && echo "último teste: §(head -n1 "§STATUS_FILE")"
    [ "§BACKEND" = incandescent ] && { run_eglinfo incandescent "§ANGLE_EFF"; blk=§(wayland_block "§EGLINFO_OUT"); [ -n "§blk" ] || blk="§EGLINFO_OUT"; printf '%s\n' "§blk" | grep -i 'renderer'; }
    ;;
  diag)
    echo "== ambiente do guest"
    env | grep -E '^(VK_|MESA_|PLASMAVK_|HYBRIS_|WAYLAND_|XDG_RUNTIME|PLASMA_|LIBGL_|LIBEGL|LIBGLESV2|INC_|LD_LIBRARY|__EGL)' | sort
    echo "== arquivos: §INC_LIB | §ANGLE_DIR | §HYBRIS_DIR/libEGL*"; ls -l "§INC_LIB" "§ANGLE_DIR" 2>&1 | head; ls -l "§HYBRIS_DIR"/libEGL.so* "§HYBRIS_DIR"/libGLESv2.so* 2>&1 | head
    echo "== plataforma EGL do libhybris"; ls "§HYBRIS_DIR"/libhybris/eglplatform_*.so 2>/dev/null || echo "nenhum eglplatform_*.so em §HYBRIS_DIR/libhybris (HYBRIS_EGLPLATFORM=§{HYBRIS_EGLPLATFORM:-<não definido>})"
    echo "== sockets Wayland"; ls -l "§XDG_RUNTIME_DIR" 2>&1 | head
    echo "== vulkaninfo"
    if command -v vulkaninfo >/dev/null 2>&1; then
      o=§(timeout 25 vulkaninfo 2>&1); echo "§o" | grep -E 'deviceName|apiVersion|driverName|VK_KHR_surface|VK_KHR_wayland_surface|VK_KHR_swapchain' | sort -u | head -20
    else echo "vulkaninfo ausente (apt install vulkan-tools)"; fi
    pref=§(angle_pref); echo "== ANGLE: preferência=§pref, libs §(angle_libs_ok && echo presentes || echo ausentes)"
    for a in off on; do
      [ "§a" = on ] && ! angle_libs_ok && continue
      echo "== teste incandescent (ANGLE §a)"
      probe "§a"; rc=§?; echo "resultado: §rc §{PROBE_WHY:+(§PROBE_WHY)} wayland=§PROBE_WL gpu=§{PROBE_GPU:-?} janela=§WINDOW_STATE"
    done
    echo "== fim do log do EGL (§LOG)"; tail -n 40 "§LOG" 2>/dev/null
    ;;
  angle)
    case "§{2:-status}" in
      on|1)   mkdir -p "§INC_STATE"; echo 1 > "§ANGLE_FILE"; rm -f "§CACHE"; echo "ANGLE: ligado (tenta o ANGLE; sem ele ou se falhar, usa o GLES nativo)" ;;
      off|0)  mkdir -p "§INC_STATE"; echo 0 > "§ANGLE_FILE"; rm -f "§CACHE"; echo "ANGLE: desligado (GLES nativo)" ;;
      auto)   mkdir -p "§INC_STATE"; echo auto > "§ANGLE_FILE"; rm -f "§CACHE"; echo "ANGLE: automático (GLES nativo; ANGLE só se o nativo falhar)" ;;
      *) echo "ANGLE: preferência=§(angle_pref), libs §(angle_libs_ok && echo presentes || echo ausentes)" ;;
    esac ;;
  info) shift; resolve --no-probe; cmd_info ;;
  default) shift; cmd_default "§{1:-status}" ;;
  *) echo "uso: plasma-gl detect [--force] | env [--no-probe] | status | diag | angle [auto|on|off] | default [on|off|status] | info"; exit 1 ;;
esac
SH
chmod 755 /usr/local/bin/plasma-gl
rm -f /var/lib/plasma/gl-backend   # refaz o teste no próximo uso
/usr/local/bin/plasma-gl default on >/dev/null 2>&1 || true
""".replace('§', '$'), true),
        Entry("usr/local/lib/plasma/patches/wayland-env.sh", """#!/bin/bash
# desc: ambiente padrão de apps Qt/KDE (Wayland do PlasmaDroid, D-Bus, OpenGL pelo Incandescent) em env.sh
set -u
mkdir -p /usr/local/lib/plasma
cat > /usr/local/lib/plasma/env.sh <<'SH'
# carregado por `plasma run` e pelos launchers de apps (. env.sh)
export XDG_RUNTIME_DIR="§{XDG_RUNTIME_DIR:-/run/plasma-wayland}"
export WAYLAND_DISPLAY="§{WAYLAND_DISPLAY:-wayland-0}"
export QT_QPA_PLATFORM="§{QT_QPA_PLATFORM:-wayland}"
export QSG_RHI_BACKEND="§{QSG_RHI_BACKEND:-vulkan}"   # apps que forçam OpenGL (Kdenlive) ignoram; o launcher deles ajusta
if command -v plasma-dbus >/dev/null 2>&1; then
  DBUS_SESSION_BUS_ADDRESS="§(plasma-dbus)"; export DBUS_SESSION_BUS_ADDRESS
fi
# OpenGL pelo Incandescent (ANGLE/Vulkan da GPU), com fallback automático para llvmpipe. PLASMA_GL=llvmpipe desliga.
if [ "§{PLASMA_GL:-auto}" != llvmpipe ] && command -v plasma-gl >/dev/null 2>&1; then
  eval "§(plasma-gl env)"
fi
SH
""".replace('§', '$'), true),
        Entry("usr/local/lib/plasma/apps/kdenlive.sh", """#!/bin/bash
# desc: Kdenlive (editor de vídeo) no Wayland do PlasmaDroid, com dbus, OpenGL pelo Incandescent e frei0r
. /usr/local/lib/plasma/lib.sh
need_root

PKGS="kdenlive qt6-wayland mesa-vulkan-drivers libgl1-mesa-dri mesa-utils vulkan-tools dbus-daemon dbus-bin frei0r-plugins ffmpeg breeze-icon-theme"

apply_base_patches || exit 1
apt_install §PKGS || exit 1

cat > /usr/local/bin/kdenlive <<'SH'
#!/bin/bash
# launcher do PlasmaDroid: Wayland + D-Bus + OpenGL pelo Incandescent (ANGLE/Vulkan).
# PLASMA_VERBOSE=1 mostra o log inteiro; PLASMA_GL=llvmpipe força a CPU; PLASMA_RHI=vulkan|software troca o backend do Qt Quick.
. /usr/local/lib/plasma/env.sh
# o Kdenlive exige OpenGL (monitor MLT): é aí que o Incandescent entra, traduzindo para GLES e levando ao Vulkan da GPU pelo ANGLE
export QSG_RHI_BACKEND="§{PLASMA_RHI:-opengl}"
if [ "§{PLASMA_VERBOSE:-0}" = 1 ]; then exec /usr/bin/kdenlive "§@"; fi
LOG=/tmp/kdenlive.log
/usr/bin/kdenlive "§@" >"§LOG" 2>&1
rc=§?
if [ "§rc" -ne 0 ]; then
  echo "[kdenlive] saiu com código §rc (log completo: §LOG)"
  # o dump de crash (incandescent-crash / FAULT) fica no começo: mostra ele inteiro e depois o final do log
  grep -a -A60 'incandescent-crash\|FAULT' "§LOG" | head -90
  echo "----- fim do log -----"
  grep -av 'QThreadStorage\|plugin not available' "§LOG" | tail -30
fi
exit §rc
SH
chmod 755 /usr/local/bin/kdenlive

plasma-gl detect --force
say "Kdenlive instalado. Rode: kdenlive   (plasma-gl status mostra o renderer; PLASMA_VERBOSE=1 kdenlive mostra o log)"
""".replace('§', '$'), true),
        Entry("usr/local/bin/plasma-gl", """#!/bin/bash
# plasma-gl — OpenGL dos apps Qt/KDE do guest.
#   incandescent  PADRÃO e único driver GL: libincandescent.so (Bionic, carregada pelo libhybris) traduz GL -> GLES do aparelho;
#                 com o ANGLE ele vai GLES -> Vulkan. O ANGLE é opcional: funciona com ele, sem ele, e se ele faltar ou falhar.
#   llvmpipe      OpenGL por CPU. Só entra quando você pede (PLASMA_GL=llvmpipe) ou, no modo auto, quando o teste prova que o
#                 Incandescent não consegue funcionar neste aparelho (para os apps não ficarem sem GL).
# Ordem do ANGLE (plasma-gl angle auto|on|off; PLASMA_ANGLE=auto|1|0; as Configurações do PlasmaDroid gravam o mesmo arquivo):
#   auto (padrão)  tenta o GLES nativo; se falhar e o ANGLE estiver instalado, tenta o ANGLE
#   on             tenta o ANGLE; se faltar ou falhar, usa o GLES nativo
#   off            nunca usa o ANGLE
# uso: plasma-gl detect [--force] | env [--no-probe] | status | diag | angle [auto|on|off] | default [on|off|status] | info
# Variáveis: PLASMA_GL=incandescent (PADRÃO, sempre)|auto (testa e pode cair no llvmpipe)|llvmpipe  PLASMA_GL_EXCLUSIVE=0 (não bloqueia o Mesa)  PLASMA_GL_WINDOW_TEST=0 (pula o teste de janela)
# Caminhos (sobrescrevíveis): PLASMA_INC_LIB, PLASMA_ANGLE_DIR, PLASMA_HYBRIS_DIR, PLASMA_GL_STATE (padrão /var/lib/plasma)
STATE="§{PLASMA_GL_STATE:-/var/lib/plasma}"
CACHE="§STATE/gl-backend"
LOG="§STATE/gl-probe.log"
INC_STATE="§STATE/incandescent"
ANGLE_FILE="§INC_STATE/angle"
STATUS_FILE="§INC_STATE/status"
GPU_REPORT="§INC_STATE/gpu"
PROFILE_D="§{PLASMA_GL_PROFILE_D:-/etc/profile.d/10-plasma-gl.sh}"
export XDG_RUNTIME_DIR="§{XDG_RUNTIME_DIR:-/run/plasma-wayland}"
export WAYLAND_DISPLAY="§{WAYLAND_DISPLAY:-wayland-0}"

INC_LIB="§{PLASMA_INC_LIB:-/usr/lib/plasmavk/bionic/incandescent/libincandescent.so}"
ANGLE_DIR="§{PLASMA_ANGLE_DIR:-/usr/lib/plasmavk/bionic/angle}"
HYBRIS_DIR="§{PLASMA_HYBRIS_DIR:-/usr/lib/plasmavk}"
FAIL_TTL="§{PLASMA_GL_FAIL_TTL:-3600}"   # um teste que reprovou vale por 1 h (um que passou vale até o ambiente mudar)

# ---------- ANGLE ----------
angle_pref() {
  local v="§{PLASMA_ANGLE:-}"
  [ -n "§v" ] || v=§(head -n1 "§ANGLE_FILE" 2>/dev/null)
  case "§v" in 1|on|true|yes|sim) echo on ;; 0|off|false|no|nao|não) echo off ;; *) echo auto ;; esac
}
angle_libs_ok() { [ -r "§ANGLE_DIR/libEGL_angle.so" ] && [ -r "§ANGLE_DIR/libGLESv2_angle.so" ]; }
angle_label() { # §1 = on|off efetivo
  case "§1" in on) echo "ligado (GLES -> Vulkan)" ;; *) echo "desligado (GLES nativo)" ;; esac
}
fsig() { stat -c '%Y:%s' "§1" 2>/dev/null || echo -; }
# o resultado vale só para esta combinação de biblioteca/ANGLE/ICD: se mudar, testa de novo
fingerprint() { echo "§{VK_DRIVER_FILES:-}~§{PLASMAVK_BIONIC_LIB:-}~§(fsig "§INC_LIB")~pref=§(angle_pref)~§(fsig "§ANGLE_DIR/libEGL_angle.so")~§(fsig "§ANGLE_DIR/libGLESv2_angle.so")"; }

# ---------- ambiente de cada backend ----------
# Cada linha é NOME=valor (um por linha); o `env` valida o nome antes de emitir `export`.
# §1 = backend, §2 = on|off (ANGLE efetivo)
backend_env() {
  local ld p
  case "§1" in
    incandescent)
      echo "LIBEGL=§INC_LIB"
      echo "LIBGLESV2=§INC_LIB"
      # INC_ANGLE_DIR vazio = "não use ANGLE" (o Incandescent usa o GLES nativo); INC_ANGLE=0|1 sobrepõe o config.json
      if [ "§{2:-off}" = on ]; then echo "INC_ANGLE_DIR=§ANGLE_DIR"; echo "INC_ANGLE=1"; else echo "INC_ANGLE_DIR="; echo "INC_ANGLE=0"; fi
      echo "INC_DIR_PATH=§{INC_DIR_PATH:-§INC_STATE}"
      echo "INC_PLUGIN_STATUS=§{INC_PLUGIN_STATUS:-§STATUS_FILE}"
      echo "INC_GPU_REPORT=§{INC_GPU_REPORT:-§GPU_REPORT}"
      echo "INC_RENDERER_TEMPLATE=§{INC_RENDERER_TEMPLATE:-{GPU\} (Incandescent)}"
      ld="§HYBRIS_DIR"
      for p in §{LD_LIBRARY_PATH//:/ }; do [ "§p" = "§HYBRIS_DIR" ] || ld="§ld:§p"; done
      echo "LD_LIBRARY_PATH=§ld"
      if [ "§{PLASMA_GL_EXCLUSIVE:-1}" != 0 ]; then
        # Incandescent como ÚNICO driver GL: o Mesa (DRI/Zink/llvmpipe) e o vendor do glvnd não podem fornecer GL por baixo.
        echo "LIBGL_DRIVERS_PATH=/var/empty/plasma-gl-no-mesa"
        echo "__EGL_VENDOR_LIBRARY_FILENAMES=/var/empty/plasma-gl-no-vendor.json"
      fi
      ;;
    llvmpipe)
      echo "LIBGL_ALWAYS_SOFTWARE=1"
      echo "GALLIUM_DRIVER=llvmpipe"
      # o libEGL do libhybris não pode sombrear o do Debian/Mesa
      ld=""
      for p in §{LD_LIBRARY_PATH//:/ }; do [ "§p" = "§HYBRIS_DIR" ] || ld="§{ld:+§ld:}§p"; done
      [ -n "§ld" ] && echo "LD_LIBRARY_PATH=§ld"
      ;;
  esac
}

# Variáveis que um backend NÃO pode herdar do outro
unset_for() {
  case "§1" in
    incandescent) echo "LIBGL_ALWAYS_SOFTWARE GALLIUM_DRIVER MESA_LOADER_DRIVER_OVERRIDE LIBGL_KOPPER_DISABLE MESA_GL_VERSION_OVERRIDE MESA_GLSL_VERSION_OVERRIDE ZINK_DESCRIPTORS" ;;
    llvmpipe) echo "LIBEGL LIBGLESV2 INC_ANGLE INC_ANGLE_DIR INC_DIR_PATH INC_PLUGIN_STATUS INC_GPU_REPORT INC_RENDERER_TEMPLATE LIBGL_DRIVERS_PATH __EGL_VENDOR_LIBRARY_FILENAMES MESA_LOADER_DRIVER_OVERRIDE LIBGL_KOPPER_DISABLE" ;;
  esac
}

# ---------- testes ----------
# eglinfo com o backend §1 (ANGLE §2). Saída bruta completa em §LOG; guarda o texto em §EGLINFO_OUT.
RUN_RC=0
EGLINFO_OUT=""
run_eglinfo() {
  local tmp sb=""
  tmp=§(mktemp /tmp/plasma-gl.XXXXXX) || return 1
  # lista NOME=valor em array (um item por linha): valores com espaço, como "{GPU} (Incandescent)", não podem sofrer word splitting
  local -a benv=(); mapfile -t benv < <(backend_env "§1" "§2")
  command -v stdbuf >/dev/null 2>&1 && sb="stdbuf -oL -eL"
  mkdir -p "§INC_STATE" 2>/dev/null; rm -f "§GPU_REPORT" 2>/dev/null
  { env "§{benv[@]}" EGL_PLATFORM=wayland EGL_LOG_LEVEL=debug LIBGL_DEBUG=verbose §sb timeout "§{EGL_TMO:-25}" eglinfo -B >"§tmp" 2>&1; RUN_RC=§?; } 2>/dev/null
  mkdir -p "§STATE" 2>/dev/null
  { echo "### §(date '+%F %T') backend=§1 angle=§{2:-off} rc=§RUN_RC"; env | grep -E '^(VK_|MESA_|PLASMAVK_|HYBRIS_|WAYLAND_|XDG_RUNTIME|LIBEGL|LIBGLESV2|INC_|LD_LIBRARY)' | sort; echo "---"; cat "§tmp"; } >> "§LOG" 2>/dev/null
  # o log não cresce sem limite
  [ "§(stat -c %s "§LOG" 2>/dev/null || echo 0)" -gt 400000 ] && tail -c 200000 "§LOG" > "§LOG.tmp" && mv -f "§LOG.tmp" "§LOG"
  EGLINFO_OUT=§(cat "§tmp"); rm -f "§tmp"
}

# bloco "Wayland platform:" do texto em §1 (vazio se não houver)
wayland_block() { printf '%s\n' "§1" | awk '/^[A-Za-z0-9 ]+ platform:/ {p=(§1=="Wayland")} p'; }

# Teste de janela: abre um cliente EGL Wayland de verdade e vê se ele apresenta. Só roda se houver es2gears_wayland/eglgears_wayland.
WINDOW_STATE=skip
window_probe() { # §1 backend, §2 angle
  WINDOW_STATE=skip
  [ "§{PLASMA_GL_WINDOW_TEST:-1}" = 0 ] && return 0
  local t="" c tmp rc
  for c in es2gears_wayland eglgears_wayland; do command -v "§c" >/dev/null 2>&1 && { t="§c"; break; }; done
  [ -n "§t" ] || return 0
  [ -S "§XDG_RUNTIME_DIR/§WAYLAND_DISPLAY" ] || return 0
  tmp=§(mktemp /tmp/plasma-gl.XXXXXX) || return 0
  mkdir -p "§INC_STATE" 2>/dev/null
  local -a benv=(); mapfile -t benv < <(backend_env "§1" "§2")
  { env "§{benv[@]}" EGL_PLATFORM=wayland timeout 6 "§t" >"§tmp" 2>&1; rc=§?; } 2>/dev/null
  { echo "### janela (§t) rc=§rc"; cat "§tmp"; } >> "§LOG" 2>/dev/null
  # rc 124 = ainda desenhando quando o timeout cortou; qualquer outra saída é janela que não abriu ou caiu
  if [ "§rc" -eq 124 ] && ! grep -Eqi 'failed|error|cannot|can.t |unable|bad_native|not supported|segmentation|abort' "§tmp"; then WINDOW_STATE=ok
  else WINDOW_STATE="fail:§t rc=§rc §(grep -Ei 'failed|error|cannot|unable|bad_native' "§tmp" | head -n1 | cut -c1-120)"; fi
  rm -f "§tmp"
  return 0
}

# 0 = Incandescent funcionando; 1 = não serve (motivo em PROBE_WHY); 2 = não dá para testar (eglinfo ausente)
PROBE_WHY=""; PROBE_GPU=""; PROBE_WL=""
probe() { # §1 = on|off
  local ang="§{1:-off}" blk lines r
  PROBE_WHY=""; PROBE_GPU=""; PROBE_WL=""
  command -v eglinfo >/dev/null 2>&1 || return 2
  [ -r "§INC_LIB" ] || { PROBE_WHY="libincandescent.so não encontrada em §INC_LIB (PLASMA_INC_LIB)"; return 1; }
  ls "§HYBRIS_DIR"/libEGL.so* >/dev/null 2>&1 || { PROBE_WHY="libEGL do libhybris ausente em §HYBRIS_DIR (PLASMA_HYBRIS_DIR)"; return 1; }
  if [ "§ang" = on ] && ! angle_libs_ok; then PROBE_WHY="ANGLE ausente em §ANGLE_DIR (libEGL_angle.so, libGLESv2_angle.so)"; return 1; fi
  case "§{VK_DRIVER_FILES:-}§{VK_ICD_FILENAMES:-}" in
    *lvp*|*lavapipe*) [ "§ang" = on ] && { PROBE_WHY="o Vulkan do guest é o lavapipe (CPU): o ANGLE não teria GPU"; return 1; } ;;
  esac
  run_eglinfo incandescent "§ang"
  blk=§(wayland_block "§EGLINFO_OUT")
  if [ -n "§blk" ]; then lines=§(printf '%s\n' "§blk" | grep -i 'renderer'); PROBE_WL=sim
  else lines=§(printf '%s\n' "§EGLINFO_OUT" | grep -i 'renderer'); PROBE_WL=nao-listada; fi
  PROBE_GPU=§(sed -n 's/^GPU=//p' "§GPU_REPORT" 2>/dev/null | head -n1)
  if [ "§RUN_RC" -ge 128 ] && [ -z "§lines" ]; then PROBE_WHY="eglinfo morreu com o sinal §((RUN_RC-128)) antes de listar o renderer (veja §LOG)"; return 1; fi
  if [ "§RUN_RC" -eq 124 ] && [ -z "§lines" ]; then PROBE_WHY="eglinfo travou (timeout) ao criar o contexto (veja §LOG)"; return 1; fi
  if [ -z "§lines" ]; then
    if printf '%s\n' "§EGLINFO_OUT" | grep -qi 'failed to get.*display\|eglGetPlatformDisplay\|no display'; then
      PROBE_WHY="o EGL não criou o display Wayland (a libincandescent.so é antiga? recompile com o egl.cpp novo; veja §LOG)"
    else PROBE_WHY="o EGL Wayland não criou o contexto do Incandescent (veja §LOG)"; fi
    return 1
  fi
  printf '%s\n' "§lines" | grep -qi 'llvmpipe\|lavapipe\|swiftshader\|softpipe\|software' && { PROBE_WHY="o driver GLES caiu em dispositivo de CPU"; return 1; }
  # prova de que foi o Incandescent: o nome do renderer ou o relatório de GPU que só ele grava
  if ! printf '%s\n' "§lines" | grep -qi incandescent && [ -z "§PROBE_GPU" ]; then
    PROBE_WHY="o renderer não é o do Incandescent: §(printf '%s\n' "§lines" | head -n1 | cut -c1-100)"; return 1
  fi
  window_probe incandescent "§ang"
  case "§WINDOW_STATE" in fail:*) PROBE_WHY="contexto ok, mas a janela Wayland não apresentou (§{WINDOW_STATE#fail:}); falta a apresentação EGL->Wayland"; return 1 ;; esac
  return 0
}

write_status() { mkdir -p "§INC_STATE" 2>/dev/null; echo "§*" > "§STATUS_FILE" 2>/dev/null; }

# lê o cache: CB (backend) CFP (impressão) CANG (on|off) CTS (epoch). 0 se existe e é legível.
read_cache() {
  CB=""; CFP=""; CANG=off; CTS=0
  [ -f "§CACHE" ] || return 1
  IFS='|' read -r CB CFP CANG CTS < "§CACHE" 2>/dev/null
  case "§CB" in incandescent|llvmpipe) ;; *) return 1 ;; esac
  case "§CANG" in on|off) ;; *) CANG=off ;; esac
  case "§CTS" in ''|*[!0-9]*) CTS=0 ;; esac
  return 0
}

# Define BACKEND e ANGLE_EFF.
BACKEND=incandescent; ANGLE_EFF=off; VERIFIED=sim
detect() {
  local fp now pref a rc cands=() tried="" last_why=""
  fp=§(fingerprint); now=§(date +%s)
  [ "§{1:-}" = --force ] && rm -f "§CACHE"
  if read_cache && [ "§CFP" = "§fp" ]; then
    if [ "§CB" = incandescent ] || [ §((now - CTS)) -lt "§FAIL_TTL" ]; then BACKEND=§CB; ANGLE_EFF=§CANG; VERIFIED=sim; return 0; fi
  fi
  pref=§(angle_pref)
  case "§pref" in
    off)  cands=(off) ;;
    on)   if angle_libs_ok; then cands=(on off); else cands=(off); echo "[plasma-gl] ANGLE pedido, mas ausente em §ANGLE_DIR: seguindo sem ele" >&2; fi ;;
    *)    if angle_libs_ok; then cands=(off on); else cands=(off); fi ;;
  esac
  mkdir -p "§STATE" 2>/dev/null
  for a in "§{cands[@]}"; do
    probe "§a"; rc=§?
    if [ "§rc" -eq 2 ]; then
      echo "[plasma-gl] eglinfo ausente (apt install mesa-utils): não deu para testar; usando o Incandescent sem teste" >&2
      BACKEND=incandescent; VERIFIED=nao
      ANGLE_EFF=off; [ "§pref" = on ] && angle_libs_ok && ANGLE_EFF=on
      return 0
    fi
    if [ "§rc" -eq 0 ]; then
      BACKEND=incandescent; ANGLE_EFF=§a; VERIFIED=sim
      echo "incandescent|§fp|§a|§now" > "§CACHE"
      write_status "OK: §{PROBE_GPU:-GPU do aparelho} via Incandescent, ANGLE §(angle_label "§a"), Wayland: §PROBE_WL, janela: §{WINDOW_STATE%%:*}"
      echo "[plasma-gl] backend OpenGL: incandescent (§{PROBE_GPU:-GPU}, ANGLE §(angle_label "§a"))" >&2
      return 0
    fi
    last_why="§PROBE_WHY"; tried="§tried §{a}"
    echo "[plasma-gl] incandescent (ANGLE §(angle_label "§a")) não serviu: §PROBE_WHY" >&2
  done
  BACKEND=llvmpipe; ANGLE_EFF=off; VERIFIED=sim
  echo "llvmpipe|§fp|off|§now" > "§CACHE"
  write_status "FALHA: §last_why (testado ANGLE:§{tried}); usando llvmpipe"
  echo "[plasma-gl] o Incandescent não funciona agora (§last_why); usando llvmpipe (CPU). Force com PLASMA_GL=incandescent ou rode: plasma-gl diag" >&2
}

forced_angle() { # ANGLE efetivo quando o backend é forçado (sem teste)
  local p; p=§(angle_pref)
  if [ "§p" = on ] && angle_libs_ok; then echo on; return; fi
  if [ "§p" = auto ] && read_cache && [ "§CB" = incandescent ] && [ "§CANG" = on ] && angle_libs_ok; then echo on; return; fi
  echo off
}

resolve() { # §1 = --no-probe
  local c="§{PLASMA_GL:-incandescent}"
  case "§c" in
    zink|zink-nokopper) echo "[plasma-gl] o Zink foi substituído pelo Incandescent; usando incandescent" >&2; c=incandescent ;;
  esac
  VERIFIED=sim
  case "§c" in
    incandescent) BACKEND=incandescent; ANGLE_EFF=§(forced_angle) ;;
    llvmpipe)     BACKEND=llvmpipe; ANGLE_EFF=off ;;
    *)
      if [ "§{1:-}" = --no-probe ]; then
        if read_cache && [ "§CFP" = "§(fingerprint)" ]; then BACKEND=§CB; ANGLE_EFF=§CANG
        else BACKEND=incandescent; ANGLE_EFF=§(forced_angle); VERIFIED=nao; fi
      else detect >&2; fi ;;
  esac
}

emit_env() {
  local v l name
  mkdir -p "§INC_STATE" 2>/dev/null
  echo "unset §(unset_for "§BACKEND")"
  backend_env "§BACKEND" "§ANGLE_EFF" | while IFS= read -r l; do
    name="§{l%%=*}"
    case "§name" in ''|[0-9]*|*[!A-Za-z0-9_]*) continue ;; esac   # só NOME=valor válido: nada de lixo no eval
    case "§l" in *=*) ;; *) continue ;; esac
    printf 'export %s=%q\n' "§name" "§{l#*=}"
  done
  # sinaliza para os apps/Configurações qual backend está valendo
  echo "export PLASMA_GL_BACKEND=§BACKEND"
}

# Linha "OpenGL" do fastfetch (estilo PC): versão do GL + biblioteca [EGL] + backend em uso. A versão fica em cache.
cmd_info() {
  local how f key ver blk
  case "§BACKEND" in
    incandescent) if [ "§ANGLE_EFF" = on ]; then how="Incandescent → ANGLE → Vulkan"; else how="Incandescent → GLES nativo"; fi ;;
    *) how="llvmpipe (CPU)" ;;
  esac
  f="§INC_STATE/glinfo"; key="§BACKEND|§ANGLE_EFF|§(fsig "§INC_LIB")"; ver=""
  [ -r "§f" ] && [ "§(sed -n 1p "§f" 2>/dev/null)" = "§key" ] && ver=§(sed -n 2p "§f" 2>/dev/null)
  if [ -z "§ver" ] && command -v eglinfo >/dev/null 2>&1; then
    EGL_TMO="§{PLASMA_GL_INFO_TIMEOUT:-10}"
    run_eglinfo "§BACKEND" "§ANGLE_EFF"
    blk=§(wayland_block "§EGLINFO_OUT"); [ -n "§blk" ] || blk="§EGLINFO_OUT"
    ver=§(printf '%s\n' "§blk" | sed -n -e 's/^ *OpenGL compatibility profile version: *//p' -e 's/^ *OpenGL core profile version: *//p' -e 's/^ *OpenGL ES profile version: *//p' | head -n1)
    if [ -n "§ver" ]; then mkdir -p "§INC_STATE" 2>/dev/null; printf '%s\n%s\n' "§key" "§ver" > "§f" 2>/dev/null; fi
  fi
  echo "§{ver:-versão indisponível (instale mesa-utils-bin)} [EGL · §how]"
}

cmd_default() {
  case "§{1:-status}" in
    on)
      mkdir -p "§(dirname "§PROFILE_D")" 2>/dev/null
      cat > "§PROFILE_D" <<'PD'
# plasma-gl: o Incandescent é o OpenGL padrão do guest (não testa nada aqui: usa o último resultado guardado)
if [ "§{PLASMA_GL:-auto}" != llvmpipe ] && [ -z "§{PLASMA_GL_BACKEND:-}" ] && command -v plasma-gl >/dev/null 2>&1; then
  eval "§(plasma-gl env --no-probe 2>/dev/null)"
fi
PD
      chmod 644 "§PROFILE_D"; echo "Incandescent é o OpenGL padrão (§PROFILE_D)" ;;
    off) rm -f "§PROFILE_D"; echo "padrão removido (§PROFILE_D)" ;;
    *) if [ -f "§PROFILE_D" ]; then echo "padrão: ligado (§PROFILE_D)"; else echo "padrão: desligado (rode: plasma-gl default on)"; fi ;;
  esac
}

case "§{1:-env}" in
  detect) shift; detect "§@"; exit §? ;;
  env)
    shift; resolve "§{1:-}"
    emit_env ;;
  status)
    resolve
    echo "backend: §BACKEND§([ "§VERIFIED" = nao ] && echo ' (sem teste)')"
    echo "angle: preferência=§(angle_pref), efetivo=§(angle_label "§ANGLE_EFF"), libs=§(angle_libs_ok && echo presentes || echo ausentes)"
    echo "exclusivo: §([ "§{PLASMA_GL_EXCLUSIVE:-1}" != 0 ] && echo sim || echo não)   padrão do shell: §([ -f "§PROFILE_D" ] && echo ligado || echo desligado)"
    [ -f "§STATUS_FILE" ] && echo "último teste: §(head -n1 "§STATUS_FILE")"
    [ "§BACKEND" = incandescent ] && { run_eglinfo incandescent "§ANGLE_EFF"; blk=§(wayland_block "§EGLINFO_OUT"); [ -n "§blk" ] || blk="§EGLINFO_OUT"; printf '%s\n' "§blk" | grep -i 'renderer'; }
    ;;
  diag)
    echo "== ambiente do guest"
    env | grep -E '^(VK_|MESA_|PLASMAVK_|HYBRIS_|WAYLAND_|XDG_RUNTIME|PLASMA_|LIBGL_|LIBEGL|LIBGLESV2|INC_|LD_LIBRARY|__EGL)' | sort
    echo "== arquivos: §INC_LIB | §ANGLE_DIR | §HYBRIS_DIR/libEGL*"; ls -l "§INC_LIB" "§ANGLE_DIR" 2>&1 | head; ls -l "§HYBRIS_DIR"/libEGL.so* "§HYBRIS_DIR"/libGLESv2.so* 2>&1 | head
    echo "== plataforma EGL do libhybris"; ls "§HYBRIS_DIR"/libhybris/eglplatform_*.so 2>/dev/null || echo "nenhum eglplatform_*.so em §HYBRIS_DIR/libhybris (HYBRIS_EGLPLATFORM=§{HYBRIS_EGLPLATFORM:-<não definido>})"
    echo "== sockets Wayland"; ls -l "§XDG_RUNTIME_DIR" 2>&1 | head
    echo "== vulkaninfo"
    if command -v vulkaninfo >/dev/null 2>&1; then
      o=§(timeout 25 vulkaninfo 2>&1); echo "§o" | grep -E 'deviceName|apiVersion|driverName|VK_KHR_surface|VK_KHR_wayland_surface|VK_KHR_swapchain' | sort -u | head -20
    else echo "vulkaninfo ausente (apt install vulkan-tools)"; fi
    pref=§(angle_pref); echo "== ANGLE: preferência=§pref, libs §(angle_libs_ok && echo presentes || echo ausentes)"
    for a in off on; do
      [ "§a" = on ] && ! angle_libs_ok && continue
      echo "== teste incandescent (ANGLE §a)"
      probe "§a"; rc=§?; echo "resultado: §rc §{PROBE_WHY:+(§PROBE_WHY)} wayland=§PROBE_WL gpu=§{PROBE_GPU:-?} janela=§WINDOW_STATE"
    done
    echo "== fim do log do EGL (§LOG)"; tail -n 40 "§LOG" 2>/dev/null
    ;;
  angle)
    case "§{2:-status}" in
      on|1)   mkdir -p "§INC_STATE"; echo 1 > "§ANGLE_FILE"; rm -f "§CACHE"; echo "ANGLE: ligado (tenta o ANGLE; sem ele ou se falhar, usa o GLES nativo)" ;;
      off|0)  mkdir -p "§INC_STATE"; echo 0 > "§ANGLE_FILE"; rm -f "§CACHE"; echo "ANGLE: desligado (GLES nativo)" ;;
      auto)   mkdir -p "§INC_STATE"; echo auto > "§ANGLE_FILE"; rm -f "§CACHE"; echo "ANGLE: automático (GLES nativo; ANGLE só se o nativo falhar)" ;;
      *) echo "ANGLE: preferência=§(angle_pref), libs §(angle_libs_ok && echo presentes || echo ausentes)" ;;
    esac ;;
  info) shift; resolve --no-probe; cmd_info ;;
  default) shift; cmd_default "§{1:-status}" ;;
  *) echo "uso: plasma-gl detect [--force] | env [--no-probe] | status | diag | angle [auto|on|off] | default [on|off|status] | info"; exit 1 ;;
esac
""".replace('§', '$'), true),
        Entry("usr/local/lib/plasma/env.sh", """# carregado por `plasma run` e pelos launchers de apps (. env.sh)
export XDG_RUNTIME_DIR="§{XDG_RUNTIME_DIR:-/run/plasma-wayland}"
export WAYLAND_DISPLAY="§{WAYLAND_DISPLAY:-wayland-0}"
export QT_QPA_PLATFORM="§{QT_QPA_PLATFORM:-wayland}"
export QSG_RHI_BACKEND="§{QSG_RHI_BACKEND:-vulkan}"   # apps que forçam OpenGL (Kdenlive) ignoram; o launcher deles ajusta
if command -v plasma-dbus >/dev/null 2>&1; then
  DBUS_SESSION_BUS_ADDRESS="§(plasma-dbus)"; export DBUS_SESSION_BUS_ADDRESS
fi
# OpenGL pelo Incandescent (ANGLE/Vulkan da GPU), com fallback automático para llvmpipe. PLASMA_GL=llvmpipe desliga.
if [ "§{PLASMA_GL:-auto}" != llvmpipe ] && command -v plasma-gl >/dev/null 2>&1; then
  eval "§(plasma-gl env)"
fi
""".replace('§', '$'), false)
    )

    fun install(rootfs: File) {
        for (e in ENTRIES) {
            val f = File(rootfs, e.path)
            f.parentFile?.mkdirs()
            if (!f.isFile || f.readText() != e.text) f.writeText(e.text)
            f.setReadable(true, false)
            if (e.exec) f.setExecutable(true, false)
        }
        File(rootfs, "var/lib/plasma").mkdirs()
    }
}
