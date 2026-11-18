package org.plasma.proot

import java.io.File

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
  plasma repair                     conserta o dpkg (fix-broken, scripts que abortam no PRoot)
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
  repair)
    need_root
    apply_patch dpkg-systemd; repair_dpkg
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
        Entry("usr/local/bin/plasma-appimage", """#!/bin/bash
# plasma-appimage — suporte a AppImage no PlasmaDroid (usado pelo popup do Dolphin; também serve no terminal)
#   plasma-appimage is   <arquivo>          sai com 0 se for uma AppImage
#   plasma-appimage info <arquivo>          imprime chave=valor (name, comment, arch, host_arch, icon) e extrai o ícone para o cache
#   plasma-appimage run  <arquivo> [args]   prepara (cópia executável, se preciso) e executa
CACHE=/var/cache/plasma/appimage
say() { echo "[appimage] §*" >&2; }

elf_arch() {
  case "§(od -An -tx1 -j18 -N1 "§1" 2>/dev/null | tr -d ' \n')" in
    b7) echo aarch64 ;; 3e) echo x86_64 ;; 28) echo armv7l ;; 03) echo i686 ;; *) echo desconhecida ;;
  esac
}

is_appimage() {
  [ -f "§1" ] || return 1
  head -c4 "§1" 2>/dev/null | grep -q ELF || return 1
  [ "§(dd if="§1" bs=1 skip=8 count=2 2>/dev/null | tr -d '\0')" = AI ] && return 0
  case "§{1,,}" in *.appimage) return 0 ;; esac
  return 1
}

# o armazenamento do Android (/sdcard) é noexec: copia para o home do guest e executa a cópia
runnable() {
  local f="§1" dest
  case "§f" in
    /sdcard/*|/storage/*|/mnt/*)
      dest="§HOME/.local/share/appimages/§(basename "§f")"
      mkdir -p "§(dirname "§dest")"
      if [ ! -f "§dest" ] || [ "§(stat -c %s "§f")" != "§(stat -c %s "§dest")" ] || [ "§f" -nt "§dest" ]; then
        say "copiando para §dest (o armazenamento do Android não permite executar)"
        cp -f "§f" "§dest" || return 1
      fi
      chmod +x "§dest"; echo "§dest" ;;
    *) chmod +x "§f" 2>/dev/null; echo "§f" ;;
  esac
}

# offset do squashfs = fim do ELF (e_shoff + e_shentsize * e_shnum), o mesmo cálculo do runtime da AppImage
sqfs_offset() {
  readelf -h "§1" 2>/dev/null | awk -F: '
    /Start of section headers/ {o=§2+0} /Size of section headers/ {s=§2+0} /Number of section headers/ {n=§2+0}
    END { if (s>0 && n>0) printf "%d", o + s*n }'
}

# extrai só o necessário (sem executar a AppImage: serve também para x86_64 neste aparelho)
extract_meta() {
  local f="§1" out="§2" off list root desk icon cand tgt
  off=§(sqfs_offset "§f"); [ -n "§off" ] || return 1
  command -v unsquashfs >/dev/null 2>&1 || return 1
  list=§(unsquashfs -o "§off" -l "§f" 2>/dev/null | sed 's#^squashfs-root/\?##' | grep -v '^§') || return 1
  [ -n "§list" ] || return 1
  root="§out/root"; mkdir -p "§root"
  desk=§(echo "§list" | grep -E '^[^/]+\.desktop§' | head -1)
  [ -n "§desk" ] || desk=§(echo "§list" | grep -E '^usr/share/applications/[^/]+\.desktop§' | head -1)
  [ -n "§desk" ] && unsquashfs -o "§off" -f -d "§root" "§f" "§desk" >/dev/null 2>&1
  [ -n "§desk" ] && cp -f "§root/§desk" "§out/app.desktop" 2>/dev/null
  icon=§(sed -n 's/^Icon=//p' "§out/app.desktop" 2>/dev/null | head -1 | tr -d '\r')
  cand=""
  if [ -n "§icon" ]; then
    # prefere o maior PNG do hicolor; depois PNG/SVG na raiz
    cand=§(echo "§list" | grep -E "(^|/)§{icon//./\\.}\.png§" | awk '{ n=0; if (match(§0,/[0-9]+x[0-9]+/)) n=substr(§0,RSTART,RLENGTH)+0; print n, §0 }' | sort -rn | head -1 | cut -d' ' -f2-)
    [ -n "§cand" ] || cand=§(echo "§list" | grep -E "(^|/)§{icon//./\\.}\.svg§" | head -1)
  fi
  [ -n "§cand" ] || cand=§(echo "§list" | grep -E '^[^/]+\.png§' | head -1)
  [ -n "§cand" ] || cand=§(echo "§list" | grep -x '\.DirIcon' | head -1)
  [ -n "§cand" ] || return 0
  unsquashfs -o "§off" -f -d "§root" "§f" "§cand" >/dev/null 2>&1
  if [ -L "§root/§cand" ]; then            # .DirIcon costuma ser um link para o ícone real
    tgt=§(readlink "§root/§cand"); tgt="§{tgt#./}"
    case "§tgt" in /*) tgt="§{tgt#/}" ;; *) [ "§(dirname "§cand")" != . ] && tgt="§(dirname "§cand")/§tgt" ;; esac
    unsquashfs -o "§off" -f -d "§root" "§f" "§tgt" >/dev/null 2>&1
    [ -f "§root/§tgt" ] && cand="§tgt"
  fi
  [ -f "§root/§cand" ] && echo "§root/§cand" > "§out/icon.src"
  return 0
}

# alternativa: a própria AppImage (tipo 2) extrai arquivos com --appimage-extract (só arquitetura compatível)
extract_meta_run() {
  local f="§1" out="§2" run tmp icon
  run=§(runnable "§f") || return 1
  tmp=§(mktemp -d)
  ( cd "§tmp" && APPIMAGE_EXTRACT_AND_RUN=1 timeout 90 "§run" --appimage-extract '*.desktop' >/dev/null 2>&1
    APPIMAGE_EXTRACT_AND_RUN=1 timeout 90 "§run" --appimage-extract '.DirIcon' >/dev/null 2>&1
    APPIMAGE_EXTRACT_AND_RUN=1 timeout 90 "§run" --appimage-extract '*.png' >/dev/null 2>&1 )
  cp -f "§(ls "§tmp"/squashfs-root/*.desktop 2>/dev/null | head -1)" "§out/app.desktop" 2>/dev/null
  icon=§(sed -n 's/^Icon=//p' "§out/app.desktop" 2>/dev/null | head -1 | tr -d '\r')
  for c in "§tmp/squashfs-root/§icon.png" "§tmp/squashfs-root/.DirIcon" "§(ls "§tmp"/squashfs-root/*.png 2>/dev/null | head -1)"; do
    [ -f "§c" ] && { mkdir -p "§out/root"; cp -fL "§c" "§out/root/icon-src"; echo "§out/root/icon-src" > "§out/icon.src"; break; }
  done
  rm -rf "§tmp"
}

to_png() {  # §1 origem (png/svg/xpm), §2 destino png (256px)
  case "§1" in
    *.svg) if command -v rsvg-convert >/dev/null 2>&1; then rsvg-convert -w 256 -h 256 -o "§2" "§1" 2>/dev/null
           elif command -v convert >/dev/null 2>&1; then convert -background none -resize 256x256 "§1" "§2" 2>/dev/null; else return 1; fi ;;
    *) if head -c8 "§1" 2>/dev/null | grep -q PNG; then cp -f "§1" "§2"
       elif command -v convert >/dev/null 2>&1; then convert "§1" -resize 256x256 "§2" 2>/dev/null; else return 1; fi ;;
  esac
  [ -s "§2" ]
}

# unsquashfs/readelf extraem o ícone sem executar a AppImage; instala uma vez se faltarem (o Setup novo já instala)
need_tools() {
  command -v unsquashfs >/dev/null 2>&1 && command -v readelf >/dev/null 2>&1 && return 0
  [ -f /var/lib/plasma/appimage-tools-tried ] && return 1
  mkdir -p /var/lib/plasma; touch /var/lib/plasma/appimage-tools-tried
  say "instalando squashfs-tools e binutils (uma vez)..."
  if [ -f /usr/local/lib/plasma/lib.sh ]; then
    ( . /usr/local/lib/plasma/lib.sh; apt_install squashfs-tools binutils librsvg2-bin ) >/dev/null 2>&1
  else
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends squashfs-tools binutils librsvg2-bin >/dev/null 2>&1
  fi
  command -v unsquashfs >/dev/null 2>&1
}

cmd_info() {
  local f="§1" k out name comment src
  echo "arch=§(elf_arch "§f")"; echo "host_arch=§(uname -m)"
  need_tools || true
  k=§(printf '%s|%s|%s' "§f" "§(stat -c %Y "§f")" "§(stat -c %s "§f")" | sha1sum | cut -c1-16)
  out="§CACHE/§k"; mkdir -p "§out"
  if [ ! -f "§out/done" ]; then
    rm -rf "§out/root" "§out/icon.src" "§out/icon.png" "§out/app.desktop"
    extract_meta "§f" "§out" || true
    [ -f "§out/icon.src" ] || [ -f "§out/app.desktop" ] || extract_meta_run "§f" "§out" || true
    [ -f "§out/icon.src" ] && { src=§(cat "§out/icon.src"); to_png "§src" "§out/icon.png" || rm -f "§out/icon.png"; }
    rm -rf "§out/root"; touch "§out/done"
  fi
  name=§(sed -n 's/^Name=//p' "§out/app.desktop" 2>/dev/null | head -1 | tr -d '\r')
  comment=§(sed -n 's/^Comment=//p' "§out/app.desktop" 2>/dev/null | head -1 | tr -d '\r')
  echo "name=§name"; echo "comment=§comment"
  [ -s "§out/icon.png" ] && echo "icon=§out/icon.png"
  return 0
}

# memfd: o runtime da AppImage faz memfd_create + fexecve, que o Android barra (EACCES: "Failed to exec memfd").
# Em vez de depender do fallback do runtime, extrai o squashfs com unsquashfs (uma vez, em cache) e executa o AppRun direto.
# Desligue com PLASMA_APPIMAGE_MODE=runtime.
extract_dir() {  # §1 = AppImage executável; imprime o diretório extraído (extrai se preciso)
  local f="§1" off k dir tmp
  [ "§{PLASMA_APPIMAGE_MODE:-extract}" = runtime ] && return 1
  need_tools || return 1
  off=§(sqfs_offset "§f"); [ -n "§off" ] || return 1
  k=§(printf '%s|%s|%s' "§f" "§(stat -c %Y "§f")" "§(stat -c %s "§f")" | sha1sum | cut -c1-16)
  dir="§{HOME:-/root}/.local/share/appimages/.extracted/§k"
  if [ ! -x "§dir/AppRun" ]; then
    tmp="§dir.tmp"; rm -rf "§tmp" "§dir"; mkdir -p "§(dirname "§dir")"
    say "extraindo a AppImage (uma vez; evita o memfd bloqueado pelo Android)..."
    if ! { unsquashfs -no-xattrs -f -d "§tmp" -o "§off" "§f" >/dev/null 2>&1 || { rm -rf "§tmp"; unsquashfs -f -d "§tmp" -o "§off" "§f" >/dev/null 2>&1; }; }; then
      rm -rf "§tmp"; return 1
    fi
    chmod -R u+rwX "§tmp" 2>/dev/null
    [ -e "§tmp/AppRun" ] || { rm -rf "§tmp"; return 1; }
    chmod +x "§tmp/AppRun" 2>/dev/null
    mv "§tmp" "§dir" || return 1
  fi
  echo "§dir"
}

cmd_run() {
  local f="§1" run rc a dir; shift
  is_appimage "§f" || { say "não parece uma AppImage: §f"; exit 1; }
  a=§(elf_arch "§f")
  if [ "§a" != "§(uname -m)" ] && [ "§a" != desconhecida ]; then
    if command -v fex-run >/dev/null 2>&1; then say "AppImage §a: executada pelo FEX (fex-run)"
    else say "aviso: AppImage §a, mas este aparelho é §(uname -m); provavelmente não vai executar (instale o FEX: plasma install fex)"; fi
  fi
  run=§(runnable "§f") || exit 1
  export XDG_RUNTIME_DIR="§{XDG_RUNTIME_DIR:-/run/plasma-wayland}" WAYLAND_DISPLAY="§{WAYLAND_DISPLAY:-wayland-0}"
  export APPIMAGE_EXTRACT_AND_RUN=1 NO_AT_BRIDGE=1   # sem FUSE no PRoot: a AppImage se extrai para /tmp e roda de lá
  say "executando: §run §{PLASMA_APPIMAGE_ARGS:-} §*"
  if dir=§(extract_dir "§run") && [ -n "§dir" ]; then
    export APPIMAGE="§run" APPDIR="§dir" ARGV0="§run" OWD="§PWD"
    export PATH="§dir/usr/bin:§PATH"
    "§dir/AppRun" §{PLASMA_APPIMAGE_ARGS:-} "§@"; rc=§?
  else
    "§run" §{PLASMA_APPIMAGE_ARGS:-} "§@"; rc=§?
  fi
  if [ "§rc" -ne 0 ]; then
    say "saiu com código §rc"
    say "dicas: apps Electron como root precisam de   PLASMA_APPIMAGE_ARGS=--no-sandbox plasma-appimage run <arquivo>"
    say "       apps GTK/Qt sem Wayland: GDK_BACKEND=x11 / QT_QPA_PLATFORM=xcb com o servidor X11 nativo (plasma-x11 start)"
  fi
  exit "§rc"
}

case "§{1:-}" in
  is)   is_appimage "§{2:-}" ;;
  info) is_appimage "§{2:-}" || { echo "appimage=0"; exit 1; }; echo "appimage=1"; cmd_info "§2" ;;
  run)  [ -n "§{2:-}" ] || { echo "uso: plasma-appimage run <arquivo> [args]"; exit 2; }; f="§2"; shift 2; cmd_run "§f" "§@" ;;
  *)    sed -n '2,6p' "§0" | sed 's/^# \?//'; exit 2 ;;
esac
""".replace('§', '$'), true),
        Entry("usr/local/lib/plasma/lib.sh", """# This file belongs to the PlasmaDroid project.
#
# Copyright (C) 2026 ElxSnowkk (https://github.com/ElxSnowkk)
#
# PlasmaDroid is free software: you can redistribute it and/or modify it under the
# terms of the GNU General Public License as published by the Free Software
# Foundation, either version 3 of the License, or (at your option) any later version.
#
# Licensed under GPL-3.0-or-later. See the LICENSE file at the project root,
# or <https://www.gnu.org/licenses/gpl-3.0.html>.
#
# This program is distributed in the hope that it will be useful, but WITHOUT ANY
# WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
# PARTICULAR PURPOSE. See the GNU General Public License for more details.

L=/usr/local/lib/plasma
STATE=/var/lib/plasma
BASE_PATCHES="dns os-arch quiet dpkg-systemd dbus-session gl-vulkan wayland-env adwaita-icons"

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

heal() {
  local n=0 pkg ph base f
  while read -r pkg ph; do
    [ -n "§pkg" ] || continue
    base="§{pkg%%:*}"
    for f in "/var/lib/dpkg/info/§pkg.§ph" "/var/lib/dpkg/info/§base.§ph"; do
      [ -f "§f" ] || continue
      grep -q 'plasma-stub' "§f" && continue
      mkdir -p "§STATE/postinst-orig"
      cp "§f" "§STATE/postinst-orig/§(basename "§f")"
      printf '#!/bin/sh\n# plasma-stub\nexit 0\n' > "§f"; chmod 755 "§f"
      n=§((n+1)); say "§ph de §pkg ignorado (falha no PRoot)"
    done
  done < <(awk '/error processing package/ {p=§5}
    /script subprocess returned error exit status/ && p!="" {
      ph = (§0 ~ /pre-installation/) ? "preinst" : (§0 ~ /pre-removal/) ? "prerm" : (§0 ~ /post-removal/) ? "postrm" : "postinst"
      print p, ph; p=""
    }' "§1" | sort -u)
  [ "§n" -gt 0 ]
}

APT_OPTS=(-o DPkg::Lock::Timeout=900 -o Acquire::Retries=5 -o Acquire::By-Hash=yes -o Acquire::http::Pipeline-Depth=0
          -o Dpkg::Options::=--force-confold -o Dpkg::Options::=--force-confdef)

repair_dpkg() {
  local log=/tmp/plasma-dpkg.log try rc f
  for f in /usr/bin/* /usr/sbin/* /usr/lib/dpkg/*; do [ -f "§f" ] && [ ! -x "§f" ] && chmod 755 "§f"; done
  export DEBIAN_FRONTEND=noninteractive
  for try in 1 2 3; do
    dpkg --configure -a --force-confold --force-confdef 2>&1 | tee "§log"; rc=§{PIPESTATUS[0]}
    if [ "§rc" -ne 0 ]; then heal "§log" && continue; fi
    apt-get "§{APT_OPTS[@]}" -f install -y --fix-missing 2>&1 | tee "§log"; rc=§{PIPESTATUS[0]}
    [ "§rc" -eq 0 ] && return 0
    heal "§log" || break
  done
  say "dpkg ainda inconsistente:"; dpkg --audit 2>&1 | head -20
  return 1
}

apt_install() {
  local log=/tmp/plasma-apt.log try rc upgraded=0
  export DEBIAN_FRONTEND=noninteractive
  apt-get "§{APT_OPTS[@]}" update || say "aviso: apt-get update falhou, tentando mesmo assim"
  repair_dpkg >/dev/null 2>&1 || true
  for try in 1 2 3; do
    apt-get "§{APT_OPTS[@]}" install -y "§@" 2>&1 | tee "§log"; rc=§{PIPESTATUS[0]}
    [ "§rc" -eq 0 ] && return 0
    heal "§log" || true
    if grep -q 'Unmet dependencies\|not going to be installed\|held broken\|Unable to satisfy' "§log" && [ "§upgraded" = 0 ]; then
      upgraded=1; say "dependências não resolvidas: alinhando o rootfs com o mirror (full-upgrade)"
      apt-get "§{APT_OPTS[@]}" full-upgrade -y --fix-missing 2>&1 | tee "§log" || true
      heal "§log" || true
    fi
    repair_dpkg >/dev/null 2>&1 || true
  done
  say "a instalação falhou. Últimas linhas de §log:"; tail -15 "§log"
  return 1
}
""".replace('§', '$'), false),
        Entry("usr/local/lib/plasma/patches/adwaita-icons.sh", """#!/bin/bash
# This file belongs to the PlasmaDroid project.
#
# Copyright (C) 2026 ElxSnowkk (https://github.com/ElxSnowkk)
#
# Licensed under GPL-3.0-or-later. See the LICENSE file at the project root,
# or <https://www.gnu.org/licenses/gpl-3.0.html>.
#
# This program is distributed in the hope that it will be useful, but WITHOUT ANY
# WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
# PARTICULAR PURPOSE. See the GNU General Public License for more details.

# desc: tema de ícones Adwaita (GTK) para apps Linux, incluindo o Pi-Apps

set -u
export DEBIAN_FRONTEND=noninteractive

if ! dpkg -s adwaita-icon-theme >/dev/null 2>&1 || ! dpkg -s hicolor-icon-theme >/dev/null 2>&1; then
  apt-get install -y --no-install-recommends adwaita-icon-theme hicolor-icon-theme >/dev/null 2>&1 \
    || { echo "[plasma] falhou ao instalar adwaita-icon-theme"; exit 1; }
fi

for v in gtk-3.0 gtk-4.0; do
  mkdir -p "/etc/xdg/§v"
  printf '[Settings]\ngtk-icon-theme-name=Adwaita\n' > "/etc/xdg/§v/settings.ini"
done

if command -v gtk-update-icon-cache >/dev/null 2>&1; then
  gtk-update-icon-cache -q -f /usr/share/icons/Adwaita 2>/dev/null || true
  gtk-update-icon-cache -q -f /usr/share/icons/hicolor 2>/dev/null || true
fi

if [ -d /home/plasma ]; then
  mkdir -p /home/plasma/.config/gtk-3.0
  printf '[Settings]\ngtk-icon-theme-name=Adwaita\n' > /home/plasma/.config/gtk-3.0/settings.ini
  chown -R plasma:plasma /home/plasma/.config/gtk-3.0 2>/dev/null || true
fi
exit 0
""".replace('§', '$'), true),
        Entry("usr/local/lib/plasma/patches/dbus-session.sh", """#!/bin/bash
# This file belongs to the PlasmaDroid project.
#
# Copyright (C) 2026 ElxSnowkk (https://github.com/ElxSnowkk)
#
# PlasmaDroid is free software: you can redistribute it and/or modify it under the
# terms of the GNU General Public License as published by the Free Software
# Foundation, either version 3 of the License, or (at your option) any later version.
#
# Licensed under GPL-3.0-or-later. See the LICENSE file at the project root,
# or <https://www.gnu.org/licenses/gpl-3.0.html>.
#
# This program is distributed in the hope that it will be useful, but WITHOUT ANY
# WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
# PARTICULAR PURPOSE. See the GNU General Public License for more details.

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
        Entry("usr/local/lib/plasma/patches/dns.sh", """#!/bin/bash
# This file belongs to the PlasmaDroid project.
#
# Copyright (C) 2026 ElxSnowkk (https://github.com/ElxSnowkk)
#
# PlasmaDroid is free software: you can redistribute it and/or modify it under the
# terms of the GNU General Public License as published by the Free Software
# Foundation, either version 3 of the License, or (at your option) any later version.
#
# Licensed under GPL-3.0-or-later. See the LICENSE file at the project root,
# or <https://www.gnu.org/licenses/gpl-3.0.html>.
#
# This program is distributed in the hope that it will be useful, but WITHOUT ANY
# WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
# PARTICULAR PURPOSE. See the GNU General Public License for more details.

# DNS do guest. Ordem de preferência:
#   1) os DNS da rede ativa do aparelho, que o host (HostDns.kt) grava em /var/lib/plasma/host-dns a cada abertura;
#   2) se eles não resolvem github.com, os DNS públicos (1.1.1.1, 8.8.8.8, ...).
# Antes este script escrevia 1.1.1.1/8.8.8.8 fixos e mantinha qualquer nameserver que já existisse; em redes que
# bloqueiam ou interceptam UDP/53 para DNS públicos, o guest ficava sem resolver nada (loja do Pi-Apps vazia).
# PLASMA_* só existem para testar o script fora do guest.
set -u
RESOLV="§{PLASMA_RESOLV:-/etc/resolv.conf}"
HOSTS="§{PLASMA_HOSTS:-/etc/hosts}"
HOST_DNS="§{PLASMA_HOST_DNS:-/var/lib/plasma/host-dns}"

# só IPs (v4/v6) gravados pelo host
host_servers() {
  [ -s "§HOST_DNS" ] || return 0
  grep -E '^[0-9A-Fa-f.:]+§' "§HOST_DNS" | head -n 2 | tr '\n' ' ' | sed 's/ *§//'
}
current_servers() {
  [ -f "§RESOLV" ] && [ ! -L "§RESOLV" ] || return 0
  awk '§1=="nameserver"{print §2}' "§RESOLV" | tr '\n' ' ' | sed 's/ *§//'
}
write_resolv() {
  local tmp="§RESOLV.plasma-tmp" s
  : > "§tmp" || return 1
  for s in "§@"; do echo "nameserver §s" >> "§tmp"; done
  echo "options timeout:2 attempts:3" >> "§tmp"
  rm -f "§RESOLV"; mv "§tmp" "§RESOLV"
}
resolves() { getent hosts github.com >/dev/null 2>&1 && getent hosts raw.githubusercontent.com >/dev/null 2>&1; }

HOSTDNS=§(host_servers)
WANT="§{HOSTDNS:-1.1.1.1 8.8.8.8 9.9.9.9}"
if [ "§(current_servers)" != "§WANT" ]; then
  set -- §WANT; write_resolv "§@"
fi
if ! resolves; then
  for set in "1.1.1.1 8.8.8.8" "8.8.8.8 8.8.4.4" "9.9.9.9 149.112.112.112" "94.140.14.14 208.67.222.222"; do
    set -- §set; write_resolv "§@"
    resolves && break
  done
  # nada resolveu: volta para a lista da rede do aparelho (é a que o Android usa)
  if ! resolves && [ -n "§HOSTDNS" ]; then set -- §HOSTDNS; write_resolv "§@"; fi
fi
grep -qE '^127\.0\.0\.1[[:space:]]+localhost' /etc/hosts 2>/dev/null || echo "127.0.0.1 localhost" >> /etc/hosts
grep -qE '^::1[[:space:]]' /etc/hosts 2>/dev/null || echo "::1 localhost ip6-localhost ip6-loopback" >> /etc/hosts
h=§(cat /etc/hostname 2>/dev/null); [ -n "§h" ] && ! grep -qw "§h" /etc/hosts && echo "127.0.1.1 §h" >> /etc/hosts
if resolves; then exit 0; fi
echo "[plasma] dns: não consegui resolver github.com (sem internet no Android/Wi-Fi?). O resolv.conf foi ajustado; teste: getent hosts github.com" >&2
exit 0
""".replace('§', '$'), true),
        Entry("usr/local/lib/plasma/patches/dpkg-systemd.sh", """#!/bin/bash
# This file belongs to the PlasmaDroid project.
#
# Copyright (C) 2026 ElxSnowkk (https://github.com/ElxSnowkk)
#
# PlasmaDroid is free software: you can redistribute it and/or modify it under the
# terms of the GNU General Public License as published by the Free Software
# Foundation, either version 3 of the License, or (at your option) any later version.
#
# Licensed under GPL-3.0-or-later. See the LICENSE file at the project root,
# or <https://www.gnu.org/licenses/gpl-3.0.html>.
#
# This program is distributed in the hope that it will be useful, but WITHOUT ANY
# WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
# PARTICULAR PURPOSE. See the GNU General Public License for more details.

set -u
printf '#!/bin/sh\nexit 101\n' > /usr/sbin/policy-rc.d; chmod 755 /usr/sbin/policy-rc.d

for b in systemd-sysusers systemd-tmpfiles; do
  p=/usr/bin/§b
  if ! dpkg-divert --list 2>/dev/null | grep -q "diversion of §p to"; then
    dpkg-divert --local --rename --divert "§p.distrib" --add "§p" >/dev/null
  fi
  printf '#!/bin/sh\nexit 0\n' > "§p"; chmod 755 "§p"
done

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
# This file belongs to the PlasmaDroid project.
#
# Copyright (C) 2026 ElxSnowkk (https://github.com/ElxSnowkk)
#
# PlasmaDroid is free software: you can redistribute it and/or modify it under the
# terms of the GNU General Public License as published by the Free Software
# Foundation, either version 3 of the License, or (at your option) any later version.
#
# Licensed under GPL-3.0-or-later. See the LICENSE file at the project root,
# or <https://www.gnu.org/licenses/gpl-3.0.html>.
#
# This program is distributed in the hope that it will be useful, but WITHOUT ANY
# WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
# PARTICULAR PURPOSE. See the GNU General Public License for more details.

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
rm -f /var/lib/plasma/gl-backend
/usr/local/bin/plasma-gl default on >/dev/null 2>&1 || true
""".replace('§', '$'), true),
        Entry("usr/local/lib/plasma/patches/locale.sh", """#!/bin/bash
# This file belongs to the PlasmaDroid project.
#
# Copyright (C) 2026 ElxSnowkk (https://github.com/ElxSnowkk)
#
# PlasmaDroid is free software: you can redistribute it and/or modify it under the
# terms of the GNU General Public License as published by the Free Software
# Foundation, either version 3 of the License, or (at your option) any later version.
#
# Licensed under GPL-3.0-or-later. See the LICENSE file at the project root,
# or <https://www.gnu.org/licenses/gpl-3.0.html>.
#
# This program is distributed in the hope that it will be useful, but WITHOUT ANY
# WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
# PARTICULAR PURPOSE. See the GNU General Public License for more details.

set -u
export DEBIAN_FRONTEND=noninteractive
command -v locale-gen >/dev/null 2>&1 || apt-get install -y --no-install-recommends locales >/dev/null 2>&1 || true
if [ -f /etc/locale.gen ]; then
  for l in en_US.UTF-8 pt_BR.UTF-8; do
    if grep -q "^# *§l" /etc/locale.gen; then sed -i "s/^# *§l/§l/" /etc/locale.gen
    elif ! grep -q "^§l" /etc/locale.gen; then echo "§l UTF-8" >> /etc/locale.gen; fi
  done
fi
locale-gen >/dev/null 2>&1 || true
printf 'LANG=C.UTF-8\n' > /etc/default/locale
for l in en_US pt_BR; do
  if [ -d /usr/lib/locale/C.utf8 ] && ! LC_ALL="§l.UTF-8" locale charmap 2>/dev/null | grep -qi '^utf-8§'; then
    rm -rf "/usr/lib/locale/§l.utf8" "/usr/lib/locale/§l.UTF-8"
    ln -s C.utf8 "/usr/lib/locale/§l.utf8"
  fi
done
exit 0
""".replace('§', '$'), true),
        Entry("usr/local/lib/plasma/patches/os-arch.sh", """#!/bin/bash
# This file belongs to the PlasmaDroid project.
#
# Copyright (C) 2026 ElxSnowkk (https://github.com/ElxSnowkk)
#
# PlasmaDroid is free software: you can redistribute it and/or modify it under the
# terms of the GNU General Public License as published by the Free Software
# Foundation, either version 3 of the License, or (at your option) any later version.
#
# Licensed under GPL-3.0-or-later. See the LICENSE file at the project root,
# or <https://www.gnu.org/licenses/gpl-3.0.html>.
#
# This program is distributed in the hope that it will be useful, but WITHOUT ANY
# WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
# PARTICULAR PURPOSE. See the GNU General Public License for more details.

set -u
mkdir -p /usr/sbin
if [ ! -e /usr/sbin/init ]; then
  rm -f /usr/sbin/init
  ln -s /usr/bin/bash /usr/sbin/init
fi
[ -e /sbin/init ] || { [ -d /sbin ] && ln -s /usr/bin/bash /sbin/init 2>/dev/null; }
b=§(od -An -t x1 -j4 -N1 /usr/sbin/init 2>/dev/null | tr -d ' \n')
[ "§b" = 02 ] || { echo "[plasma] os-arch: /usr/sbin/init não é um ELF 64 bits (byte=§b)" >&2; exit 1; }
exit 0
""".replace('§', '$'), true),
        Entry("usr/local/lib/plasma/patches/pi-user.sh", """#!/bin/bash
# This file belongs to the PlasmaDroid project.
#
# Copyright (C) 2026 ElxSnowkk (https://github.com/ElxSnowkk)
#
# PlasmaDroid is free software: you can redistribute it and/or modify it under the
# terms of the GNU General Public License as published by the Free Software
# Foundation, either version 3 of the License, or (at your option) any later version.
#
# Licensed under GPL-3.0-or-later. See the LICENSE file at the project root,
# or <https://www.gnu.org/licenses/gpl-3.0.html>.
#
# This program is distributed in the hope that it will be useful, but WITHOUT ANY
# WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
# PARTICULAR PURPOSE. See the GNU General Public License for more details.

set -u
cat > /usr/local/bin/plasma-as <<'SH'
#!/bin/bash
u="§{1:-}"; [ -n "§u" ] || { echo "uso: plasma-as <usuário> [comando...]" >&2; exit 2; }
shift
ent=§(getent passwd "§u") || { echo "plasma-as: usuário inexistente: §u" >&2; exit 1; }
uid=§(echo "§ent" | cut -d: -f3); gid=§(echo "§ent" | cut -d: -f4); home=§(echo "§ent" | cut -d: -f6)
[ -d "§home" ] || mkdir -p "§home"
[ §# -gt 0 ] || set -- /bin/bash -l
run=(env HOME="§home" USER="§u" LOGNAME="§u" SHELL=/bin/bash "§@")

# Raw syscall via perl (pula o wrapper "setxid" da glibc, que sob o PRoot falha com "setresgid failed: Success").
case "§(uname -m)" in
  aarch64|arm64) SG=149; SU=147 ;;
  x86_64)        SG=119; SU=117 ;;
  arm*)          SG=210; SU=208 ;;
  i?86)          SG=170; SU=164 ;;
  *)             SG=; SU= ;;
