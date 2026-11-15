/*
 * This file belongs to the PlasmaDroid project.
 *
 * Copyright (C) 2026 ElxSnowkk (https://github.com/ElxSnowkk)
 *
 * PlasmaDroid is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later version.
 *
 * Licensed under GPL-3.0-or-later. See the LICENSE file at the project root,
 * or <https://www.gnu.org/licenses/gpl-3.0.html>.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
 * PARTICULAR PURPOSE. See the GNU General Public License for more details.
 */

package org.plasma.app.session

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import org.plasma.app.PlasmaApp
import org.plasma.app.ui.gpuDescription
import org.plasma.core.Keys
import org.plasma.core.PlasmaLog
import org.plasma.proot.DebianGuest
import org.plasma.proot.GuestPatches
import org.plasma.proot.DebianShell
import org.plasma.proot.PlasmaVKSupport

class TerminalSession(private val app: PlasmaApp, private val session: DesktopSession) {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private var shell: DebianShell? = null

    var lines by mutableStateOf<List<String>>(emptyList())
        private set
    var revision by mutableIntStateOf(0)
        private set
    private val buf = ArrayList<String>()
    private val bufLock = Any()
    private val flushScheduled = java.util.concurrent.atomic.AtomicBoolean(false)
    var input by mutableStateOf("")
    var busy by mutableStateOf(false)
    var closed by mutableStateOf(false)

    var partial by mutableStateOf("")
        private set
    @Volatile private var partialRaw = ""
    private val secretRe = Regex("(senha|password|passphrase|passwort|contrase|\\[sudo\\])", RegexOption.IGNORE_CASE)
    var cwdPath by mutableStateOf("~")
        private set

    val user: String = "root"
    val host: String = "plasmadroid"
    private var cwd = "/root"
    private val history = ArrayList<String>()
    private var histIdx = 0
    private var pendingFetch = app.settings.bool(Keys.TERM_FASTFETCH, true)

    init { scope.launch { startShell() } }

    fun promptAnsi(): String = "$ESC[1;32m$user@$host$ESC[0m:$ESC[1;34m${shortCwd()}$ESC[0m# "
    val tabTitle: String get() = "$user@$host:${shortCwd()}"
    private fun shortCwd() = if (cwd == "/root") "~" else if (cwd.startsWith("/root/")) "~" + cwd.removePrefix("/root") else cwd

    private var x11Mode = false

    private fun startShell() {
        val deb = app.debian
        if (!deb.installed) {
            print("$ESC[1;33mO Debian ainda não está instalado.$ESC[0m Abrindo a janela de configuração inicial…")
            print("Digite $ESC[1msetup$ESC[0m para reabrir o assistente.")
            session.openTool("setup"); return
        }
        runCatching { DebianGuest.writeX11Launcher(deb.paths.rootfs) }
        runCatching { GuestPatches.install(deb.paths.rootfs) }
        runCatching { DebianGuest.writeFastfetchConfig(deb.paths.rootfs) }
        runCatching { DebianGuest.writeGpuInfo(deb.paths.rootfs, gpuDescription(session), PlasmaVKSupport.status(deb.paths.rootfs)) }
        val sh = DebianShell(deb.paths, onLine = { print(it) }, onDone = { dir, code ->
            cwd = dir; cwdPath = shortCwd()
            if (code != 0 && code != 130) print("$ESC[2m[código de saída $code]$ESC[0m")
            partialRaw = ""; busy = false
        }, onPartial = { if (it != partialRaw) { partialRaw = it; scheduleFlush() } },
        onExit = { partialRaw = ""; print("$ESC[2m[shell encerrado — pressione Enter para reiniciar]$ESC[0m"); shell = null; busy = false })
        try {

            runCatching { app.wayland.ensureStarted(deb.paths) }
            val x11 = app.settings.string(Keys.GFX_SERVER, "wayland") == "x11"
            x11Mode = x11
            val env = runCatching { app.wayland.clientEnvironment(x11) }.getOrDefault(emptyMap())
            val binds = runCatching { app.wayland.clientBinds(deb.paths) }.getOrDefault(emptyList())
            sh.start(cwd, extraEnv = env, binds = binds); shell = sh
        }
        catch (t: Throwable) { PlasmaLog.w("term", "falha ao iniciar o shell Debian", t); print("Falha ao iniciar o Debian: ${t.message}"); return }
        if (x11Mode) {

            sh.run("plasma-x11 --background")
        }
        if (pendingFetch) { pendingFetch = false; busy = true; sh.run("command -v fastfetch >/dev/null && fastfetch") }
    }

