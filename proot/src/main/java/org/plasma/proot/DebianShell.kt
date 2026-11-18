
package org.plasma.proot

import java.io.File
import java.io.OutputStream
import java.util.Base64

class ShellOutputParser(private val onLine: (String) -> Unit, private val onDone: (cwd: String, code: Int) -> Unit) {
    private val cur = StringBuilder()
    private val osc = StringBuilder()
    private var pendingCr = false
    private var state = NORMAL

    @Synchronized fun feed(s: CharSequence) {
        for (ch in s) {
            when (state) {
                NORMAL -> when (ch) {
                    '\u001b' -> state = ESC
                    '\r' -> pendingCr = true
                    '\n' -> { pendingCr = false; flushLine() }
                    '\u0007' -> {}
                    else -> put(ch)
                }
                ESC -> when (ch) {
                    ']' -> { osc.setLength(0); state = OSC }
                    'P', 'X', '^', '_' -> state = STR
                    '[' -> { if (pendingCr) { cur.setLength(0); pendingCr = false }; cur.append('\u001b').append('['); state = CSI }
                    '(', ')', '*', '+', '#', '%' -> state = SKIP1
                    '\u001b' -> {}
                    else -> state = NORMAL
                }
                SKIP1 -> state = NORMAL
                CSI -> { cur.append(ch); if (ch in '@'..'~') state = NORMAL }
                OSC -> when (ch) {
                    '\u0007' -> finishOsc()
                    '\u001b' -> state = OSC_ESC
                    else -> if (osc.length < MAX_OSC) osc.append(ch)
                }
                OSC_ESC -> when (ch) {
                    '\\' -> finishOsc()
                    ']' -> { osc.setLength(0); state = OSC }
                    else -> state = OSC
                }
                STR -> when (ch) { '\u0007' -> state = NORMAL; '\u001b' -> state = STR_ESC }
                STR_ESC -> state = if (ch == '\\') NORMAL else STR
            }
        }
    }

    private fun put(ch: Char) {
        if (pendingCr) { cur.setLength(0); pendingCr = false }
        cur.append(ch)
    }

    private fun finishOsc() {
        state = NORMAL
        val t = osc.toString(); osc.setLength(0)
        if (!t.startsWith(PREFIX)) return
        val p = t.substring(PREFIX.length)
        val i = p.lastIndexOf(';')
        if (i > 0) onDone(p.substring(0, i), p.substring(i + 1).trim().toIntOrNull() ?: 0)
    }

    private fun flushLine() { val l = cur.toString(); cur.setLength(0); onLine(l) }

    @Synchronized fun partial(): String = cur.toString()

    @Synchronized fun echoLine(text: String) { pendingCr = false; cur.append(text); flushLine() }

    private companion object {
        const val PREFIX = "777;plasma;"
        const val MAX_OSC = 8192
        const val NORMAL = 0; const val ESC = 1; const val CSI = 2; const val OSC = 3; const val OSC_ESC = 4
        const val STR = 5; const val STR_ESC = 6; const val SKIP1 = 7
    }
}

class DebianShell(private val paths: ProotPaths, private val onLine: (String) -> Unit, private val onDone: (String, Int) -> Unit, private val onExit: () -> Unit, private val onPartial: (String) -> Unit = {}) {
    @Volatile private var proc: Process? = null
    private var out: OutputStream? = null
    @Volatile var closed = false

    private val lock = Any()
    private var ready = false
    private val queue = ArrayList<String>()

    private val parser = ShellOutputParser(onLine) { cwd, code ->
        if (!ready) markReady() else onDone(cwd, code)
    }

    private fun markReady() {
        synchronized(lock) {
            if (ready) return
            queue.forEach { raw(it) }; queue.clear()
            ready = true
        }
    }

    fun start(startDir: String = "/root", cols: Int = 120, rows: Int = 36, extraEnv: Map<String, String> = emptyMap(), binds: List<Pair<File, String>> = emptyList()) {
        ProotLauncher.prepare(paths)

        val inner = "stty -echo cols $cols rows $rows 2>/dev/null; export PS1= PS2= PS4= PROMPT_COMMAND=; " +

            "export XDG_RUNTIME_DIR=\"\${XDG_RUNTIME_DIR:-/tmp/runtime-root}\"; mkdir -p \"\$XDG_RUNTIME_DIR\" 2>/dev/null; chmod 0700 \"\$XDG_RUNTIME_DIR\" 2>/dev/null; " +

            "if [ -x /usr/local/bin/plasma-gl ]; then eval \"\$(/usr/local/bin/plasma-gl env --no-probe 2>/dev/null)\"; fi; " +
            "printf '\\033]777;plasma;%s;0\\007' \"\$PWD\"; exec bash --noprofile --norc --noediting -s"
        val args = if (File(paths.rootfs, "usr/bin/script").exists()) listOf("/usr/bin/script", "-qfc", inner, "/dev/null")
                   else listOf("/bin/bash", "-c", inner)
        val cmd = ProotLauncher.command(paths, paths.rootfs, args, binds = binds, guestEnv = extraEnv + mapOf("COLUMNS" to "$cols", "LINES" to "$rows"), workdir = startDir)
        val pb = ProcessBuilder(cmd).directory(paths.root).redirectErrorStream(true)
        pb.environment().apply { clear(); putAll(ProotLauncher.hostEnv(paths)) }
        val p = pb.start(); proc = p; out = p.outputStream
        Thread({
            val r = p.inputStream.reader(Charsets.UTF_8); val buf = CharArray(4096)
            try { while (true) { val n = r.read(buf); if (n < 0) break; parser.feed(java.nio.CharBuffer.wrap(buf, 0, n)); onPartial(parser.partial()) } } catch (_: Exception) {}
            if (!closed) onExit()
        }, "debian-shell").apply { isDaemon = true }.start()

        Thread({ try { Thread.sleep(10_000) } catch (_: InterruptedException) {}; if (!closed) markReady() }, "debian-shell-ready").apply { isDaemon = true }.start()
    }

    private fun raw(s: String) { try { out?.write(s.toByteArray()); out?.flush() } catch (_: Exception) {} }
    private fun send(s: String) { synchronized(lock) { if (!ready) { queue += s; return } }; raw(s) }

    fun run(cmd: String) {
        val b64 = Base64.getEncoder().encodeToString(cmd.toByteArray(Charsets.UTF_8))
        send(". /usr/local/lib/plasma-x11-env.sh 2>/dev/null && __plasma_dpy; eval \"\$(printf %s $b64 | base64 -d)\"; printf '\\033]777;plasma;%s;%d\\007' \"\$PWD\" \$?\n")
    }

    fun sendInput(text: String, echo: Boolean = true) { send(text + "\n"); parser.echoLine(if (echo) text else "") }
    fun sendEof() = send("\u0004")
    fun interrupt() = raw("\u0003")
    fun close() { closed = true; runCatching { out?.close() }; proc?.destroyForcibly() }
}
