
package org.plasma.proot

import android.content.Context
import android.system.Os
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import org.plasma.core.PlasmaLog
import java.io.File

enum class StepState { PENDING, RUNNING, DONE, FAILED }
data class SetupStep(val id: String, val label: String, val state: StepState = StepState.PENDING, val detail: String = "", val background: Boolean = false)
data class SetupUi(
    val steps: List<SetupStep>,
    val log: List<String> = emptyList(),
    val running: Boolean = false,
    val complete: Boolean = false,
    val error: String? = null,
    val fastfetch: String = "",
)

class DebianEnvironment(private val ctx: Context, val paths: ProotPaths) {
    companion object {
        const val S_ENV = "env"; const val S_PKGS = "pkgs"; const val S_SRC = "src"; const val S_KR = "plasmavk"; const val S_FETCH = "fetch"
        const val S_PIAPPS = "piapps"; const val S_FEX = "fex"; const val S_MEDIUM = "medium"
        private const val FEX_ASSET = "fex/fex-aarch64.tar.xz"
        private const val FEX_ROOTFS_ASSET = "fex/rootfs-debian-min.sqsh"
        private const val FASTFETCH_DEB = "https://github.com/fastfetch-cli/fastfetch/releases/latest/download/fastfetch-linux-aarch64.deb"
        val X11_PACKAGES = listOf("xauth", "x11-xkb-utils", "xkb-data",
            "x11-xserver-utils", "x11-utils", "openbox", "libgl1-mesa-dri", "libglx-mesa0", "xfonts-base", "fonts-dejavu-core")
        val BASE_PACKAGES = listOf("ca-certificates", "curl", "wget", "locales", "procps", "iproute2", "nano", "less",
            "sudo", "xz-utils", "libvulkan1", "vulkan-tools", "mesa-utils", "mesa-utils-bin", "mesa-utils-extra",
            "mesa-vulkan-drivers", "squashfs-tools", "binutils", "librsvg2-bin") + X11_PACKAGES
        private val CRITICAL_PACKAGES = setOf("ca-certificates", "curl", "wget", "locales", "procps", "sudo", "libvulkan1")
        val ULTRA_PACKAGES = CRITICAL_PACKAGES + setOf("xz-utils", "squashfs-tools")
    }

    private val lock = Mutex()
    private val bgLock = Mutex()
    private val bgScope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private fun initialSteps() = listOf(
        SetupStep(S_ENV, "Ambiente Debian"),
        SetupStep(S_PKGS, "Pacotes essenciais"),
        SetupStep(S_SRC, "Source lists"),
        SetupStep(S_KR, "Graphics Environment (Vulkan And PlasmaGL)"),
        SetupStep(S_MEDIUM, "Pacotes complementares (X11, Mesa, editor)", background = true),
        SetupStep(S_FETCH, "Fastfetch"),
        SetupStep(S_PIAPPS, "Pi-Apps (loja de apps)", background = true),
        SetupStep(S_FEX, "FEX (apps x86-64)", background = true),
    ).map { if (paths.stepMarker(it.id).exists()) it.copy(state = StepState.DONE) else it }

    private val _ui = MutableStateFlow(SetupUi(initialSteps(), complete = paths.setupDone.exists()))
    val ui: StateFlow<SetupUi> = _ui.asStateFlow()

    val installed: Boolean get() = paths.rootfs.exists() && paths.marker.exists()
    val ready: Boolean get() = installed && paths.setupDone.exists()

    private val logFile: File get() = File(paths.root, "setup.log")
    private fun log(l: String) {
        _ui.value = _ui.value.let { it.copy(log = (it.log + l).takeLast(400)) }
        runCatching { logFile.appendText(l + "\n") }
    }
    private fun step(id: String, st: StepState, detail: String = "") {
        _ui.value = _ui.value.copy(steps = _ui.value.steps.map { if (it.id == id) it.copy(state = st, detail = detail) else it })
        val marker = paths.stepMarker(id)
        if (st == StepState.DONE) marker.writeText("1\n") else if (st == StepState.FAILED || st == StepState.PENDING) marker.delete()
    }
    private fun detail(id: String, d: String) { _ui.value = _ui.value.copy(steps = _ui.value.steps.map { if (it.id == id) it.copy(detail = d) else it }) }

    fun refreshDns() {
        runCatching { HostDns.sync(ctx, paths.rootfs) }
            .onFailure { PlasmaLog.e("debian", "não atualizou o DNS do guest", it) }
    }

