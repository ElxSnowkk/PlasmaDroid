// Parte do PlasmaDroid — módulo PlasmaVK.
// Créditos: baseado no projeto Proot.

package org.plasma.proot

import java.io.File

object DebianGuest {
    private const val APT_CONF = "APT::Sandbox::User \"root\";\nAPT::Sandbox::Seccomp \"false\";\nAPT::Install-Recommends \"false\";\n" +
        "Acquire::Languages \"none\";\nDpkg::Use-Pty \"0\";\nDPkg::Options:: \"--force-unsafe-io\";\n"

    fun prepareBare(root: File) {
        for (d in listOf("dev", "proc", "sys", "tmp", "root", "etc")) File(root, d).mkdirs()
        File(root, "tmp").setWritable(true, false)
        ensureResolvConf(root)
    }

    fun apply(rootfs: File) {
        runCatching { ensureResolvConf(rootfs) }
        runCatching { configureApt(rootfs) }
        runCatching { syncAndroidGroups(rootfs) }
        runCatching { configureShellStartup(rootfs) }
        runCatching { writeFastfetchConfig(rootfs) }
        runCatching { GuestPatches.install(rootfs) }
    }

    fun ensureResolvConf(rootfs: File) {
        val etc = File(rootfs, "etc").apply { mkdirs() }
        val f = File(etc, "resolv.conf")
        val broken = java.nio.file.Files.isSymbolicLink(f.toPath()) || !f.isFile ||
            f.readLines().none { l -> l.trim().startsWith("nameserver") && !l.contains("127.0.") && !l.contains("::1") }
        if (broken) { java.nio.file.Files.deleteIfExists(f.toPath()); f.writeText("nameserver 1.1.1.1\nnameserver 8.8.8.8\n") }
    }

    private fun configureApt(rootfs: File) {
        val dir = File(rootfs, "etc/apt/apt.conf.d").apply { mkdirs() }
        val conf = File(dir, "99plasma")
        if (!conf.isFile || conf.readText() != APT_CONF) conf.writeText(APT_CONF)
        val policy = File(rootfs, "usr/sbin/policy-rc.d")
        if (File(rootfs, "usr/sbin").isDirectory && !policy.exists()) { policy.writeText("#!/bin/sh\nexit 101\n"); policy.setExecutable(true, false) }
    }

    fun syncAndroidGroups(rootfs: File) {
        val gids = LinkedHashSet<Int>()
        runCatching {
            File("/proc/self/status").readLines().firstOrNull { it.startsWith("Groups:") }
                ?.substringAfter(':')?.trim()?.split(Regex("\\s+"))?.mapNotNull { it.toIntOrNull() }?.let { gids += it }
        }
        gids += android.os.Process.myUid()
        val group = File(File(rootfs, "etc").apply { mkdirs() }, "group")
        val text = if (group.isFile) group.readText() else "root:x:0:\n"
        val known = text.lineSequence().mapNotNull { it.split(':').getOrNull(2)?.toIntOrNull() }.toHashSet()
        val names = text.lineSequence().map { it.substringBefore(':') }.filter { it.isNotEmpty() }.toHashSet()
        val add = StringBuilder()
        for (gid in gids) {
            if (gid <= 0 || gid in known) continue
            var n = "aid_$gid"; if (n in names) n = "aid_${gid}_"
            names += n; known += gid; add.append("$n:x:$gid:\n")
        }
        if (add.isEmpty()) return
        group.setWritable(true, true)
        group.writeText(text.let { if (it.endsWith("\n")) it else it + "\n" } + add)
        group.setReadable(true, false)
    }

    fun writeSources(rootfs: File, suite: String) {
        val d = File(rootfs, "etc/apt/sources.list.d").apply { mkdirs() }
        File(d, "debian.sources").writeText(
            """Types: deb
URIs: https://deb.debian.org/debian
Suites: $suite $suite-updates
Components: main contrib non-free non-free-firmware
Signed-By: /usr/share/keyrings/debian-archive-keyring.gpg

Types: deb
URIs: https://security.debian.org/debian-security
Suites: $suite-security
Components: main contrib non-free non-free-firmware
Signed-By: /usr/share/keyrings/debian-archive-keyring.gpg
""")
        File(rootfs, "etc/apt/sources.list").let { if (it.exists()) it.writeText("# Veja /etc/apt/sources.list.d/debian.sources\n") }
    }

    fun writeGpuInfo(rootfs: File, gpu: String, plasmavkStatus: String) {
        val d = File(rootfs, "etc/plasmadroid").apply { mkdirs() }
        File(d, "gpu").writeText(gpu.trim().ifEmpty { "desconhecida" } + "\n")
        File(d, "plasmavk").writeText(plasmavkStatus + "\n")
        File(d, "gpu").setReadable(true, false); File(d, "plasmavk").setReadable(true, false)
    }