esac
PERL_AS='my(§m,§sg,§su,§g,§u,@c)=@ARGV;
my §r=syscall(§sg,§g,§g,§g); if(§r!=0 && §m eq "strict"){print STDERR "setresgid: §!\n"; exit 1}
§r=syscall(§su,§u,§u,§u);    if(§r!=0 && §m eq "strict"){print STDERR "setresuid: §!\n"; exit 1}
exec {§c[0]} @c; print STDERR "exec: §!\n"; exit 127;'

# via <método> comando...  (sempre faz exec; o teste roda dentro de um subshell)
via() {
  local m="§1"; shift
  case "§m" in
    setpriv)       exec setpriv --reuid="§uid" --regid="§gid" --clear-groups "§@" ;;
    runuser)       exec runuser -u "§u" -- "§@" ;;
    su)            exec su -s /bin/bash "§u" -c 'exec "§@"' -- _ "§@" ;;
    perl)          exec perl -e "§PERL_AS" strict "§SG" "§SU" "§gid" "§uid" "§@" ;;
    perl-lenient)  exec perl -e "§PERL_AS" lenient "§SG" "§SU" "§gid" "§uid" "§@" ;;
  esac
}

# Usa o primeiro método que realmente funciona neste PRoot (testado com `true` antes de executar o comando de verdade).
for m in setpriv runuser su perl perl-lenient; do
  bin="§{m%%-*}"
  command -v "§bin" >/dev/null 2>&1 || continue
  case "§m" in perl*) [ -n "§SG" ] || continue ;; esac
  ( via "§m" env true ) </dev/null >/dev/null 2>&1 || continue
  via "§m" "§{run[@]}"