    fun exec(script: String, rootfs: File = paths.rootfs, timeoutMs: Long = 20 * 60_000L, noSeccomp: Boolean = false, quiet: Boolean = false, sink: ((String) -> Unit)? = null): Int {
        ProotLauncher.prepare(paths)
        if (rootfs == paths.rootfs) refreshDns()
        val cmd = ProotLauncher.command(paths, rootfs, listOf("/bin/bash", "-c", script),
            guestEnv = mapOf("DEBIAN_FRONTEND" to "noninteractive", "LC_ALL" to "C.UTF-8"), useGuestEnvWrapper = File(rootfs, "usr/bin/env").exists())
        val pb = ProcessBuilder(cmd).directory(paths.root).redirectErrorStream(true)
        pb.environment().apply { clear(); putAll(ProotLauncher.hostEnv(paths, noSeccomp)) }
        val p = pb.start(); runCatching { p.outputStream.close() }
        val t = Thread { try { p.inputStream.bufferedReader().forEachLine { l -> val c = l.substringAfterLast('\r'); sink?.invoke(c); if (!quiet) log(c) } } catch (_: java.io.IOException) {   } catch (e: Exception) { PlasmaLog.e("debian", "leitor de saída falhou", e) } }
        t.isDaemon = true; t.start()
        if (!p.waitFor(timeoutMs, java.util.concurrent.TimeUnit.MILLISECONDS)) { p.destroyForcibly(); t.join(1000); return -1 }
        t.join(2000); return p.exitValue()
    }

    private fun captured(script: String): String { val sb = StringBuilder(); exec(script, quiet = true, sink = { sb.appendLine(it) }); return sb.toString().trim() }

    suspend fun recheckVulkan() {
        if (_ui.value.running || !installed) return
        lock.withLock {
            withContext(Dispatchers.IO) {
                _ui.value = _ui.value.copy(running = true, error = null)
                try {
                    PlasmaVKSupport.invalidateProbe(paths.rootfs)
                    stepPlasmaVK()
                } catch (e: Exception) {
                    PlasmaLog.e("debian", "recheckVulkan falhou", e)
                    log("PlasmaVK: ${e.message ?: e.javaClass.simpleName}")
                } finally { _ui.value = _ui.value.copy(running = false) }
            }
        }
    }

    val COMPONENT_VERSIONS: Map<String, Int> = mapOf(
        S_ENV to 1, S_PKGS to 1, S_SRC to 1, S_KR to 1, S_MEDIUM to 1, S_PIAPPS to 2, S_FEX to 2, S_FETCH to 1,
    )
    private val manifest: File get() = File(paths.root, "components.json")
    @Volatile private var reconciling = false

    private fun readManifest(): Map<String, Int> = runCatching {
        val o = org.json.JSONObject(manifest.readText())
        o.keys().asSequence().associateWith { o.getInt(it) }
    }.getOrDefault(emptyMap())

    private fun writeManifest() {
        runCatching {
            val o = org.json.JSONObject()
            COMPONENT_VERSIONS.forEach { (id, v) -> o.put(id, v) }
            manifest.writeText(o.toString())
        }.onFailure { PlasmaLog.e("debian", "não gravou components.json", it) }
    }

    fun missingComponents(): List<String> {
        val saved = readManifest()
        return COMPONENT_VERSIONS.filter { (id, v) -> saved[id] != v || !paths.stepMarker(id).exists() }.keys.toList()
    }

    suspend fun reconcile() {
        if (!ready || reconciling || _ui.value.running) return
        val missing = missingComponents()
        if (missing.isEmpty()) return
        reconciling = true
        try {
            log("Componentes faltando, instalando de fundo: ${missing.joinToString()}")
            setup()
            if (_ui.value.error == null) writeManifest()
        } finally { reconciling = false }
    }

    suspend fun setup() = lock.withLock {
        withContext(Dispatchers.IO) {
            val bgAtivo = _ui.value.steps.filter { it.background && (it.state == StepState.RUNNING || it.state == StepState.PENDING) && bgLock.isLocked }
            _ui.value = _ui.value.copy(running = true, error = null, complete = false,
                steps = initialSteps().map { s -> bgAtivo.firstOrNull { it.id == s.id }?.let { s.copy(state = it.state, detail = it.detail) } ?: s })
            runCatching { if (logFile.length() > 1_000_000) logFile.delete() }
            try {
                if (!paths.stepMarker(S_ENV).exists()) stepEnv() else { refreshDns(); DebianGuest.apply(paths.rootfs, HostDns.deviceServers(ctx)) }
                if (!paths.stepMarker(S_PKGS).exists()) stepPackages()
                if (!paths.stepMarker(S_SRC).exists()) stepSources()
                stepFastfetch()
                stepPlasmaVK()
                paths.setupDone.writeText("done ${System.currentTimeMillis()}\n")
                _ui.value = _ui.value.copy(complete = true)
                iniciarSegundoPlano()
            } catch (e: Exception) {
                PlasmaLog.e("debian", "setup falhou", e)
                _ui.value.steps.firstOrNull { it.state == StepState.RUNNING }?.let { step(it.id, StepState.FAILED, e.message ?: "erro") }
                _ui.value = _ui.value.copy(error = e.message ?: e.javaClass.simpleName)
            } finally { _ui.value = _ui.value.copy(running = false) }
        }
    }

