// Parte do PlasmaDroid — módulo PlasmaVK.
// Créditos: baseado no projeto Proot.

package org.plasma.proot

import android.content.Context
import android.system.Os
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import org.plasma.core.PlasmaLog
import java.io.File

enum class StepState { PENDING, RUNNING, DONE, FAILED }
data class SetupStep(val id: String, val label: String, val state: StepState = StepState.PENDING, val detail: String = "")
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
        private const val FASTFETCH_DEB = "https://github.com/fastfetch-cli/fastfetch/releases/latest/download/fastfetch-linux-aarch64.deb"
        val BASE_PACKAGES = listOf("ca-certificates", "curl", "wget", "locales", "procps", "iproute2", "pciutils", "nano", "less",
            "sudo", "xz-utils", "libvulkan1", "vulkan-tools", "mesa-utils", "mesa-utils-bin", "mesa-utils-extra",
            "mesa-vulkan-drivers", "wayland-utils")
        private val CRITICAL_PACKAGES = setOf("ca-certificates", "curl", "wget", "locales", "procps", "sudo", "libvulkan1")
    }

    private val lock = Mutex()
    private fun initialSteps() = listOf(
        SetupStep(S_ENV, "Ambiente Debian"),
        SetupStep(S_PKGS, "Pacotes básicos"),
        SetupStep(S_SRC, "Source lists"),
        SetupStep(S_KR, "Graphics Environment (Vulkan And PlasmaGL)"),
        SetupStep(S_FETCH, "Fastfetch"),
    ).map { if (paths.stepMarker(it.id).exists()) it.copy(state = StepState.DONE) else it }

    private val _ui = MutableStateFlow(SetupUi(initialSteps(), complete = paths.setupDone.exists()))
    val ui: StateFlow<SetupUi> = _ui.asStateFlow()

    val installed: Boolean get() = paths.rootfs.exists() && paths.marker.exists()
    val ready: Boolean get() = installed && paths.setupDone.exists()

    private fun log(l: String) { _ui.value = _ui.value.let { it.copy(log = (it.log + l).takeLast(400)) } }
    private fun step(id: String, st: StepState, detail: String = "") {
        _ui.value = _ui.value.copy(steps = _ui.value.steps.map { if (it.id == id) it.copy(state = st, detail = detail) else it })
        val marker = paths.stepMarker(id)
        if (st == StepState.DONE) marker.writeText("1\n") else if (st == StepState.FAILED || st == StepState.PENDING) marker.delete()
    }
    private fun detail(id: String, d: String) { _ui.value = _ui.value.copy(steps = _ui.value.steps.map { if (it.id == id) it.copy(detail = d) else it }) }

    fun exec(script: String, rootfs: File = paths.rootfs, timeoutMs: Long = 20 * 60_000L, noSeccomp: Boolean = false, quiet: Boolean = false, sink: ((String) -> Unit)? = null): Int {
        ProotLauncher.prepare(paths)
        val cmd = ProotLauncher.command(paths, rootfs, listOf("/bin/bash", "-c", script),
            guestEnv = mapOf("DEBIAN_FRONTEND" to "noninteractive", "LC_ALL" to "C.UTF-8"), useGuestEnvWrapper = File(rootfs, "usr/bin/env").exists())
        val pb = ProcessBuilder(cmd).directory(paths.root).redirectErrorStream(true)
        pb.environment().apply { clear(); putAll(ProotLauncher.hostEnv(paths, noSeccomp)) }
        val p = pb.start(); runCatching { p.outputStream.close() }
        val t = Thread { p.inputStream.bufferedReader().forEachLine { l -> val c = l.substringAfterLast('\r'); sink?.invoke(c); if (!quiet) log(c) } }
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

    suspend fun setup() = lock.withLock {
        withContext(Dispatchers.IO) {
            _ui.value = _ui.value.copy(running = true, error = null, complete = false, steps = initialSteps())
            try {
                if (!paths.stepMarker(S_ENV).exists()) stepEnv() else DebianGuest.apply(paths.rootfs)
                if (!paths.stepMarker(S_PKGS).exists()) stepPackages() else runCatching { ensureMesaUtils() }
                if (!paths.stepMarker(S_SRC).exists()) stepSources()
                stepPlasmaVK()
                stepFastfetch()
                paths.setupDone.writeText("done ${System.currentTimeMillis()}\n")
                _ui.value = _ui.value.copy(complete = true)
            } catch (e: Exception) {
                PlasmaLog.e("debian", "setup falhou", e)
                _ui.value.steps.firstOrNull { it.state == StepState.RUNNING }?.let { step(it.id, StepState.FAILED, e.message ?: "erro") }
                _ui.value = _ui.value.copy(error = e.message ?: e.javaClass.simpleName)
            } finally { _ui.value = _ui.value.copy(running = false) }
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
        DebianGuest.apply(paths.rootfs)
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

    private fun preflightHardlinks() {
        step(S_PKGS, StepState.RUNNING, "Testando hardlinks em /var…")
        val t = ".plasma-lt"
        val script = "cd /var/lib/dpkg && rm -f $t $t" + "2 && echo t > $t && ln $t $t" + "2 && cmp -s $t $t" + "2 " +
            "&& rm -f $t $t" + "2 || { rm -f $t $t" + "2; exit 1; }"
        val out = StringBuilder()
        val rc = exec(script, quiet = true, sink = { out.appendLine(it) })
        if (rc == 0) { log("Hardlinks em /var: OK"); return }
        val detail = out.toString().trim().lines().takeLast(3).joinToString(" | ")
        val msg = if (!prootPatched())
            "O libproot.so deste APK não tem o patch PlasmaDroid (hardlink → cópia). " +
                "Rode scripts/build-proot.sh --clean e depois scripts/build-all.sh, e reinstale."
        else
            "O PRoot tem o patch, mas link() em /var/lib/dpkg ainda falha (código $rc). $detail"
        throw IllegalStateException(msg)
    }

    private val APT = "apt-get -o Acquire::Retries=5 -o Acquire::By-Hash=yes -o Acquire::http::Pipeline-Depth=0 " +
        "-o Dpkg::Options::=--force-confold -o Dpkg::Options::=--force-confdef"

    @Volatile private var distUpgraded = false

    private fun repairDpkg() {
        exec("dpkg --configure -a --force-confold || true", quiet = true)
        exec("$APT -f install -y --fix-missing || true")
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
        fun attempt(extra: String = "", list: String = names): Boolean {
            out = StringBuilder()
            return exec("$APT install -y $extra $list", sink = { out.appendLine(it) }) == 0
        }
        if (attempt()) return true
        if (!out.contains("Unmet dependencies") && !out.contains("unmet") && !out.contains("not installable") && !out.contains("Broken packages")) {
            return attempt("--fix-missing")
        }
        log("Dependências não resolvidas: reparando dpkg e atualizando listas…")
        repairDpkg()
        if (attempt("--fix-missing")) return true
        if (aptUpdate() && attempt("--fix-missing")) return true

        if (!distUpgraded) {
            distUpgraded = true
            log("Alinhando o rootfs com o mirror (apt-get dist-upgrade)…")
            exec("$APT dist-upgrade -y --fix-missing || true")
            if (attempt("--fix-missing")) return true
        }

        val deps = Regex("""Depends: ([a-z0-9][a-z0-9+.\-]*)""").findAll(out).map { it.groupValues[1] }.toSet()
        if (deps.isNotEmpty()) {
            log("Instalando dependências citadas: ${deps.joinToString()}")
            exec("apt-cache policy ${deps.joinToString(" ")} 2>&1 | head -40")
            exec("$APT install -y --fix-missing ${deps.joinToString(" ")} || true")
            return attempt("--fix-missing")
        }
        return false
    }

    private fun ensureMesaUtils() {
        if (File(paths.rootfs, "usr/bin/eglinfo").exists() && File(paths.rootfs, "usr/bin/es2gears_wayland").exists()) return
        detail(S_PKGS, "Instalando mesa-utils…")
        aptUpdate()
        aptInstall(listOf("mesa-utils", "mesa-utils-bin", "mesa-utils-extra"))
    }

    private fun stepPackages() {
        preflightHardlinks()
        step(S_PKGS, StepState.RUNNING, "apt-get update…")

        repairDpkg()
        check(aptUpdate()) { "apt-get update falhou" }
        detail(S_PKGS, "Instalando ${BASE_PACKAGES.size} pacotes…")

        aptInstall(listOf("libtirpc3t64")) || aptInstall(listOf("libtirpc3"))

        if (!aptInstall(BASE_PACKAGES)) {
            repairDpkg()
            val failed = BASE_PACKAGES.filter { !aptInstall(listOf(it)) }
            val critical = BASE_PACKAGES.filter { it in CRITICAL_PACKAGES }
            check(failed.none { it in critical }) { "Falha ao instalar pacotes básicos: ${failed.filter { it in critical }.joinToString()}" }
            if (failed.isNotEmpty()) log("Pacotes opcionais não instalados: ${failed.joinToString()}")
        }
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

    private fun stepFastfetch() {
        if (!paths.stepMarker(S_FETCH).exists() || !File(paths.rootfs, "usr/bin/fastfetch").exists()) {
            step(S_FETCH, StepState.RUNNING, "Instalando fastfetch (apt)…")
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
