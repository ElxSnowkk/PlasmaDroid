
package org.plasma.proot

import java.io.File

object DebianGuest {
    private const val APT_CONF = "APT::Sandbox::User \"root\";\nAPT::Sandbox::Seccomp \"false\";\nAPT::Install-Recommends \"false\";\n" +
        "Acquire::Languages \"none\";\nDpkg::Use-Pty \"0\";\nDPkg::Options:: \"--force-unsafe-io\";\n"

    fun prepareBare(root: File) {
        for (d in listOf("dev", "proc", "sys", "tmp", "root", "etc")) File(root, d).mkdirs()
        File(root, "tmp").setWritable(true, false)
        ensureResolvConf(root)
        runCatching { ensureInitStub(root) }
    }

    fun apply(rootfs: File, deviceDns: List<String> = emptyList()) {
        runCatching { ensureResolvConf(rootfs, deviceDns) }
        runCatching { ensureInitStub(rootfs) }
        runCatching { configureApt(rootfs) }
        runCatching { syncAndroidGroups(rootfs) }
        runCatching { configureShellStartup(rootfs) }
        runCatching { writeFastfetchConfig(rootfs) }
        runCatching { GuestPatches.install(rootfs) }
    }

    fun ensureResolvConf(rootfs: File, deviceDns: List<String> = emptyList()) {
        val etc = File(rootfs, "etc").apply { mkdirs() }
        val f = File(etc, "resolv.conf")
        val servers = (deviceDns.take(2) + HostDns.PUBLIC).distinct().take(3)
        val text = buildString {
            servers.forEach { append("nameserver ").append(it).append('\n') }
            append("options timeout:2 attempts:3\n")
        }
        val isLink = java.nio.file.Files.isSymbolicLink(f.toPath())
        if (isLink || !f.isFile || f.readText() != text) {
            java.nio.file.Files.deleteIfExists(f.toPath())
            f.writeText(text)
        }
    }

    fun ensureInitStub(rootfs: File) {
        val sbin = File(rootfs, "usr/sbin").apply { mkdirs() }
        val init = File(sbin, "init")
        val p = init.toPath()
        val exists = java.nio.file.Files.exists(p)
        if (exists) return
        java.nio.file.Files.deleteIfExists(p)
        java.nio.file.Files.createSymbolicLink(p, java.nio.file.Paths.get("/usr/bin/bash"))
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
# plasma-x11 [start [new|:N]|stop [:N]|--status|--list|run [--new|:N] <app...>]
#
# Comando do servidor X11 NATIVO do PlasmaDroid. O servidor roda no app Android (libplasma_x11.so);
# este comando só manda pedidos para o app (/tmp/plasma-x11.request) e lê a resposta (/tmp/plasma-x11.reply).
#   start        garante que exista um display (cria :0; se já houver um nosso, reaproveita)
#   start new    cria SEMPRE um display novo (:1, :2...) = uma nova janela/sessão X11
#   start :N     exige o display :N (falha se estiver ocupado por outro processo)
#   run [--new] <app>   roda o app no display certo (com --new, num display novo)
# Se o :0 estiver ocupado por outro servidor, o PlasmaDroid pula para o próximo livre sozinho.
REQ=/tmp/plasma-x11.request
REPLY=/tmp/plasma-x11.reply
CUR=/tmp/plasma-x11.display
STATUS=/tmp/plasma-x11.status

sock() { echo "/tmp/.X11-unix/X§{1#:}"; }

# ask <pedido...>  -> imprime ":N" no stdout; erros vão para o stderr
ask() {
  rm -f "§REPLY"
  echo "§*" > "§REQ"
  i=0
  while [ ! -s "§REPLY" ] && [ "§i" -lt 75 ]; do sleep 0.2; i=§((i + 1)); done
  if [ ! -s "§REPLY" ]; then
    echo "[X11] o PlasmaDroid não respondeu (o desktop está aberto?). Veja: plasma-x11 --status" >&2
    return 1
  fi
  r=§(cat "§REPLY")
  case "§r" in
    :[0-9]*) echo "§r"; return 0 ;;
  esac
  echo "[X11] §r" >&2
  return 1
}

valid_display() {
  case "§1" in :[0-9]*) n="§{1#:}"; n="§{n%%.*}"; case "§n" in ''|*[!0-9]*) return 1 ;; esac; return 0 ;; esac
  return 1
}