    private fun bgRun(nome: String, f: () -> Unit) {
        try {
            f()
        } catch (e: Exception) {
            PlasmaLog.e("debian", "$nome falhou", e)
            val msg = e.message ?: e.javaClass.simpleName
            log("$nome: $msg")
            _ui.value.steps.firstOrNull { it.background && it.state == StepState.RUNNING }?.let { step(it.id, StepState.FAILED, msg) }
        }
    }

    private fun stepMedium() {
        if (paths.stepMarker(S_MEDIUM).exists()) { step(S_MEDIUM, StepState.DONE, "Pacotes complementares prontos"); return }
        step(S_MEDIUM, StepState.RUNNING, "Instalando pacotes complementares…")
        val medium = BASE_PACKAGES.filter { it !in ULTRA_PACKAGES }
        if (!aptInstall(medium)) {
            val falhou = medium.filter { !aptInstallLight(it) }
            if (falhou.isNotEmpty()) log("Pacotes complementares não instalados: ${falhou.joinToString()}")
        }
        runCatching { ensureMesaUtils() }
        runCatching { ensureX11Packages() }
        step(S_MEDIUM, StepState.DONE, "Pacotes complementares instalados")
    }

    fun iniciarSegundoPlano() {
        bgScope.launch {
            bgLock.withLock {
                bgRun("Pacotes complementares") { stepMedium() }
                bgRun("Pi-Apps") { stepPiApps() }
                bgRun("FEX") { stepFex() }
            }
        }
    }

    private fun stepEnv() {
        step(S_ENV, StepState.RUNNING, "Verificando PRoot…")
        val missing = paths.missingBinaries()
        check(missing.isEmpty()) { "Faltam no APK: ${missing.joinToString()}. Rode scripts/fetch-proot.sh (ou build-all.sh) e recompile." }
        ProotLauncher.prepare(paths)
        runCatching { PlasmaVKSupport.initialize(ctx, paths) }
        runCatching { SysVShm.installClient(ctx, paths.rootfs) }
        if (!(paths.rootfs.exists() && paths.marker.exists())) downloadRootfs()
        refreshDns()
        DebianGuest.apply(paths.rootfs, HostDns.deviceServers(ctx))
        step(S_ENV, StepState.DONE, "Debian pronto")
    }

    private fun downloadRootfs() {
        paths.downloads.mkdirs()
        val archive = File(paths.downloads, "debian-rootfs.download")
        var last: Exception? = null
        for (c in DebianCatalog.candidates()) {
            try {
                SafeFs.deleteTree(paths.staging)
                detail(S_ENV, "Baixando Debian ${c.suite}…"); log("Baixando ${c.url}")
                var lastPct = -1
                Downloader.download(c.url, archive, c.sha256) { d, t ->
                    val pct = if (t > 0) (d * 100 / t).toInt() else -1
                    if (pct != lastPct) { lastPct = pct; detail(S_ENV, "Baixando Debian ${c.suite}" + if (pct >= 0) " ($pct%)" else "") }
                }
                detail(S_ENV, "Extraindo rootfs…")
                val r = RootfsExtractor.extract(archive, paths.staging, symlinker = { t, l -> Os.symlink(t, l.path) },
                    onProgress = { n -> if (n % 1000 == 0) detail(S_ENV, "Extraindo… $n entradas") })
                log("Extraído: ${r.files} arquivos, ${r.symlinks} symlinks")
                DebianGuest.prepareBare(paths.staging)
                detail(S_ENV, "Validando com PRoot…")
                var ok = exec("echo plasma-ok", paths.staging, 30_000, quiet = true) == 0
                if (!ok && exec("echo plasma-ok", paths.staging, 30_000, noSeccomp = true, quiet = true) == 0) { paths.noSeccompFlag.writeText("1\n"); ok = true; log("seccomp desligado neste aparelho") }
                check(ok) { "O rootfs Debian não executa via PRoot (veja o log)." }
                SafeFs.deleteTree(paths.previous)
                if (paths.rootfs.exists()) SafeFs.deleteTree(paths.rootfs)
                check(paths.staging.renameTo(paths.rootfs)) { "Não foi possível ativar o rootfs" }
                paths.marker.writeText("installed ${System.currentTimeMillis()}\n")
                archive.delete(); return
            } catch (e: Exception) { last = e; log("Falha: ${e.message}"); SafeFs.deleteTree(paths.staging) } finally { archive.delete() }
        }
        throw last ?: IllegalStateException("Nenhuma fonte de rootfs disponível")
    }