    private fun print(s: String) {
        synchronized(bufLock) {
            s.split('\n').forEach { buf += if (it.length > MAX_LINE) it.take(MAX_LINE) + "…" else it }
            if (buf.size > MAX_LINES * 2) buf.subList(0, buf.size - MAX_LINES).clear()
        }
        scheduleFlush()
    }
    private fun scheduleFlush() {
        if (!flushScheduled.compareAndSet(false, true)) return
        scope.launch { delay(if (busy) 66 else 33); flushScheduled.set(false); publish() }
    }
    private fun publish() {
        val snap = synchronized(bufLock) { buf.subList((buf.size - MAX_LINES).coerceAtLeast(0), buf.size).toList() }
        lines = snap; partial = if (busy) partialRaw else ""; revision++
    }
    fun clear() { synchronized(bufLock) { buf.clear() }; lines = emptyList(); revision++ }

    fun submit() {

        val sh0 = shell
        if (busy && sh0 != null) {
            val answer = input; input = ""
            sh0.sendInput(answer, echo = !secretRe.containsMatchIn(Ansi.strip(partialRaw)))
            return
        }
        val cmd = input.trim()
        print(promptAnsi() + input)
        input = ""
        if (cmd.isEmpty()) { if (shell == null && app.debian.installed) scope.launch { startShell() }; return }
        if (history.lastOrNull() != cmd) history += cmd
        histIdx = history.size
        when (cmd) {
            "clear" -> { clear(); return }
            "setup" -> { session.openTool("setup"); return }
            "exit", "logout" -> { session.closeWindowOf(this); return }
            "help" -> { print(HELP); return }
        }
        val sh = shell ?: run { scope.launch { startShell(); shell?.let { s -> busy = true; s.run(cmd) } }; return }
        busy = true
        sh.run(cmd)
    }

    fun historyPrev() { if (history.isNotEmpty()) { histIdx = (histIdx - 1).coerceAtLeast(0); input = history[histIdx] } }
    fun historyNext() {
        if (history.isEmpty()) return
        histIdx = (histIdx + 1).coerceAtMost(history.size)
        input = if (histIdx >= history.size) "" else history[histIdx]
    }

    fun eof() { if (busy) shell?.sendEof() }

    fun interrupt() {
        if (busy) { shell?.interrupt(); print("^C") } else { print(promptAnsi() + input + "^C"); input = "" }
    }

    fun close() { closed = true; shell?.close(); scope.cancel() }

    fun complete() {
        if (busy || shell == null) return
        val idx = input.lastIndexOf(' ')
        val tok = input.substring(idx + 1)
        if (tok.isEmpty()) return
        val head = input.substring(0, idx + 1)
        val comp = if (idx < 0) "compgen -c -- " else "compgen -f -- "
        val sb = ArrayList<String>()
        scope.launch {
            val r = org.plasma.app.session.Completion.run(app, cwd, comp + "'" + tok.replace("'", "'\\''") + "'")
            sb += r
            if (sb.isEmpty()) return@launch
            if (sb.size == 1) input = head + sb[0] + " "
            else { var pre = sb[0]; for (x in sb) while (!x.startsWith(pre)) pre = pre.dropLast(1)
                if (pre.length > tok.length) input = head + pre else print(sb.joinToString("  ")) }
        }
    }

    companion object {
        private const val ESC = "\u001b"
        private const val MAX_LINES = 1200
        private const val MAX_LINE = 4000
        const val HELP = "Konsole (Plasma Android) — shell Debian real (bash via PRoot).\n" +
            "Comandos locais: clear, exit, help, setup (reabre a configuração do Debian).\n" +
            "Tudo mais roda no Debian: apt, fastfetch, vulkaninfo, nano… /sdcard é o armazenamento do Android.\n" +
            "Atalhos: Tab completa · setas: histórico · Ctrl+C interrompe · Ctrl+L limpa."
    }
}

object Completion {
    fun run(app: PlasmaApp, cwd: String, script: String): List<String> {
        val d = app.debian
        val cmd = org.plasma.proot.ProotLauncher.command(d.paths, d.paths.rootfs, listOf("/bin/bash", "-c", script), workdir = cwd)
        val pb = ProcessBuilder(cmd).directory(d.paths.root).redirectErrorStream(true)
        pb.environment().apply { clear(); putAll(org.plasma.proot.ProotLauncher.hostEnv(d.paths)) }
        return runCatching {
            val p = pb.start(); runCatching { p.outputStream.close() }
            val out = p.inputStream.bufferedReader().readLines().filter { it.isNotBlank() }.distinct().sorted().take(200)
            p.waitFor(5, java.util.concurrent.TimeUnit.SECONDS); p.destroyForcibly(); out
        }.getOrDefault(emptyList())
    }
}