done
echo "plasma-as: não consegui trocar para o usuário §u (setresgid/setresuid barrado pelo PRoot)" >&2
exit 1
SH
chmod 755 /usr/local/bin/plasma-as

cat > /usr/local/bin/sudo <<'SH'
#!/bin/bash
target=root; shell=0
while [ §# -gt 0 ]; do
  case "§1" in
    -u) target="§2"; shift 2 ;;
    --user=*) target="§{1#*=}"; shift ;;
    -g|-C|-D|-h|-p|-r|-t|-T|-U) shift 2 ;;
    -v|-k|-K|-l|--validate|--reset-timestamp) exit 0 ;;
    -i|-s) shell=1; shift ;;
    --) shift; break ;;
    -*) shift ;;
    *) break ;;
  esac
done
if [ "§(id -un 2>/dev/null)" = "§target" ] || { [ "§(id -u)" = 0 ] && [ "§target" = root ]; }; then
  [ §# -gt 0 ] && exec "§@"
  exec /bin/bash -l
fi
if [ §# -eq 0 ]; then set -- /bin/bash -l; fi
exec /usr/local/bin/plasma-as "§target" "§@"
SH
chmod 755 /usr/local/bin/sudo

U=plasma; ID=1000
getent group "§U" >/dev/null || { echo "§U:x:§ID:" >> /etc/group; [ -f /etc/gshadow ] && echo "§U:!::" >> /etc/gshadow; }
if ! getent passwd "§U" >/dev/null; then
  echo "§U:x:§ID:§ID:Plasma:/home/§U:/bin/bash" >> /etc/passwd
  [ -f /etc/shadow ] && echo "§U:*:20000:0:99999:7:::" >> /etc/shadow
fi
for g in sudo audio video input render; do
  getent group "§g" >/dev/null && ! getent group "§g" | grep -q "[:,]§U\(,\|\§\)" && sed -i "s/^\(§g:.*[^:]\)\§/\1,§U/;s/^\(§g:.*:\)\§/\1§U/" /etc/group
done
mkdir -p "/home/§U" /etc/sudoers.d
[ -f "/home/§U/.bashrc" ] || cp /etc/skel/.bashrc "/home/§U/.bashrc" 2>/dev/null || true
grep -q '# plasmadroid' "/home/§U/.bashrc" 2>/dev/null || printf '\n# plasmadroid\nexport LANG=C.UTF-8\n' >> "/home/§U/.bashrc"
chown -R "§ID:§ID" "/home/§U" 2>/dev/null || true
echo "§U ALL=(ALL) NOPASSWD:ALL" > "/etc/sudoers.d/§U"; chmod 440 "/etc/sudoers.d/§U"
exit 0
""".replace('§', '$'), true),
        Entry("usr/local/lib/plasma/patches/quiet.sh", """#!/bin/bash
# This file belongs to the PlasmaDroid project.
#
# Copyright (C) 2026 ElxSnowkk (https://github.com/ElxSnowkk)
#
# PlasmaDroid is free software: you can redistribute it and/or modify it under the
# terms of the GNU General Public License as published by the Free Software
# Foundation, either version 3 of the License, or (at your option) any later version.
#
# Licensed under GPL-3.0-or-later. See the LICENSE file at the project root,
# or <https://www.gnu.org/licenses/gpl-3.0.html>.
#
# This program is distributed in the hope that it will be useful, but WITHOUT ANY
# WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
# PARTICULAR PURPOSE. See the GNU General Public License for more details.

set -u
mkdir -p /opt /usr/local/bin /usr/local/share/applications /usr/local/share/icons
cat > /etc/profile.d/plasma-quiet.sh <<'SH'
# PlasmaDroid: ambiente que evita avisos/erros barulhentos
case ":§{XDG_DATA_DIRS:-}:" in
  *:/usr/local/share:*) ;;
  *) export XDG_DATA_DIRS="/usr/local/share:§{XDG_DATA_DIRS:-/usr/share}" ;;