    private fun prootPatched(): Boolean = runCatching {
        ProotLauncher.prepare(paths)
        val pb = ProcessBuilder(paths.proot.path, "-V").redirectErrorStream(true)
        pb.environment().apply { clear(); putAll(ProotLauncher.hostEnv(paths)) }
        val p = pb.start(); runCatching { p.outputStream.close() }
        val sb = StringBuilder()
        val t = Thread { runCatching { sb.append(p.inputStream.bufferedReader().readText()) } }
        t.isDaemon = true; t.start()
        if (!p.waitFor(10, java.util.concurrent.TimeUnit.SECONDS)) p.destroyForcibly()
        t.join(1000)
        sb.contains("Patched For PlasmaDroid")
    }.getOrDefault(false)

    private val HARDLINK_TEST = """
t() { cd "§1" 2>/dev/null || return 0; rm -f .plasma-lt .plasma-lt2; echo t > .plasma-lt && ln .plasma-lt .plasma-lt2 && cmp -s .plasma-lt .plasma-lt2; r=§?; rm -f .plasma-lt .plasma-lt2; [ §r -eq 0 ] || echo "hardlink falhou em §1"; return §r; }
( t /var/lib/dpkg ) && ( t /var/cache/apt ) && ( t /usr/share ) && ( t /etc )
""".replace('§', '$')

    private fun hardlinkTest(detail: StringBuilder = StringBuilder()): Boolean =
        exec(HARDLINK_TEST, quiet = true, sink = { detail.appendLine(it) }) == 0

    private fun enableLink2Symlink(): Boolean {
        if (paths.link2symlinkFlag.exists()) return false
        paths.link2symlinkFlag.parentFile?.mkdirs()
        paths.link2symlinkFlag.writeText("1\n")
        log("Hardlink bloqueado neste aparelho: ativando o modo --link2symlink do PRoot")
        return true
    }

    private fun preflightHardlinks() {
        step(S_PKGS, StepState.RUNNING, "Testando hardlinks…")
        val detail = StringBuilder()
        if (hardlinkTest(detail)) { log("Hardlinks: OK"); return }
        log("Hardlinks falharam: ${detail.toString().trim().lines().takeLast(3).joinToString(" | ")}")
        if (enableLink2Symlink()) {
            detail.clear()
            if (hardlinkTest(detail)) { log("Hardlinks: OK (modo link2symlink)"); return }
        }
        val msg = if (!prootPatched())
            "O libproot.so deste APK não tem o patch PlasmaDroid (hardlink → cópia). " +
                "Rode scripts/build-proot.sh --clean e depois scripts/build-all.sh, e reinstale."
        else
            "Este aparelho não permite hardlinks nem com --link2symlink. ${detail.toString().trim().lines().takeLast(3).joinToString(" | ")}"
        throw IllegalStateException(msg)
    }

    private val APT = "apt-get -o DPkg::Lock::Timeout=900 -o Acquire::Retries=5 -o Acquire::By-Hash=yes -o Acquire::http::Pipeline-Depth=0 " +
        "-o Dpkg::Options::=--force-confold -o Dpkg::Options::=--force-confdef"

    @Volatile private var distUpgraded = false

    @Volatile private var repairBudget = 2

    @Volatile private var lastDpkgError = ""

    private fun repairDpkgBudgeted() {
        if (repairBudget <= 0) { log("Reparos do dpkg esgotados neste passo: seguindo sem reparar de novo"); return }
        repairBudget--
        repairDpkg()
    }

    private fun needsRepair(): Boolean = exec("[ -z \"\$(dpkg --audit 2>&1)\" ]", quiet = true) != 0

    private val CAUSE = Regex("(?i)(dpkg: error|unable to|cannot|can't|permission denied|operation not permitted|no such file|" +
        "bad system call|segmentation|killed by signal|returned error exit status|invalid cross-device|backup link|failed to link|read-only file)")
    private val LINK_PROBLEM = Regex("(?i)(backup link|failed to link|hard ?link|link\\(\\)|cross-device)")

    private fun fixExecBits() {
        exec("ls -l /usr/bin/dpkg-maintscript-helper 2>&1; " +
            "for f in /usr/bin/* /usr/sbin/* /bin/* /sbin/* /usr/lib/dpkg/* /usr/share/debconf/frontend /usr/share/debconf/confmodule; do " +
            "[ -f \"\$f\" ] && [ ! -x \"\$f\" ] && { chmod 755 \"\$f\" && echo \"chmod 755 \$f\"; }; done; true")
    }

    private fun ensureCaBundle() {
        exec("B=/etc/ssl/certs/ca-certificates.crt; " +
            "[ -s \$B ] || update-ca-certificates --fresh 2>&1 | tail -3; " +
            "[ -s \$B ] || { mkdir -p /etc/ssl/certs; cat /usr/share/ca-certificates/mozilla/*.crt > \$B 2>/dev/null; }; " +
            "[ -s \$B ] && echo 'ca-certificates.crt: OK' || echo 'ca-certificates.crt: AUSENTE'", quiet = true, sink = { log(it) })
    }

