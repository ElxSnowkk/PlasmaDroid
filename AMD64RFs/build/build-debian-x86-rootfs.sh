#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/out"
STAGE="$OUT/stage"
IMAGE=debian:trixie-slim

PKGS=(
  ca-certificates curl wget locales procps iproute2 nano less sudo xz-utils
  libvulkan1 vulkan-tools mesa-utils mesa-utils-bin mesa-utils-extra mesa-vulkan-drivers
  squashfs-tools binutils librsvg2-bin
  xauth x11-xkb-utils xkb-data x11-xserver-utils x11-utils openbox
  libgl1-mesa-dri libglx-mesa0 xfonts-base fonts-dejavu-core
)

mkdir -p "$OUT"
cname="plasma-x86-rootfs-$$"
docker rm -f "$cname" >/dev/null 2>&1 || true
echo ">> instalando pacotes (amd64, Debian 13)"
docker run --platform linux/amd64 --name "$cname" "$IMAGE" bash -euc "
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq
  apt-get install -y -qq --no-install-recommends ${PKGS[*]} >/dev/null
  apt-get clean
  rm -rf /var/lib/apt/lists/*
"

echo ">> exportando"
rm -rf "$STAGE" && mkdir -p "$STAGE"
docker export "$cname" | tar -C "$STAGE" -xf - 2>/dev/null || true
docker rm "$cname" >/dev/null

echo ">> limpando (docs, man, locales que não usamos)"
( cd "$STAGE"
  rm -rf usr/share/doc/* usr/share/man/* usr/share/info/* usr/share/lintian \
         var/cache/debconf/*-old var/log/* .dockerenv
  find usr/share/locale -mindepth 1 -maxdepth 1 ! -name 'locale.alias' -exec rm -rf {} + 2>/dev/null || true
)
rm -rf "$STAGE/proc" "$STAGE/sys"
mkdir -p "$STAGE/tmp" "$STAGE/var/tmp" "$STAGE/dev/shm" "$STAGE/run/pressure-vessel"
chmod 1777 "$STAGE/tmp" "$STAGE/var/tmp" "$STAGE/dev/shm"

echo ">> symlinks absolutos -> relativos"
python3 - "$STAGE" <<'PY'
import os, sys
root = os.path.realpath(sys.argv[1])
for dirpath, dirnames, filenames in os.walk(root):
    for name in dirnames + filenames:
        p = os.path.join(dirpath, name)
        if not os.path.islink(p):
            continue
        target = os.readlink(p)
        if not target.startswith('/'):
            continue
        rel = os.path.relpath(root + target, dirpath)
        os.unlink(p)
        os.symlink(rel, p)
PY

grep -q 'Debian' "$STAGE/etc/os-release" || { echo "ERRO: rootfs sem etc/os-release Debian" >&2; exit 1; }
[ -x "$STAGE/usr/bin/dpkg" ] || { echo "ERRO: dpkg ausente" >&2; exit 1; }

echo ">> empacotando (reproduzível, sem hard links)"
tar -C "$STAGE" --numeric-owner --owner=0 --group=0 --sort=name --hard-dereference -cf - . \
  | gzip -9 > "$OUT/debian-x86-rootfs.tar.gz"
rm -rf "$STAGE"
( cd "$OUT" && sha256sum debian-x86-rootfs.tar.gz > debian-x86-rootfs.tar.gz.sha256 )
du -h "$OUT/debian-x86-rootfs.tar.gz"
cat "$OUT/debian-x86-rootfs.tar.gz.sha256"