case "§1" in
  --status)
    if [ -r "§CUR" ]; then
      d=§(cat "§CUR")
      if [ -S "§(sock "§d")" ]; then
        echo "[X11] ativo em §d (servidor nativo do PlasmaDroid)"
        [ -r "§STATUS" ] && echo "[X11] §(cat "§STATUS")"
        exit 0
      fi
    fi
    echo "[X11] inativo: rode plasma-x11 start"
    [ -r "§STATUS" ] && echo "[X11] último estado do app: §(cat "§STATUS")"
    exit 1 ;;
  --list)
    for s in /tmp/.X11-unix/X*; do [ -S "§s" ] && echo ":§{s##*/X}"; done
    exit 0 ;;
  stop)
    ask stop §2 >/dev/null 2>&1
    echo "[X11] parado"; exit 0 ;;
  start|"")
    shift 2>/dev/null
    d=§(ask start §@) || exit 1
    echo "[X11] ativo em §d"
    [ "§1" = new ] && echo "[X11] use: plasma-x11 run §d <app>   (ou DISPLAY=§d <app>)"
    exit 0 ;;
  run)
    shift
    req="start"
    case "§1" in
      --new|new) req="start new"; shift ;;
      :[0-9]*) if valid_display "§1"; then req="start §1"; shift; fi ;;
    esac
    [ -n "§PLASMA_X11_DISPLAY" ] && [ "§req" = start ] && valid_display "§PLASMA_X11_DISPLAY" && req="start §PLASMA_X11_DISPLAY"
    d=§(ask §req) || exit 1
    export DISPLAY="§d" GDK_BACKEND=x11 QT_QPA_PLATFORM=xcb SDL_VIDEODRIVER=x11 MOZ_ENABLE_WAYLAND=0 XDG_SESSION_TYPE=x11
    [ "§#" -gt 0 ] && exec "§@"
    echo "§d"
    exit 0 ;;
  *)
    echo "uso: plasma-x11 [start [new|:N]|stop [:N]|--status|--list|run [--new|:N] <app...>]"; exit 2 ;;
esac
"""

    private const val X11_ENV_SH = """# PlasmaDroid: mantém DISPLAY válido (o servidor X11 pode subir depois do shell, ou mudar de :0 para :1)
__plasma_dpy() {
  _pf=/tmp/plasma-x11.display
  [ -r "§_pf" ] || return 0
  _pd=§(cat "§_pf" 2>/dev/null)
  _pn="§{_pd#:}"
  case "§_pn" in ''|*[!0-9]*) return 0 ;; esac
  [ -S "/tmp/.X11-unix/X§_pn" ] || return 0
  case "§{DISPLAY:-}" in
    '') export DISPLAY="§_pd" ;;
    :[0-9]*)
      _pc="§{DISPLAY#:}"; _pc="§{_pc%%.*}"
      [ -S "/tmp/.X11-unix/X§_pc" ] || export DISPLAY="§_pd" ;;
  esac
  return 0
}
"""

    fun writeX11Launcher(rootfs: File) {
        val bin = File(rootfs, "usr/local/bin").apply { mkdirs() }
        val f = File(bin, "plasma-x11")
        val text = X11_LAUNCHER_SH.replace('§', '$')
        if (!f.isFile || f.readText() != text) f.writeText(text)
        f.setReadable(true, false); f.setExecutable(true, false)

        val lib = File(rootfs, "usr/local/lib").apply { mkdirs() }
        val env = File(lib, "plasma-x11-env.sh")
        val envText = X11_ENV_SH.replace('§', '$')
        if (!env.isFile || env.readText() != envText) env.writeText(envText)
        env.setReadable(true, false)
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