    private fun probeUnpack(pkgs: List<String>) {
        pkgs.forEach { p ->
            exec("f=\$(ls /var/cache/apt/archives/${p}_*.deb 2>/dev/null | head -1); " +
                "[ -n \"\$f\" ] && { echo \"dpkg -i \$f\"; dpkg -i --force-confold \"\$f\" 2>&1 | tail -12; }; true", quiet = true, sink = { log("» $it") })
        }
    }

    private fun summarizeFailure(out: String) {
        val lines = out.lineSequence().map { it.trim() }.filter { it.isNotEmpty() && CAUSE.containsMatchIn(it) && !it.startsWith("E: Sub-process") }.distinct().toList()
        if (lines.isEmpty()) return
        lastDpkgError = lines.firstOrNull { it.contains("dpkg:", true) || it.contains("unable", true) || it.contains("denied", true) || it.contains("permitted", true) }
            ?: lines.first()
        log("── Causa provável ──")
        lines.take(6).forEach { log("» $it") }
    }

    private fun dropHalfInstalled(pkg: String) {
        log("Removendo $pkg meio instalado para não bloquear os outros pacotes")
        exec("dpkg --remove --force-depends --force-remove-reinstreq $pkg 2>&1 | tail -3; dpkg --configure -a --force-confold 2>&1 | tail -3; true", quiet = true)
    }

    private fun aptInstallLight(pkg: String): Boolean {
        val out = StringBuilder()
        if (exec("$APT install -y --fix-missing $pkg", sink = { out.appendLine(it) }) == 0) return true
        summarizeFailure(out.toString())
        if (out.contains("Permission denied")) { fixExecBits(); if (exec("$APT install -y --fix-missing $pkg") == 0) return true }
        if (LINK_PROBLEM.containsMatchIn(out) && enableLink2Symlink() && exec("$APT install -y --fix-missing $pkg") == 0) return true
        if (exec("$APT install -y --solver internal --fix-missing $pkg", quiet = true) == 0) return true
        dropHalfInstalled(pkg)
        return false
    }

    private fun repairDpkg() {
        val rc = exec(
            "if [ -f /usr/local/lib/plasma/lib.sh ]; then . /usr/local/lib/plasma/lib.sh; " +
                "apply_patch dns >/dev/null 2>&1; apply_patch os-arch >/dev/null 2>&1; apply_patch quiet >/dev/null 2>&1; apply_patch dpkg-systemd >/dev/null 2>&1; repair_dpkg; " +
                "else dpkg --configure -a --force-confold; $APT -f install -y --fix-missing; fi"
        )
        if (rc != 0) log("dpkg ainda inconsistente após o reparo (código $rc)")
    }

    private fun upgradeRootfs() {
        if (distUpgraded) return
        distUpgraded = true
        detail(S_PKGS, "Atualizando o rootfs (full-upgrade)…")
        val out = StringBuilder()
        var rc = exec("$APT full-upgrade -y --fix-missing", sink = { out.appendLine(it) })
        if (rc != 0) {
            summarizeFailure(out.toString())
            if (LINK_PROBLEM.containsMatchIn(out) && enableLink2Symlink()) rc = exec("$APT full-upgrade -y --fix-missing")
        }
        if (rc != 0 || needsRepair()) repairDpkgBudgeted()
    }

    private fun aptUpdate(): Boolean {
        if (exec("$APT update") == 0 && exec("apt-cache policy libc6 | grep -q 'Candidate: [0-9]'", quiet = true) == 0) return true
        log("apt-get update incompleto: apagando listas e tentando de novo…")
        exec("rm -rf /var/lib/apt/lists/* && mkdir -p /var/lib/apt/lists/partial && apt-get clean", quiet = true)
        return exec("$APT update") == 0
    }

    private fun aptInstall(pkgs: List<String>): Boolean {
        val names = pkgs.joinToString(" ")
        var out = StringBuilder()
        fun attempt(extra: String = ""): Boolean {
            out = StringBuilder()
            return exec("$APT install -y $extra $names", sink = { out.appendLine(it) }) == 0
        }
        if (attempt()) return true
        summarizeFailure(out.toString())
        if (out.contains("Permission denied")) { fixExecBits(); if (attempt("--fix-missing")) return true }
        if (LINK_PROBLEM.containsMatchIn(out) && enableLink2Symlink() && attempt("--fix-missing")) return true
        if (needsRepair()) { repairDpkgBudgeted(); if (attempt("--fix-missing")) return true }
        else if (attempt("--fix-missing")) return true
        if (attempt("--solver internal --fix-missing")) return true
        probeUnpack(pkgs)
        return false
    }