esac
export NO_AT_BRIDGE=1
SH
chmod 644 /etc/profile.d/plasma-quiet.sh
for rc in /etc/bash.bashrc; do
  [ -f "§rc" ] || continue
  grep -q 'plasma-quiet.sh' "§rc" || printf '\n[ -f /etc/profile.d/plasma-quiet.sh ] && . /etc/profile.d/plasma-quiet.sh\n' >> "§rc"
done
for h in /root /home/plasma; do
  [ -d "§h" ] || continue
  [ -d "§h/Desktop" ] || { rm -f "§h/Desktop"; mkdir -p "§h/Desktop"; }
  [ "§h" = /root ] || chown "§(stat -c %u:%g "§h")" "§h/Desktop" 2>/dev/null
done
exit 0
""".replace('§', '$'), true),
        Entry("usr/local/lib/plasma/patches/wayland-env.sh", """#!/bin/bash
# This file belongs to the PlasmaDroid project.
#
# Copyright (C) 2026 ElxSnowkk (https://github.com/ElxSnowkk)
#
# PlasmaDroid is free software: you can redistribute it and/or modify it under the
# terms of the GNU General Public License as published by the Free Software
# Foundation, either version 3 of the License, or (at your option) any later version.
#
# Licensed under GPL-3.0-or-later. See the LICENSE file at the project root,
# or <https://www.gnu.org/licenses/gpl-3.0.html>.
#
# This program is distributed in the hope that it will be useful, but WITHOUT ANY
# WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
# PARTICULAR PURPOSE. See the GNU General Public License for more details.