    private const val VKVERSION_SH = """#!/bin/sh
command -v vulkaninfo >/dev/null 2>&1 || { echo "vulkan-tools não instalado"; exit 0; }
out=${'$'}(timeout 15 vulkaninfo --summary 2>/dev/null)
case "${'$'}out" in *apiVersion*) ;; *) out=${'$'}(timeout 20 vulkaninfo 2>/dev/null | head -600) ;; esac
res=${'$'}(printf '%s\n' "${'$'}out" | awk '
/^[ \t]*GPU[0-9]+:/ || /GPU id[ \t]*:/ { n++; next }
n > 0 && /apiVersion[ \t]*=/ { if (match(${'$'}0, /[0-9]+\.[0-9]+\.[0-9]+/)) v[n] = substr(${'$'}0, RSTART, RLENGTH) }
n > 0 && /deviceType[ \t]*=/ { t[n] = ${'$'}0 }
n > 0 && /deviceName[ \t]*=/ { x = ${'$'}0; sub(/.*=[ \t]*/, "", x); nm[n] = x }
END {
  p = 0
  for (i = 1; i <= n; i++) if (v[i] != "" && t[i] !~ /CPU/) { p = i; break }
  if (p) { print v[p]; exit }
  for (i = 1; i <= n; i++) if (v[i] != "") { print v[i] " (software: " nm[i] ")"; exit }
}')
if [ -n "${'$'}res" ]; then echo "${'$'}res"; else echo "indisponível (vulkaninfo não enumerou nenhum dispositivo)"; fi
"""

    private const val X11_LAUNCHER_SH = """#!/bin/sh
# plasma-x11 [--background] [:N]   — inicia o Xwayland (rootful) dentro do compositor Wayland do PlasmaDroid.
BG=0; D=:0
for a in "§@"; do case "§a" in --background) BG=1;; --child) ;; :*) D="§a";; esac; done
export XDG_RUNTIME_DIR="§{XDG_RUNTIME_DIR:-/run/plasma-wayland}"
export WAYLAND_DISPLAY="§{WAYLAND_DISPLAY:-wayland-0}"
N="§{D#:}"
if ! command -v Xwayland >/dev/null 2>&1; then
  echo "[X11] Xwayland não está instalado. Rode: apt-get install -y xwayland x11-xserver-utils openbox"
  exit 1
fi
if [ -S "/tmp/.X11-unix/X§N" ] && [ -f "/tmp/.X§N-lock" ] && kill -0 "§(cat /tmp/.X§N-lock 2>/dev/null)" 2>/dev/null; then
  echo "[X11] já ativo em §D"; exit 0
fi
if [ "§BG" = 1 ] && [ -z "§PLASMA_X11_CHILD" ]; then
  PLASMA_X11_CHILD=1 nohup "§0" --child "§D" >/tmp/plasma-x11.log 2>&1 &
  exit 0
fi
rm -f "/tmp/.X§N-lock" "/tmp/.X11-unix/X§N"
mkdir -p /tmp/.X11-unix && chmod 1777 /tmp/.X11-unix
export XWAYLAND_NO_GLAMOR="§{XWAYLAND_NO_GLAMOR:-1}"
GEOM="§{PLASMA_X11_GEOMETRY:-1280x720}"
Xwayland "§D" -rootful -geometry "§GEOM" -noreset -nolisten tcp >/tmp/xwayland.log 2>&1 &
XPID=§!
i=0
while [ §i -lt 100 ]; do
  [ -S "/tmp/.X11-unix/X§N" ] && break
  kill -0 §XPID 2>/dev/null || { echo "[X11] o Xwayland encerrou; veja /tmp/xwayland.log"; exit 1; }
  sleep 0.1; i=§((i+1))
done
export DISPLAY="§D"
for wm in openbox xfwm4 icewm fluxbox twm; do
  if command -v §wm >/dev/null 2>&1; then §wm >/dev/null 2>&1 & break; fi
done
echo "[X11] pronto em §D (Xwayland rootful, §GEOM)"
wait §XPID
"""

    fun writeX11Launcher(rootfs: File) {
        val bin = File(rootfs, "usr/local/bin").apply { mkdirs() }
        val f = File(bin, "plasma-x11")
        val text = X11_LAUNCHER_SH.replace('§', '$')
        if (!f.isFile || f.readText() != text) f.writeText(text)
        f.setReadable(true, false); f.setExecutable(true, false)
    }

    fun writeFastfetchConfig(rootfs: File) {
        File(rootfs, "usr/local/bin").apply { mkdirs() }.let { bin ->
            val f = File(bin, "plasmadroid-vkversion")
            f.writeText(VKVERSION_SH)
            f.setReadable(true, false); f.setExecutable(true, false)
        }
        val d = File(rootfs, "root/.config/fastfetch").apply { mkdirs() }
        File(d, "config.jsonc").writeText(
            """{
  "${'$'}schema": "https://github.com/fastfetch-cli/fastfetch/raw/dev/doc/json_schema.json",
  "modules": [
    "title", "separator", "os", "host", "kernel", "uptime", "packages", "shell", "terminal", "cpu",
    { "type": "command", "key": "GPU", "text": "cat /etc/plasmadroid/gpu" },
    { "type": "command", "key": "OpenGL", "text": "/usr/local/bin/plasma-gl info 2>/dev/null || echo indisponível" },
    { "type": "command", "key": "Vulkan", "text": "/usr/local/bin/plasmadroid-vkversion" },
    "memory", "swap", "disk", "localip", "locale", "break", "colors"
  ]
}
""")
    }

    private fun configureShellStartup(rootfs: File) {
        val rc = File(rootfs, "root/.bashrc")
        val mark = "# plasmadroid"
        val old = if (rc.isFile) rc.readText() else ""
        if (!old.contains(mark)) {
            rc.parentFile?.mkdirs()
            rc.writeText(old + "\n$mark\nexport LANG=C.UTF-8\nalias ls='ls --color=auto'\nalias grep='grep --color=auto'\nPS1='\\[\\e[1;32m\\]\\u@\\h\\[\\e[0m\\]:\\[\\e[1;34m\\]\\w\\[\\e[0m\\]\\$ '\n")
        }
        val hn = File(rootfs, "etc/hostname"); if (!hn.exists() || hn.readText().isBlank()) hn.writeText("plasmadroid\n")
    }
}