    private fun ensureX11Packages() {
        val ok = "command -v xkbcomp >/dev/null 2>&1"
        if (exec(ok, quiet = true) == 0) return
        detail(S_PKGS, "Instalando o servidor X11 (TigerVNC)…")
        aptUpdate()
        if (!aptInstall(X11_PACKAGES)) {
            val failed = X11_PACKAGES.filter { !aptInstallLight(it) }
            if (failed.isNotEmpty()) log("Pacotes do servidor X11 não instalados: ${failed.joinToString()}")
        }
    }

    private fun ensureMesaUtils() {
        if (File(paths.rootfs, "usr/bin/eglinfo").exists() && File(paths.rootfs, "usr/bin/es2gears_wayland").exists()) return
        detail(S_PKGS, "Instalando mesa-utils…")
        aptUpdate()
        aptInstall(listOf("mesa-utils", "mesa-utils-bin", "mesa-utils-extra"))
    }

    private fun stepPackages() {
        preflightHardlinks()
        repairBudget = 2
        lastDpkgError = ""
        step(S_PKGS, StepState.RUNNING, "apt-get update…")

        if (needsRepair()) repairDpkgBudgeted()
        check(aptUpdate()) { "apt-get update falhou (log completo: ${logFile.path})" }
        upgradeRootfs()
        fixExecBits()
        detail(S_PKGS, "Instalando ${ULTRA_PACKAGES.size} pacotes essenciais…")

        val critical = BASE_PACKAGES.filter { it in ULTRA_PACKAGES }
        val failedCritical = if (aptInstall(critical)) emptyList() else critical.filter { !aptInstallLight(it) }
        if (failedCritical.isNotEmpty()) {
            log("Diagnóstico (mande este trecho do log):")
            exec("dpkg --audit 2>&1 | head -20; apt-cache policy libc6 libtirpc-common libtirpc3t64 iproute2 2>&1 | head -40")
            exec("f=\$(ls /var/cache/apt/archives/libtirpc3t64*.deb /var/cache/apt/archives/*.deb 2>/dev/null | head -1); [ -n \"\$f\" ] && { echo \"dpkg -i \$f\"; dpkg -i --force-confold \"\$f\" 2>&1 | tail -15; }")
        }
        check(failedCritical.isEmpty()) {
            "Falha ao instalar pacotes básicos: ${failedCritical.joinToString()}." +
                (if (lastDpkgError.isNotBlank()) " Causa: $lastDpkgError." else "") + " Log completo: ${logFile.path}"
        }
        ensureCaBundle()
        exec("sed -i 's/^# *pt_BR.UTF-8/pt_BR.UTF-8/;s/^# *en_US.UTF-8/en_US.UTF-8/' /etc/locale.gen; locale-gen >/dev/null 2>&1 || true", quiet = true)
        step(S_PKGS, StepState.DONE, "Pacotes básicos instalados")
    }

    private fun stepSources() {
        step(S_SRC, StepState.RUNNING, "Detectando suíte…")
        val suite = captured(". /etc/os-release; echo \$VERSION_CODENAME").lineSequence().lastOrNull { it.isNotBlank() }?.trim().orEmpty().ifEmpty { "trixie" }
        DebianGuest.writeSources(paths.rootfs, suite)
        log("Source lists: Debian $suite (main contrib non-free non-free-firmware)")
        detail(S_SRC, "apt-get update ($suite)…")
        check(aptUpdate()) { "apt-get update falhou com as novas source lists" }
        step(S_SRC, StepState.DONE, "Debian $suite · main contrib non-free")
    }