set -u
mkdir -p /usr/local/lib/plasma
cat > /usr/local/lib/plasma/env.sh <<'SH'
# carregado por `plasma run` e pelos launchers de apps (. env.sh)
export XDG_RUNTIME_DIR="§{XDG_RUNTIME_DIR:-/run/plasma-wayland}"
export WAYLAND_DISPLAY="§{WAYLAND_DISPLAY:-wayland-0}"
case ":§{XDG_DATA_DIRS:-}:" in *:/usr/local/share:*) ;; *) export XDG_DATA_DIRS="/usr/local/share:§{XDG_DATA_DIRS:-/usr/share}" ;; esac
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
        Entry("usr/local/lib/plasma/apps/chromium.sh", """#!/bin/bash
# This file belongs to the PlasmaDroid project.
#
# Copyright (C) 2026 ElxSnowkk (https://github.com/ElxSnowkk)
#
# Licensed under GPL-3.0-or-later. See the LICENSE file at the project root,
# or <https://www.gnu.org/licenses/gpl-3.0.html>.

. /usr/local/lib/plasma/lib.sh
need_root

PKGS="chromium mesa-vulkan-drivers vulkan-tools"

apply_base_patches || exit 1
apt_install §PKGS || exit 1

# Flags padrão do Debian lidas por /usr/bin/chromium. Sem GPU por padrão: o processo de GPU
# morre no PRoot (drmGetDevices2 sem nós, syscalls interceptadas). Para testar GPU, rode
# `CHROMIUM_FLAGS=... chromium` ou remova --disable-gpu deste arquivo.
mkdir -p /etc/chromium.d
cat > /etc/chromium.d/plasmadroid <<'CFG'
CHROMIUM_FLAGS="§CHROMIUM_FLAGS --ozone-platform=wayland --disable-gpu --no-sandbox --disable-dev-shm-usage"
CFG

echo "[chromium] instalado. Abra com: chromium"
echo "[chromium] GPU/Vulkan: ainda não funciona no PRoot; /dev/dri precisa existir no host Android."
""".replace('§', '$'), true),
        Entry("usr/local/lib/plasma/apps/fex.sh", """#!/bin/bash
# This file belongs to the PlasmaDroid project.
#
# Copyright (C) 2026 ElxSnowkk (https://github.com/ElxSnowkk)
#
# Licensed under GPL-3.0-or-later. See the LICENSE file at the project root,
# or <https://www.gnu.org/licenses/gpl-3.0.html>.
#
# This program is distributed in the hope that it will be useful, but WITHOUT ANY
# WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
# PARTICULAR PURPOSE. See the GNU General Public License for more details.

# desc: FEX-Emu: roda programas x86-64 num Debian aarch64 (com barra de progresso)

. /usr/local/lib/plasma/lib.sh
need_root

FEX_TGZ=/usr/local/lib/plasma/fex/fex-aarch64.tar.xz
FEX_READY=/usr/local/lib/plasma/fex/.ready
RFS_DIR=/home/plasma/.local/share/fex-emu/RootFS
RFS_MIN=debian-min
RFS_ROOT="§RFS_DIR/§RFS_MIN"
MIN_SQ=/usr/local/lib/plasma/fex/rootfs-debian-min.sqsh
HOST_SQ=/usr/local/lib/plasma/fex-host/rootfs-debian-min.sqsh
STAGE2_DONE="§RFS_ROOT/var/lib/plasma-stage2.done"
UNSQ_LOG=/var/log/plasma-fex-unsquash.log
STAGE2_LOG=/var/log/plasma-fex-stage2.log
NPROC=§(nproc 2>/dev/null || echo 1)

rm -f "§FEX_READY"
fex_cleanup() {
  local rc=§?
  if [ "§rc" -ne 0 ]; then
    rm -f /usr/local/bin/fex-run "§FEX_READY"
    printf '\n'
    say "instalação do FEX não terminou (código §rc): launcher removido; rode plasma install fex de novo"
  fi
}
trap fex_cleanup EXIT
trap 'exit 130' INT TERM HUP

pbar() {
  local cur=§{1:-0} tot=§{2:-1} lbl=§3 t=§4 w=28 pct fill i bar=""
  [ "§tot" -gt 0 ] 2>/dev/null || tot=1
  pct=§(( cur * 100 / tot ))
  [ "§pct" -gt 100 ] && pct=100
  [ "§pct" -lt 0 ] && pct=0
  fill=§(( pct * w / 100 ))
  for ((i = 0; i < w; i++)); do
    if [ "§i" -lt "§fill" ]; then bar+="#"; else bar+="."; fi
  done
  printf '\r[plasma] %-36s [%s] %3d%% %4ds' "§lbl" "§bar" "§pct" "§t"
}

run_bar() {
  local lbl=§1 tot=§2 probe=§3 pid cur=0 rc t0=§SECONDS
  shift 3
  "§@" >>"§RUN_LOG" 2>&1 &
  pid=§!
  while kill -0 "§pid" 2>/dev/null; do
    cur=§(§probe 2>/dev/null)
    pbar "§{cur:-0}" "§tot" "§lbl" §((SECONDS - t0))
    sleep 1
  done
  wait "§pid"; rc=§?
  if [ "§rc" -eq 0 ]; then
    pbar "§tot" "§tot" "§lbl" §((SECONDS - t0))
  else
    pbar "§{cur:-0}" "§tot" "§lbl" §((SECONDS - t0))
    printf '  FALHOU\n'
    return "§rc"
  fi
  printf '\n'
}

probe_unsq() { grep -o '[0-9]\+%' "§UNSQ_LOG" 2>/dev/null | tail -1 | tr -d '%'; }
probe_stage2() { grep -c '^Status: install ok installed' "§RFS_ROOT/var/lib/dpkg/status" 2>/dev/null; }

[ -f "§FEX_TGZ" ] || { say "pacote do FEX ausente (§FEX_TGZ): o APK não traz o FEX; gere com scripts/build-fex.sh"; exit 1; }
[ "§(uname -m)" = aarch64 ] || { say "FEX só roda em aarch64 (este guest é §(uname -m))"; exit 1; }

apply_base_patches || exit 1
apt_install xz-utils squashfs-tools curl ca-certificates || exit 1

say "extraindo o runtime do FEX em /usr/local"
tar -C / -xJf "§FEX_TGZ" || { say "falhou ao extrair o FEX"; exit 1; }
ldconfig 2>/dev/null || true
[ -x /usr/local/bin/FEXInterpreter ] || { say "FEXInterpreter não apareceu após extrair"; exit 1; }

cat > /usr/local/bin/fex-run <<'FEXRUN'
#!/bin/bash
# fex-run <programa x86-64> [args]: executa um binário x86-64 pelo FEX.
# ~/.fex-emu/profile: compatible | balanced (padrão) | fast.
# ~/.fex-emu/no-tso-libs: bibliotecas que rodam sem emulação de TSO (uma por linha),
#   só com TSO ligado. Mantém o resto do programa com TSO: desligar tudo trava jogos Source 2.
# Cache de JIT em ~/.cache/fex-emu, que sobrevive às reinstalações do rootfs.
[ §# -gt 0 ] || { echo "uso: fex-run <programa> [args]" >&2; exit 2; }
H="§{HOME:-/root}"
CFG_DIR="§H/.fex-emu"
CACHE_DIR="§H/.cache/fex-emu"
LIBS="§CFG_DIR/no-tso-libs"
mkdir -p "§CFG_DIR" "§CACHE_DIR"
if [ ! -f "§LIBS" ]; then
  cat > "§LIBS" <<'LIBLIST'
# Bibliotecas de jogos Source 2 (Dota 2, CS2) que rodam sem TSO. Uma por linha; # comenta.
libclient.so
libserver.so
libengine2.so
libpanorama.so
librendersystemvulkan.so
libmaterialsystem2.so
libscenesystem.so
libparticles.so
libanimationsystem.so
libvphysics2.so
libworldrenderer.so
libmeshsystem.so
libsoundsystem.so
libresourcesystem.so
libpanorama_text_pango.so
libschemasystem.so
libvscript.so
libv8.so
LIBLIST
fi
PERFIL=§(cat "§CFG_DIR/profile" 2>/dev/null || echo balanced)
case "§PERFIL" in
  compatible) TSO=1; VTSO=1; MTSO=1; HTSO=1; X87=0 ;;
  fast)       TSO=0; VTSO=0; MTSO=0; HTSO=0; X87=1 ;;
  *)          TSO=1; VTSO=0; MTSO=0; HTSO=1; X87=1 ;;
esac
VOL=""
if [ "§TSO" = 1 ]; then
  VOL=§(grep -v '^[[:space:]]*#' "§LIBS" 2>/dev/null | grep -v '^[[:space:]]*§' | paste -sd: -)
fi
RFS=§(sed -n 's/.*"RootFS": *"\([^"]*\)".*/\1/p' "§CFG_DIR/Config.json" 2>/dev/null | head -1)
[ -n "§RFS" ] || RFS=debian-min
{
  printf '{\n  "Config": {\n'
  printf '    "RootFS": "%s",\n' "§RFS"
  printf '    "TSOEnabled": "%s",\n' "§TSO"
  printf '    "VectorTSOEnabled": "%s",\n' "§VTSO"
  printf '    "MemcpySetTSOEnabled": "%s",\n' "§MTSO"
  printf '    "HalfBarrierTSOEnabled": "%s",\n' "§HTSO"
  printf '    "X87ReducedPrecision": "%s",\n' "§X87"
  [ -n "§VOL" ] && printf '    "ExtendedVolatileMetadata": "%s",\n' "§VOL"
  printf '    "Multiblock": "1",\n'
  printf '    "DiskCache": "1"\n'
  printf '  }\n}\n'
} > "§CFG_DIR/Config.json.tmp" && mv "§CFG_DIR/Config.json.tmp" "§CFG_DIR/Config.json"
export FEX_ROOTFS="§H/.local/share/fex-emu/RootFS/§RFS"
export FEX_APP_CONFIG_LOCATION="§CFG_DIR/"
export FEX_APP_DATA_LOCATION="§CFG_DIR/"
export FEX_APP_CACHE_LOCATION="§CACHE_DIR/"
exec /usr/local/bin/FEXInterpreter "§@"
FEXRUN
chmod 755 /usr/local/bin/fex-run

if [ ! -f "§MIN_SQ" ] && [ -f "§HOST_SQ" ]; then
  mkdir -p "§(dirname "§MIN_SQ")" && cp "§HOST_SQ" "§MIN_SQ.part" && mv "§MIN_SQ.part" "§MIN_SQ"
fi

X86_NAME=debian-x86
ROOTFS_FORMATO=1
X86_ROOT="§RFS_DIR/§X86_NAME"
ROOTFS_BASE="§{PLASMA_ROOTFS_BASE:-https://github.com/OWNER/REPO/releases/download/debian-x86-rootfs}"

baixar_rootfs_x86() {
  local tgz=/tmp/debian-x86-rootfs.tar.gz sum=/tmp/debian-x86-rootfs.tar.gz.sha256
  mkdir -p "§RFS_DIR"
  say "baixando o rootfs x86-64 pronto (Debian 13): §ROOTFS_BASE"
  curl -fL --retry 3 -o "§sum" "§ROOTFS_BASE/debian-x86-rootfs.tar.gz.sha256" || return 1
  curl -fL --retry 3 -C - -o "§tgz" "§ROOTFS_BASE/debian-x86-rootfs.tar.gz" || return 1
  (cd /tmp && sha256sum -c debian-x86-rootfs.tar.gz.sha256) || { say "checksum do rootfs não bate: baixe de novo"; rm -f "§tgz"; return 1; }
  say "extraindo o rootfs x86-64"
  rm -rf "§X86_ROOT.tmp" && mkdir -p "§X86_ROOT.tmp"
  tar -xzf "§tgz" -C "§X86_ROOT.tmp" || { rm -rf "§X86_ROOT.tmp"; return 1; }
  printf 'format=%s %s\n' "§ROOTFS_FORMATO" "§(cat "§sum")" > "§X86_ROOT.tmp/.plasma-rootfs"
  rm -rf "§X86_ROOT" && mv "§X86_ROOT.tmp" "§X86_ROOT"
  rm -f "§tgz" "§sum"
}

ROOTFS_X86_OK=0
case "§ROOTFS_BASE" in
  *OWNER*|*REPO*) REMOTO_OK=0 ;;
  *) REMOTO_OK=1 ;;
esac
STAMP_ATUAL=§(cat "§X86_ROOT/.plasma-rootfs" 2>/dev/null || true)
SHA_REMOTO=""
if [ "§REMOTO_OK" = 1 ]; then
  SHA_REMOTO=§(curl -fsSL --retry 2 "§ROOTFS_BASE/debian-x86-rootfs.tar.gz.sha256" 2>/dev/null | head -1 || true)
fi
if [ -n "§SHA_REMOTO" ] && [ "§STAMP_ATUAL" = "format=§ROOTFS_FORMATO §SHA_REMOTO" ]; then
  ROOTFS_X86_OK=1
  say "rootfs x86-64 atualizado (§X86_NAME)"
elif [ -n "§SHA_REMOTO" ]; then
  baixar_rootfs_x86 && ROOTFS_X86_OK=1 || say "download do rootfs pronto falhou"
elif [ -n "§STAMP_ATUAL" ]; then
  ROOTFS_X86_OK=1
  say "sem a release do rootfs: usando o que já está instalado (§X86_NAME)"
else
  [ "§REMOTO_OK" = 1 ] || say "download do rootfs pronto desativado: defina PLASMA_ROOTFS_BASE com a URL da release"
fi

ROOTFS_OK=1
if [ "§ROOTFS_X86_OK" = 1 ]; then
  mkdir -p /home/plasma/.fex-emu "§X86_ROOT/var/lib"
  printf '{\n  "Config": {\n    "RootFS": "%s"\n  }\n}\n' "§X86_NAME" > /home/plasma/.fex-emu/Config.json
  printf 'ok\n' > "§X86_ROOT/var/lib/plasma-stage2.done"
  chown -R plasma:plasma /home/plasma/.local/share/fex-emu /home/plasma/.fex-emu 2>/dev/null || true
  say "rootfs x86-64 pronto: segunda etapa do debootstrap não é necessária"
else
if [ -f "§MIN_SQ" ] && [ ! -x "§RFS_ROOT/usr/bin/dpkg" ]; then
  rm -rf "§RFS_ROOT.tmp"; mkdir -p "§RFS_DIR"
  { echo "== unsquashfs §(date)"; df -h /home/plasma 2>&1; ls -la "§MIN_SQ"; } >"§UNSQ_LOG" 2>&1
  RUN_LOG=§UNSQ_LOG
  run_bar "extraindo rootfs x86-64 (§NPROC threads)" 100 probe_unsq \
    unsquashfs -percentage -processors "§NPROC" -ignore-errors -d "§RFS_ROOT.tmp" "§MIN_SQ" || true
  if [ -d "§RFS_ROOT.tmp/usr" ]; then
    rm -rf "§RFS_ROOT"; mv "§RFS_ROOT.tmp" "§RFS_ROOT"
  else
    rm -rf "§RFS_ROOT.tmp"; ROOTFS_OK=0
    say "falhou ao extrair o rootfs mínimo. Log: §UNSQ_LOG (me mande as últimas linhas)"
  fi
  if [ "§ROOTFS_OK" = 1 ] && [ ! -f /home/plasma/.fex-emu/Config.json ]; then
    mkdir -p /home/plasma/.fex-emu
    printf '{\n  "Config": {\n    "RootFS": "%s"\n  }\n}\n' "§RFS_MIN" > /home/plasma/.fex-emu/Config.json
  fi
  chown -R plasma:plasma /home/plasma/.local/share/fex-emu /home/plasma/.fex-emu 2>/dev/null || true
fi

if [ "§ROOTFS_OK" = 1 ] && [ -x "§RFS_ROOT/debootstrap/debootstrap" ] && [ ! -f "§STAGE2_DONE" ]; then
  mkdir -p "§RFS_ROOT/etc/dpkg/dpkg.cfg.d" "§RFS_ROOT/etc/apt/apt.conf.d"
  cat > "§RFS_ROOT/etc/dpkg/dpkg.cfg.d/99plasma-fast" <<'DPKG'
force-unsafe-io
path-exclude=/usr/share/man/*
path-exclude=/usr/share/doc/*
path-exclude=/usr/share/info/*
DPKG
  cat > "§RFS_ROOT/etc/apt/apt.conf.d/99plasma-fast" <<'APT'
APT::Install-Recommends "false";
Acquire::Retries "3";
APT
  contar_status() { local n; n=§(grep -c "^Status: install ok §1" "§RFS_ROOT/var/lib/dpkg/status" 2>/dev/null); echo "§{n:-0}"; }
  TOT2=§(( §(contar_status unpacked) + §(contar_status installed) ))
  [ "§TOT2" -gt 0 ] || TOT2=1
  RUN_LOG=§STAGE2_LOG
  if run_bar "configurando rootfs x86-64 (FEX)" "§TOT2" probe_stage2 \
      env HOME=/home/plasma /usr/local/bin/FEXBash -c '/debootstrap/debootstrap --second-stage'; then
    printf 'ok\n' > "§STAGE2_DONE"
  else
    ROOTFS_OK=0
    say "segunda etapa do debootstrap falhou (log: §STAGE2_LOG). Rode de novo: plasma install fex"
  fi
fi

if [ -x "§RFS_ROOT/usr/bin/dpkg" ]; then
  say "rootfs mínimo x86-64 presente"
elif ls -A "§RFS_DIR" 2>/dev/null | grep -q .; then
  say "rootfs x86-64 já existe"
else
  ROOTFS_OK=0
  say "rootfs mínimo ausente (§MIN_SQ). Gere com scripts/build-fex-rootfs.sh, gere o APK de novo e reinstale o app"
fi
fi

touch "§FEX_READY"
if [ "§ROOTFS_OK" = 1 ]; then say "FEX pronto. Rode programas x86-64 com: fex-run <programa>"
else say "AVISO: FEX instalado, mas SEM rootfs x86-64"; fi
""".replace('§', '$'), true),
        Entry("usr/local/lib/plasma/apps/kdenlive.sh", """#!/bin/bash
# This file belongs to the PlasmaDroid project.
#
# Copyright (C) 2026 ElxSnowkk (https://github.com/ElxSnowkk)
#
# PlasmaDroid is free software: you can redistribute it and/or modify it under the
# terms of the GNU General Public License as published by the Free Software
# Foundation, either version 3 of the License, or (at your option) any later version.
#
# Licensed under GPL-3.0-or-later. See the LICENSE file at the project root,
# or <https://www.gnu.org/licenses/gpl-3.0.html>.
#
# This program is distributed in the hope that it will be useful, but WITHOUT ANY
# WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
# PARTICULAR PURPOSE. See the GNU General Public License for more details.

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
        Entry("usr/local/lib/plasma/apps/pi-apps.sh", """#!/bin/bash
# This file belongs to the PlasmaDroid project.
#
# Copyright (C) 2026 ElxSnowkk (https://github.com/ElxSnowkk)
#
# Licensed under GPL-3.0-or-later. See the LICENSE file at the project root,
# or <https://www.gnu.org/licenses/gpl-3.0.html>.

# Pi-Apps: versão Bash (repositório Botspot/pi-apps, rodado pelo ./gui, X11 nativo do PlasmaDroid por padrão e
# Wayland como reserva). Não usa binário Go: o repositório Bash traz tudo (apps/, scripts, settings, gui).
# A pasta ~/pi-apps nunca é apagada: ela pode ter dados do usuário.

. /usr/local/lib/plasma/lib.sh
need_root

PKGS="libgtk-3-0 libglib2.0-bin adwaita-icon-theme dbus-x11 ca-certificates curl yad xdotool wmctrl x11-utils"
PIAPPS_HOME=/home/plasma/pi-apps
PIAPPS_REPO_TGZ=https://github.com/Botspot/pi-apps/archive/refs/heads/master.tar.gz

# baixa o repositório Bash (catálogo + scripts) para ~/pi-apps, se ainda não estiver lá
ensure_pi_apps_repo() {
  if [ -d "§PIAPPS_HOME/apps" ] && [ -n "§(ls -A "§PIAPPS_HOME/apps" 2>/dev/null)" ]; then
    say "repositório do Pi-Apps já está em §PIAPPS_HOME"
  else
    local tmp=/tmp/pi-apps-repo.tgz
    say "baixando o repositório do Pi-Apps (catálogo e scripts) para §PIAPPS_HOME"
    curl -fsSL --retry 3 --connect-timeout 20 -o "§tmp" "§PIAPPS_REPO_TGZ" || { say "não consegui baixar o repositório do Pi-Apps (rede/DNS?)"; return 1; }
    mkdir -p "§PIAPPS_HOME" && tar -xzf "§tmp" -C "§PIAPPS_HOME" --strip-components=1 || { rm -f "§tmp"; say "falhou ao extrair o repositório do Pi-Apps"; return 1; }
    rm -f "§tmp"
  fi
  mkdir -p "§PIAPPS_HOME/data" "§PIAPPS_HOME/logs"
  chown -R plasma:plasma "§PIAPPS_HOME"
}

apply_base_patches || exit 1
apply_patch locale || exit 1
apply_patch pi-user || exit 1
apt_install §PKGS || exit 1

# remove só restos de instalações antigas (launcher, binário Go e symlink quebrado), nunca a pasta ~/pi-apps
rm -f /usr/local/bin/plasma-pi-apps-patch /usr/local/bin/pi-apps-go 2>/dev/null
[ -L /home/plasma/pi-apps/settings ] && [ ! -e /home/plasma/pi-apps/settings ] && rm -f /home/plasma/pi-apps/settings

# GUI e repositório na mesma instalação: sem o repositório a loja sai vazia, então falha alto em vez de instalar em silêncio
REPO_OK=1; ensure_pi_apps_repo || REPO_OK=0

# launcher: X11 nativo do PlasmaDroid (Wayland como reserva), roda como o usuário plasma (o Pi-Apps recusa root)
cat > /usr/local/bin/pi-apps <<'LAUNCHER'
#!/bin/bash
# launcher do Pi-Apps (Bash) no PlasmaDroid. Roda como "plasma": o Pi-Apps recusa root.
U=plasma
. /usr/local/lib/plasma/env.sh
if locale -a 2>/dev/null | grep -qi '^en_US\.utf-\?8§'; then export LANG=en_US.UTF-8 LC_ALL=en_US.UTF-8; else export LANG=C.UTF-8 LC_ALL=C.UTF-8; fi
export NO_AT_BRIDGE=1 GTK_A11Y=none
if [ "§(id -u)" = 0 ]; then exec /usr/local/bin/plasma-as "§U" /usr/local/bin/pi-apps "§@"; fi
# GTK3 no Wayland nunca informa "monitor primário": o Pi-Apps acha que a tela é 0x0 ("Small screen detected
# (0x0)"), abre a janela compacta minúscula e a imagem sai cortada/esticada. No servidor X11 nativo do PlasmaDroid
# (RANDR com saída primária e tela = display real, acompanha rotação) o tamanho vem certo.
# PIAPPS_BACKEND=wayland volta ao modo antigo.
D=
if [ "§{PIAPPS_BACKEND:-x11}" = x11 ] && [ -x /usr/local/bin/plasma-x11 ]; then
  D=§(/usr/local/bin/plasma-x11 run 2>/dev/null | tail -1)
fi
case "§D" in
  :[0-9]*) export DISPLAY="§D" GDK_BACKEND=x11 XDG_SESSION_TYPE=x11 ;;
  *) export GDK_BACKEND=wayland ;;
esac
cd /home/plasma/pi-apps || { echo "Pi-Apps não está em /home/plasma/pi-apps. Rode: plasma install pi-apps" >&2; exit 1; }
exec ./gui "§@"
LAUNCHER
chmod 755 /usr/local/bin/pi-apps

mkdir -p /usr/share/applications
cat > /usr/share/applications/pi-apps.desktop <<'DESK'
[Desktop Entry]
Name=Pi-Apps
Comment=Loja de scripts de instalação (Pi-Apps)
Exec=/usr/local/bin/pi-apps
Icon=/usr/share/pixmaps/pi-apps.png
Terminal=false
Type=Application
Categories=System;PackageManager;
StartupNotify=true
DESK

if [ "§REPO_OK" = 1 ]; then say "Pi-Apps instalado com o catálogo. Rode: pi-apps"
else say "AVISO: Pi-Apps instalado, mas SEM o catálogo (sem rede). Com internet, rode de novo: plasma install pi-apps"; fi
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
case ":§{XDG_DATA_DIRS:-}:" in *:/usr/local/share:*) ;; *) export XDG_DATA_DIRS="/usr/local/share:§{XDG_DATA_DIRS:-/usr/share}" ;; esac
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