    private fun stepPlasmaVK() {
        step(S_KR, StepState.RUNNING, "Instalando e verificando ICD Vulkan…")
        val result = PlasmaVKSupport.ensureRuntime(ctx, paths.rootfs)
        log("PlasmaVK: ${result.name.lowercase()}")
        when (result) {
            PlasmaVKSupport.InstallResult.NO_RUNTIME -> {
                val message = "runtime Vulkan não incluído no APK; gere novamente runtime.tar ou runtime.tar.gz"
                log(message)
                step(S_KR, StepState.FAILED, message)
                return
            }
            PlasmaVKSupport.InstallResult.FAILED -> {
                val message = "falha ao instalar o runtime Vulkan; consulte o log PlasmaVK"
                log(message)
                step(S_KR, StepState.FAILED, message)
                return
            }
            PlasmaVKSupport.InstallResult.INSTALLED,
            PlasmaVKSupport.InstallResult.UP_TO_DATE -> Unit
        }

        BionicDeps.check(paths.rootfs).let { r -> if (!r.ok) log("PlasmaVK: ${r.describe()}") }
        val manifest = PlasmaVKSupport.detectManifest(paths.rootfs)
        if (exec("command -v vulkaninfo >/dev/null 2>&1", quiet = true) != 0) {
            detail(S_KR, "Instalando vulkan-tools…")
            fixExecBits()
            aptInstall(listOf("vulkan-tools"))
        }
        val output = StringBuilder()
        val exitCode = exec(
            "if command -v vulkaninfo >/dev/null 2>&1; then vulkaninfo --summary 2>&1; else echo 'vulkan-tools não instalado'; exit 127; fi",
            timeoutMs = 60_000,
            quiet = true,
            sink = { line -> output.appendLine(line) },
        )
        val report = output.toString()
        val devices = parseVulkanDevices(report)
        val hardware = devices.filterNot { (name, type) ->
            val token = "$name $type".lowercase()
            token.contains("lavapipe") || token.contains("llvmpipe") || token.contains("softpipe") ||
                token.contains("swrast") || token.contains("swiftshader") || type.contains("CPU", ignoreCase = true)
        }
        val software = devices.filter { (name, type) ->
            val token = "$name $type".lowercase()
            token.contains("lavapipe") || token.contains("llvmpipe") || token.contains("softpipe") ||
                token.contains("swrast") || token.contains("swiftshader") || type.contains("CPU", ignoreCase = true)
        }

        when {
            exitCode != 0 -> {
                val base = PlasmaVKSupport.status(paths.rootfs)
                val detail = if (exitCode == 127) {
                    "vulkan-tools não instalado; não foi possível confirmar a GPU ($base)"
                } else {
                    "ICD ${manifest ?: "padrão"} encontrado, mas vulkaninfo falhou (código $exitCode); $base"
                }
                log(detail)
                if (report.isNotBlank()) report.lineSequence().take(30).forEach(::log)
                step(S_KR, StepState.FAILED, detail)
            }
            hardware.isNotEmpty() -> {
                val names = hardware.joinToString(", ") { (name, _) -> name }
                val detail = if (manifest != null) "GPU reconhecida: $names" else "GPU reconhecida pelo loader do sistema: $names"
                log(detail)
                step(S_KR, StepState.DONE, detail)
            }
            software.isNotEmpty() -> {
                val names = software.joinToString(", ") { (name, _) -> name }
                val detail = "somente Vulkan por CPU ($names / LLVM); GPU física não foi reconhecida"
                log(detail)
                step(S_KR, StepState.FAILED, detail)
            }
            else -> {
                val base = PlasmaVKSupport.status(paths.rootfs)
                val detail = "nenhum dispositivo físico enumerado; $base"
                log(detail)
                if (report.isNotBlank()) report.lineSequence().take(30).forEach(::log)
                step(S_KR, StepState.FAILED, detail)
            }
        }
    }

    private fun parseVulkanDevices(report: String): List<Pair<String, String>> {
        val names = Regex("(?im)^\\s*deviceName\\s*=\\s*(.*?)\\s*$")
            .findAll(report).map { it.groupValues[1].trim() }.filter { it.isNotBlank() }.toList()
        val types = Regex("(?im)^\\s*deviceType\\s*=\\s*(.*?)\\s*$")
            .findAll(report).map { it.groupValues[1].trim() }.toList()
        return names.mapIndexed { index, name -> name to types.getOrElse(index) { "" } }
    }

    suspend fun ensurePiAppsRuns() {
        installPiAppsFiles()
        if (paths.noSeccompFlag.exists()) paths.noSeccompFlag.delete()
    }

    fun installPiAppsFiles() {
        if (!File(paths.rootfs, "usr/local/bin/pi-apps").exists()) return
        fun copyAsset(asset: String, dst: File) = runCatching {
            dst.parentFile?.mkdirs()
            ctx.assets.open(asset).use { input -> dst.outputStream().use { input.copyTo(it) } }
        }.onFailure { log("Pi-Apps: $asset não copiado (${it.message})") }
        val png = File(paths.rootfs, "usr/share/pixmaps/pi-apps.png")
        if (!png.exists()) copyAsset("pi-apps.png", png)
        copyAsset("pi-apps.desktop", File(paths.rootfs, "usr/share/applications/pi-apps.desktop"))
    }

    private fun stepPiApps() {
        installPiAppsFiles()
        if (paths.stepMarker(S_PIAPPS).exists() && File(paths.rootfs, "usr/local/bin/pi-apps").exists()) {
            step(S_PIAPPS, StepState.DONE, "Pi-Apps pronto"); return
        }
        step(S_PIAPPS, StepState.RUNNING, "Instalando o Pi-Apps…")
        val rc = runCatching {
            exec("plasma install pi-apps", timeoutMs = 30 * 60_000L, quiet = true,
                sink = { line -> log(line); detail(S_PIAPPS, line.take(120)) })
        }.getOrElse { e -> log("Pi-Apps: ${e.message}"); -1 }
        if (rc == 0 && File(paths.rootfs, "usr/local/bin/pi-apps").exists()) {
            step(S_PIAPPS, StepState.DONE, "Pi-Apps instalado")
        } else {
            step(S_PIAPPS, StepState.FAILED, "Não instalou (código $rc). Rode no terminal: plasma install pi-apps")
        }
    }

    fun ensureFexHost() {
        val dst = File(paths.fexHost, "rootfs-debian-min.sqsh")
        if (dst.exists()) return
        runCatching {
            paths.fexHost.mkdirs()
            val tmp = File(paths.fexHost, "rootfs-debian-min.sqsh.part")
            ctx.assets.open(FEX_ROOTFS_ASSET).use { input -> tmp.outputStream().use { input.copyTo(it) } }
            if (!tmp.renameTo(dst)) throw java.io.IOException("renomear falhou")
        }
    }

    private fun stepFex() {
        val pagina = android.system.Os.sysconf(android.system.OsConstants._SC_PAGESIZE)
        if (pagina != 4096L) {
            step(S_FEX, StepState.FAILED, "Kernel com páginas de ${pagina / 1024} KB: o FEX só roda com 4 KB (NOTES N-001)")
            return
        }
        val hasAsset = runCatching { ctx.assets.open(FEX_ASSET).close() }.isSuccess
        if (!hasAsset) { step(S_FEX, StepState.DONE, "FEX não incluído neste APK"); return }
        val rfsDst = File(paths.rootfs, "usr/local/lib/plasma/fex/rootfs-debian-min.sqsh")
        if (!rfsDst.exists()) runCatching {
            rfsDst.parentFile?.mkdirs()
            val tmp = File(rfsDst.parentFile, rfsDst.name + ".part")
            ctx.assets.open(FEX_ROOTFS_ASSET).use { input -> tmp.outputStream().use { input.copyTo(it) } }
            if (!tmp.renameTo(rfsDst)) throw java.io.IOException("renomear falhou")
        }.onFailure { log("FEX: rootfs mínimo não copiado (${it.message})") }
        if (paths.stepMarker(S_FEX).exists() && File(paths.rootfs, "usr/local/lib/plasma/fex/.ready").exists()) {
            step(S_FEX, StepState.DONE, "FEX pronto"); return
        }
        step(S_FEX, StepState.RUNNING, "Instalando o FEX (x86-64 → ARM64)…")
        val copied = runCatching {
            val dst = File(paths.rootfs, "usr/local/lib/plasma/fex/fex-aarch64.tar.xz")
            dst.parentFile?.mkdirs()
            ctx.assets.open(FEX_ASSET).use { input -> dst.outputStream().use { input.copyTo(it) } }
            true
        }.getOrElse { e -> log("FEX: falhou ao copiar o pacote (${e.message})"); false }
        val rc = if (!copied) -1 else runCatching {
            exec("plasma install fex", timeoutMs = 60 * 60_000L, quiet = true,
                sink = { line -> log(line); detail(S_FEX, line.take(120)) })
        }.getOrElse { e -> log("FEX: ${e.message}"); -1 }
        if (rc == 0 && File(paths.rootfs, "usr/local/lib/plasma/fex/.ready").exists()) {
            step(S_FEX, StepState.DONE, "FEX instalado")
        } else {
            step(S_FEX, StepState.FAILED, "Não instalou (código $rc). Rode no terminal: plasma install fex")
        }
    }

    private fun stepFastfetch() {
        if (!paths.stepMarker(S_FETCH).exists() || !File(paths.rootfs, "usr/bin/fastfetch").exists()) {
            step(S_FETCH, StepState.RUNNING, "Instalando fastfetch (apt)…")
            fixExecBits()
            ensureCaBundle()
            var ok = aptInstall(listOf("fastfetch"))
            if (!ok) {
                detail(S_FETCH, "Baixando .deb do GitHub…")
                ok = exec("cd /tmp && curl -fL -o fastfetch.deb '$FASTFETCH_DEB' && (dpkg -i fastfetch.deb || $APT install -f -y) && rm -f fastfetch.deb") == 0
            }
            check(ok && File(paths.rootfs, "usr/bin/fastfetch").exists()) { "Não foi possível instalar o fastfetch" }
        }
        step(S_FETCH, StepState.RUNNING, "Executando fastfetch…")
        val sb = StringBuilder()
        exec("fastfetch --pipe false 2>&1", quiet = true, sink = { sb.appendLine(it) })
        _ui.value = _ui.value.copy(fastfetch = sb.toString().trimEnd())
        step(S_FETCH, StepState.DONE, "Fastfetch executado")
    }

    suspend fun reset() = lock.withLock { withContext(Dispatchers.IO) {
        listOf(paths.rootfs, paths.staging, paths.previous).forEach { runCatching { SafeFs.deleteTree(it) } }
        paths.root.resolve("debian").listFiles { f -> f.name.startsWith(".") }?.forEach { it.delete() }
        _ui.value = SetupUi(initialSteps())
    } }
}
