
package org.plasma.proot

import android.content.Context
import java.io.File

object PlasmaVKSupport {
    @Volatile var enabled: Boolean = true

    @Volatile var driverPreference: String = "auto"

    @Volatile var wsiDmabuf: Boolean = false

    private const val BIONIC_HAL_DIR = "usr/lib/plasmavk/bionic/hal"

    private const val BIONIC_DRV_DIR = "usr/lib/plasmavk/bionic/drivers"
    private const val INJECTED_DIR = "usr/lib/aarch64-linux-gnu/plasmavk-injected"
    private const val INJECTED_MANIFEST = "usr/share/vulkan/icd.d/plasmavk_injected_icd.aarch64.json"

    private val ANDROID_HAL_DIRS = listOf(
        "/vendor/lib64/hw", "/vendor/lib64/egl", "/odm/lib64/hw", "/odm/lib64/egl",
        "/system/lib64/hw", "/system/lib64/egl", "/system_ext/lib64/hw",
    )

    private val LIB_DIRS = listOf(
        "/usr/lib/aarch64-linux-gnu",
        "/usr/lib/aarch64-linux-gnu/libhybris",
        "/usr/lib/plasmavk",
    )
    private val ICD_DIRS = listOf(
        "usr/share/vulkan/icd.d",
        "etc/vulkan/icd.d",
        "usr/local/share/vulkan/icd.d",
        "usr/lib/aarch64-linux-gnu/vulkan/icd.d",
        "usr/lib/vulkan/icd.d",
    )
    private val LIB_SEARCH = listOf(
        "usr/lib/aarch64-linux-gnu",
        "usr/lib/aarch64-linux-gnu/libhybris",
        "usr/lib/plasmavk",
    )

    private val ASSET_CANDIDATES = listOf(
        "plasmavk/runtime.tar.gz",
        "plasmavk/runtime.tar",
    )
    private const val MARKER = "usr/lib/plasmavk/.runtime-crc"
    private val LIBRARY_PATH = Regex("\"library_path\"\\s*:\\s*\"([^\"]+)\"")

    private enum class DriverKind(val label: String, val priority: Int) {

        INJECTED("Driver Vulkan injetado", 0),
        WRAPPER("Mesa Vulkan wrapper", 0),
        HYBRIS("ICD do sistema via libhybris", 1),
        PANVK("Mesa PanVK / Mali", 2),
        TURNIP("Mesa Turnip / Adreno", 3),
        OTHER_HARDWARE("ICD Vulkan de hardware", 4),
        SOFTWARE("Lavapipe / LLVM (software)", 5),
        UNKNOWN("ICD não classificado", 6),
    }

    private class Manifest(val guestPath: String, val file: File, val library: String?) {
        fun token(): String = (file.name + " " + library.orEmpty()).lowercase()
    }

    @Synchronized fun initialize(ctx: Context, p: ProotPaths) {
        if (p.rootfs.isDirectory) {
            runCatching { repairManifests(p.rootfs) }
            runCatching { ensureRuntime(ctx, p.rootfs) }
        }
    }

    enum class InstallResult { INSTALLED, UP_TO_DATE, NO_RUNTIME, FAILED }

    private fun migrateLegacyLibDir(rootfs: File) {
        val lib = File(rootfs, "usr/lib")
        val legacy = File(lib, "prootkr")
        val current = File(lib, "plasmavk")
        if (java.nio.file.Files.isSymbolicLink(legacy.toPath()) || !legacy.isDirectory) return
        if (current.exists()) {
            android.util.Log.w("PlasmaVK", "usr/lib/prootkr e usr/lib/plasmavk existem; migração não aplicada")
            return
        }
        if (!legacy.renameTo(current)) {
            android.util.Log.e("PlasmaVK", "falha ao mover usr/lib/prootkr para usr/lib/plasmavk")
            return
        }
        runCatching { android.system.Os.symlink("plasmavk", legacy.path) }
            .onFailure { android.util.Log.e("PlasmaVK", "migrado, mas falhou o symlink prootkr -> plasmavk", it) }
        android.util.Log.i("PlasmaVK", "migrado: usr/lib/prootkr -> usr/lib/plasmavk")
    }

    fun ensureRuntime(ctx: Context, rootfs: File): InstallResult {
        runCatching { migrateLegacyLibDir(rootfs) }
        val tmp = File(ctx.cacheDir, "plasmavk-runtime.tar")
        var selectedAsset: String? = null
        var crcValue: Long? = null

        for (assetName in ASSET_CANDIDATES) {
            val crc = java.util.zip.CRC32()
            val copied = runCatching {
                ctx.assets.open(assetName).use { input ->
                    java.io.FileOutputStream(tmp, false).use { output ->
                        val buffer = ByteArray(65536)
                        while (true) {
                            val count = input.read(buffer)
                            if (count < 0) break
                            crc.update(buffer, 0, count)
                            output.write(buffer, 0, count)
                        }
                    }
                }
            }.isSuccess
            if (copied && tmp.isFile && tmp.length() > 0L) {
                selectedAsset = assetName
                crcValue = crc.value
                break
            }
            tmp.delete()
        }
        val assetName = selectedAsset ?: return InstallResult.NO_RUNTIME

        return try {

            val stamp = assetName.substringAfterLast('/') + ":" + crcValue!!.toString(16)
            val marker = File(rootfs, MARKER)
            val alreadyInstalled = marker.isFile && marker.readText().trim() == stamp &&
                hasAnyUsableManifest(rootfs)
            if (alreadyInstalled) {
                repairManifests(rootfs)
                runCatching { quarantineShadowingLibs(rootfs) }
                InstallResult.UP_TO_DATE
            } else {
                RootfsExtractor.extract(
                    tmp,
                    rootfs,
                    symlinker = { target, link ->
                        link.delete()
                        android.system.Os.symlink(target, link.path)
                    },
                )
                repairManifests(rootfs)
                quarantineShadowingLibs(rootfs)
                marker.parentFile?.mkdirs()
                marker.writeText(stamp + "\n")
                InstallResult.INSTALLED
            }
        } catch (error: Exception) {
            android.util.Log.e("PlasmaVK", "falha instalando runtime", error)
            InstallResult.FAILED
        } finally {
            tmp.delete()
        }
    }

    private val SHADOW_LIBS = Regex("^(libvulkan\\.so.*|libEGL\\.so.*|libGLES.*\\.so.*|libOpenCL\\.so.*|libgbm\\.so.*)$")

    private fun isHybrisBuild(file: File): Boolean = runCatching {
        val needle = "libhybris".toByteArray()
        val chunk = 65536
        val buf = ByteArray(chunk + needle.size)
        var kept = 0
        var found = false
        file.inputStream().use { input ->
            while (!found) {
                val n = input.read(buf, kept, chunk)
                if (n < 0) break
                val total = kept + n
                var i = 0
                while (i <= total - needle.size && !found) {
                    var j = 0
                    while (j < needle.size && buf[i + j] == needle[j]) j++
                    if (j == needle.size) found = true
                    i++
                }
                kept = minOf(needle.size - 1, total)
                System.arraycopy(buf, total - kept, buf, 0, kept)
            }
        }
        found
    }.getOrDefault(false)

    fun quarantineShadowingLibs(rootfs: File) {
        val multiarch = File(rootfs, "usr/lib/aarch64-linux-gnu")
        val priv = File(rootfs, "usr/lib/plasmavk").apply { mkdirs() }
        val files = multiarch.listFiles() ?: return
        for (file in files) {
            if (!SHADOW_LIBS.matches(file.name)) continue
            if (!file.isFile) continue
            if (!isHybrisBuild(file)) continue
            val dest = File(priv, file.name)
            runCatching {
                dest.delete()
                if (!file.renameTo(dest)) { file.copyTo(dest, overwrite = true); file.delete() }
                android.util.Log.w("PlasmaVK", "lib do hybris tirada do multiarch: ${file.name} -> /usr/lib/plasmavk")
            }.onFailure { android.util.Log.w("PlasmaVK", "não consegui mover ${file.name}", it) }
        }
    }

    private fun libraryOf(file: File): String? =
        runCatching { LIBRARY_PATH.find(file.readText())?.groupValues?.get(1) }.getOrNull()

    private fun manifests(rootfs: File): List<Manifest> = ICD_DIRS.flatMap { relative ->
        File(rootfs, relative).listFiles { file -> file.isFile && file.name.endsWith(".json") }
            ?.sortedBy { it.name }
            ?.map { file -> Manifest("/$relative/${file.name}", file, libraryOf(file)) }
            .orEmpty()
    }

    private fun resolves(rootfs: File, guestLibrary: String?): Boolean {
        if (guestLibrary.isNullOrBlank() || !guestLibrary.startsWith('/')) return false
        if (guestLibrary.split('/').any { it == ".." }) return false

        if (!guestLibrary.startsWith("/usr/lib/aarch64-linux-gnu/")) return false
        return runCatching {
            val base = rootfs.canonicalFile.toPath()
            val candidate = File(rootfs, guestLibrary.removePrefix("/")).canonicalFile
            candidate.toPath().startsWith(base) && candidate.isFile && candidate.canRead()
        }.getOrDefault(false)
    }

    private fun kind(manifest: Manifest): DriverKind {
        val token = manifest.token()
        return when {
            manifest.file.name.startsWith("plasmavk_injected") -> DriverKind.INJECTED
            listOf("lavapipe", "llvmpipe", "softpipe", "swrast", "libvulkan_lvp", "_lvp", "swiftshader").any(token::contains) -> DriverKind.SOFTWARE
            listOf("panvk", "panfrost", "mali").any(token::contains) -> DriverKind.PANVK
            "hybris" in token -> DriverKind.HYBRIS
            listOf("freedreno", "turnip", "kgsl", "adreno").any(token::contains) -> DriverKind.TURNIP
            "wrapper" in token -> DriverKind.WRAPPER
            listOf("radv", "amdvlk", "anv", "intel", "nouveau", "nvk", "v3dv", "vc4", "virtio", "gfxstream", "dzn").any(token::contains) -> DriverKind.OTHER_HARDWARE
            else -> DriverKind.UNKNOWN
        }
    }

    private fun availableManifests(rootfs: File): List<Manifest> = manifests(rootfs).filter { manifest ->
        val driver = kind(manifest)
        resolves(rootfs, manifest.library) && driver != DriverKind.UNKNOWN &&
            (driver != DriverKind.WRAPPER || detectInnerLibrary(rootfs) != null || detectBionicWrapper(rootfs) != null)
    }

    private fun wantedKind(): DriverKind? = when (driverPreference) {
        "panvk" -> DriverKind.PANVK
        "turnip" -> DriverKind.TURNIP
        "injected" -> DriverKind.INJECTED
        "injected_bionic" -> DriverKind.WRAPPER
        else -> null
    }

    private fun preferredManifest(rootfs: File, includeSoftware: Boolean): Manifest? {
        val wanted = wantedKind()
        return availableManifests(rootfs)
            .filter { includeSoftware || kind(it) != DriverKind.SOFTWARE }

            .filter { if (wanted != null) kind(it) == wanted else kind(it) != DriverKind.INJECTED }
            .sortedWith(compareBy<Manifest> { kind(it).priority }.thenBy { it.file.name })
            .firstOrNull()
    }

    private fun hasAnyUsableManifest(rootfs: File): Boolean =
        preferredManifest(rootfs, includeSoftware = true) != null

    fun detectBionicWrapper(rootfs: File): String? {
        for (name in listOf("libvulkan_wrapper.so", "libwrapper_vulkan.so")) {
            val guest = "/usr/lib/plasmavk/bionic/$name"
            if (File(rootfs, guest.removePrefix("/")).isFile) return guest
        }
        return null
    }

    fun detectInnerLibrary(rootfs: File): String? {
        for (directory in LIB_DIRS) {
            val base = File(rootfs, directory.removePrefix("/"))
            if (!base.isDirectory) continue
            val hit = runCatching {
                base.walkTopDown().maxDepth(3).firstOrNull { file ->
                    file.isFile && file.name.contains("hybris", ignoreCase = true) &&
                        file.name.contains("vulkan", ignoreCase = true) && file.name.contains(".so")
                }
            }.getOrNull()
            if (hit != null) return "/" + hit.relativeTo(rootfs).invariantSeparatorsPath
        }
        return null
    }

    fun detectManifest(rootfs: File): String? =
        preferredManifest(rootfs, includeSoftware = false)?.guestPath

    fun repairManifests(rootfs: File) {
        for (manifest in manifests(rootfs)) {
            val libraryName = manifest.library?.substringAfterLast('/')?.takeIf { it.isNotBlank() } ?: continue

            val standard = File(rootfs, "usr/lib/aarch64-linux-gnu/$libraryName")
            if (standard.isFile) {
                val standardPath = "/usr/lib/aarch64-linux-gnu/$libraryName"
                if (manifest.library != standardPath) {
                    runCatching {
                        val contents = manifest.file.readText()
                        manifest.file.writeText(contents.replace(LIBRARY_PATH) { "\"library_path\": \"$standardPath\"" })
                    }.onFailure { android.util.Log.w("PlasmaVK", "não foi possível normalizar ${manifest.file}", it) }
                }
                continue
            }
            if (resolves(rootfs, manifest.library)) continue
            val found = LIB_SEARCH.asSequence()
                .map { File(rootfs, it) }
                .filter { it.isDirectory }
                .flatMap { directory -> directory.walkTopDown().maxDepth(4) }
                .firstOrNull { file -> file.isFile && file.name == libraryName }
                ?: continue
            val guestPath = "/" + found.relativeTo(rootfs).invariantSeparatorsPath
            runCatching {
                val contents = manifest.file.readText()
                manifest.file.writeText(contents.replace(LIBRARY_PATH) { "\"library_path\": \"$guestPath\"" })
            }.onFailure { android.util.Log.w("PlasmaVK", "não foi possível reparar ${manifest.file}", it) }
        }
    }

    fun guestEnv(rootfs: File): Map<String, String> {
        if (!enabled) return emptyMap()
        val selected = preferredManifest(rootfs, includeSoftware = false) ?: return softwareEnv(rootfs)
        val selectedKind = kind(selected)

        if ((selectedKind == DriverKind.HYBRIS || selectedKind == DriverKind.WRAPPER) &&
            probeState(rootfs, selected) == false) return softwareEnv(rootfs)
        return hardwareEnv(rootfs, selected)
    }

    private fun hardwareEnv(rootfs: File, selected: Manifest): Map<String, String> {
        val selectedKind = kind(selected)
        val wrapper = selectedKind == DriverKind.WRAPPER
        val bionicWrapper = if (wrapper) detectBionicWrapper(rootfs) else null

        val inner = if (wrapper) (if (bionicWrapper != null) injectedBionicIcd(rootfs) else (detectInnerLibrary(rootfs) ?: return emptyMap())) else null
        val backend = when (selectedKind) {
            DriverKind.PANVK -> PlasmaVK.Backend.MESA_PANVK_KBASE
            DriverKind.TURNIP -> PlasmaVK.Backend.MESA_TURNIP
            DriverKind.HYBRIS -> PlasmaVK.Backend.SYSTEM_BIONIC_LIBHYBRIS
            DriverKind.SOFTWARE -> PlasmaVK.Backend.LAVAPIPE
            DriverKind.OTHER_HARDWARE -> PlasmaVK.Backend.DIRECT_ICD
            DriverKind.INJECTED -> PlasmaVK.Backend.DIRECT_ICD
            DriverKind.WRAPPER -> PlasmaVK.Backend.SYSTEM_BIONIC_LIBHYBRIS
            DriverKind.UNKNOWN -> return emptyMap()
        }
        val env = runCatching {
            PlasmaVK.buildGuestEnvironment(
                PlasmaVK.Config(
                    backend = backend,
                    icdManifestGuestPath = selected.guestPath,
                    innerVulkanLibraryGuestPath = inner,
                    guestLibraryDirs = LIB_DIRS.filter { File(rootfs, it.removePrefix("/")).isDirectory } +
                        (if (selectedKind == DriverKind.INJECTED) listOf("/" + INJECTED_DIR) else emptyList()),
                ),
            )
        }.getOrDefault(emptyMap())

        if (!wrapper || env.isEmpty()) return env
        val out = LinkedHashMap(env)
        if (!wsiDmabuf && "MESA_VK_WSI_DEBUG" !in out) out["MESA_VK_WSI_DEBUG"] = "sw"

        if (bionicWrapper != null) {
            out["PLASMAVK_BIONIC_LIB"] = bionicWrapper

            val injectedDirs = if (driverPreference != "injected_bionic") emptyList() else listOfNotNull(
                "/$BIONIC_HAL_DIR".takeIf { halFiles(rootfs).isNotEmpty() },
                "/$BIONIC_DRV_DIR".takeIf { File(rootfs, BIONIC_DRV_DIR).isDirectory },
            )
            val bridge = AndroidVulkanBridge.env(rootfs)
            out.putAll(bridge)
            out["HYBRIS_LD_LIBRARY_PATH"] = (injectedDirs + BionicDeps.GUEST_DIR + androidHalDirs()).joinToString(":")
        }
        return out
    }

    private fun softwareEnv(rootfs: File): Map<String, String> {
        val lvp = availableManifests(rootfs).firstOrNull { kind(it) == DriverKind.SOFTWARE } ?: return emptyMap()
        return linkedMapOf(
            "VK_DRIVER_FILES" to lvp.guestPath,
            "VK_ICD_FILENAMES" to lvp.guestPath,
            "MESA_VK_WSI_DEBUG" to "sw",
        )
    }

    private fun androidHalDirs(): List<String> = ANDROID_HAL_DIRS.filter { File(it).isDirectory }

    fun androidVulkanLibs(): List<String> =
        (ANDROID_HAL_DIRS + "/system/lib64" + "/vendor/lib64").flatMap { dir ->
            File(dir).listFiles { f -> f.isFile && f.name.endsWith(".so") && (f.name.startsWith("vulkan.") || f.name.startsWith("libvulkan")) }
                ?.map { it.path }.orEmpty()
        }

    private fun getprop(key: String): String = runCatching {
        Runtime.getRuntime().exec(arrayOf("getprop", key)).inputStream.bufferedReader().readText().trim()
    }.getOrDefault("")

    fun androidVulkanSummary(): String {
        val libs = androidVulkanLibs()
        val hw = listOf("ro.hardware.vulkan", "ro.hardware", "ro.board.platform").map(::getprop).filter { it.isNotBlank() }.distinct()
        val gpu = if (hw.isEmpty()) "" else " (${hw.joinToString("/")})"
        return if (libs.isEmpty()) "nenhum vulkan.*.so legível em /vendor, /system ou /odm$gpu: pasta bloqueada ou GPU sem Vulkan nativo"
        else libs.joinToString(", ") { it.substringAfterLast('/') } + gpu
    }

    data class InjectResult(val ok: Boolean, val message: String)

    private fun halFiles(rootfs: File): List<File> =
        File(rootfs, BIONIC_HAL_DIR).listFiles { f -> f.isFile && f.name.startsWith("vulkan.") && f.name.endsWith(".so") }?.toList().orEmpty()

    private fun injectedBionicIcd(rootfs: File): String? =
        if (driverPreference != "injected_bionic") null
        else File(rootfs, "$BIONIC_DRV_DIR/icd.path").takeIf { it.isFile }?.readText()?.trim()?.takeIf { it.isNotEmpty() }

    private fun bionicInjected(rootfs: File): Boolean =
        halFiles(rootfs).isNotEmpty() || File(rootfs, "$BIONIC_DRV_DIR/icd.path").isFile

    fun extraBinds(rootfs: File): List<Pair<File, String>> {
        if (!enabled) return emptyList()
        val out = ArrayList<Pair<File, String>>()
        out += AndroidVulkanBridge.binds(rootfs)

        val drm = File(rootfs, "${BionicDeps.GUEST_DIR.removePrefix("/")}/libdrm.so")
        if (drm.isFile) for (d in listOf("/vendor/lib64", "/system/lib64")) {
            if (!File(d, "libdrm.so").exists()) out += drm to "$d/libdrm.so"
        }
        if (driverPreference == "injected_bionic")
            halFiles(rootfs).forEach { f -> for (d in listOf("/vendor/lib64/hw", "/odm/lib64/hw")) out += f to "$d/${f.name}" }
        return out
    }

    private fun halVariants(): List<String> =
        (listOf("ro.hardware.vulkan", "ro.hardware", "ro.product.board", "ro.board.platform", "ro.arch").map(::getprop)
            .filter { it.isNotBlank() && it.all { c -> c.isLetterOrDigit() || c == '_' || c == '-' } } + "default").distinct()

    fun installInjectedBionic(rootfs: File, files: List<File>): InjectResult {
        val libs = files.filter { it.name.endsWith(".so") || it.name.contains(".so.") }
        if (libs.isEmpty()) return InjectResult(false, "Selecione o .so do driver Vulkan Bionic (e as libs dele, se houver)")
        val hal = libs.firstOrNull { BionicDeps.exportsSymbol(it, "HMI") }
        val icd = if (hal != null) null else libs.firstOrNull { BionicDeps.exportsSymbol(it, "vk_icdGetInstanceProcAddr") }
        val main = hal ?: icd ?: return InjectResult(false, "Nenhum .so exporta HMI (HAL Vulkan) nem vk_icdGetInstanceProcAddr (ICD)")
        if (BionicDeps.neededOf(main) == null) return InjectResult(false, "${main.name} não é um ELF aarch64 válido")
        if (!BionicDeps.isBionicElf(main)) return InjectResult(false, "Driver recusado: ${main.name} é glibc. Só drivers Bionic (Android) são aceitos.")
        File(rootfs, BIONIC_HAL_DIR).deleteRecursively()
        File(rootfs, BIONIC_DRV_DIR).deleteRecursively()
        val dir = File(rootfs, if (hal != null) BIONIC_HAL_DIR else BIONIC_DRV_DIR).apply { mkdirs() }
        runCatching {
            for (f in libs) if (f != main) f.copyTo(File(dir, f.name), overwrite = true)
            if (hal != null) {
                val variants = halVariants()
                val first = File(dir, "vulkan.${variants.first()}.so")
                main.copyTo(first, overwrite = true)
                for (v in variants.drop(1)) {
                    val link = File(dir, "vulkan.$v.so")
                    runCatching { java.nio.file.Files.createLink(link.toPath(), first.toPath()) }.onFailure { first.copyTo(link, overwrite = true) }
                }
            } else {
                main.copyTo(File(dir, main.name), overwrite = true)
                File(dir, "icd.path").writeText("/$BIONIC_DRV_DIR/${main.name}\n")
            }
            dir.listFiles()?.forEach { it.setReadable(true, false); it.setExecutable(true, false) }
        }.onFailure { return InjectResult(false, "Falha ao copiar o driver: ${it.message}") }
        val missing = BionicDeps.missingNeeded(main, listOf(dir, File(rootfs, "usr/lib/plasmavk/bionic")))
        val kind = if (hal != null) "HAL (HMI) no lugar de vulkan.${halVariants().first()}.so" else "ICD Bionic"
        val warn = if (missing.isEmpty()) "" else " Possivelmente ausentes: ${missing.joinToString()}"
        return InjectResult(true, "Driver ${main.name} instalado como $kind.$warn")
    }

    fun clearInjectedBionic(rootfs: File) {
        File(rootfs, BIONIC_HAL_DIR).deleteRecursively()
        File(rootfs, BIONIC_DRV_DIR).deleteRecursively()
    }

    fun invalidateProbe(rootfs: File) { runCatching { probeFile(rootfs).delete() } }

    private fun probeFile(rootfs: File) = File(rootfs, "usr/lib/plasmavk/.vk-probe-v4")
    private fun fileSig(rootfs: File, guest: String?): String {
        val f = guest?.let { File(rootfs, it.removePrefix("/")) }
        return if (f != null && f.isFile) "${f.length()}-${f.lastModified()}" else "-"
    }

    private fun probeStamp(rootfs: File, m: Manifest): String =
        "${m.file.name}:${fileSig(rootfs, m.library)}:${fileSig(rootfs, detectBionicWrapper(rootfs))}:${fileSig(rootfs, detectInnerLibrary(rootfs))}"

    private fun probeState(rootfs: File, m: Manifest): Boolean? {
        val text = runCatching { probeFile(rootfs).readText().trim() }.getOrNull() ?: return null
        val (state, stamp) = text.split(' ', limit = 2).let { it[0] to it.getOrElse(1) { "" } }
        if (stamp != probeStamp(rootfs, m)) return null
        return when (state) { "ok" -> true; "fail" -> false; else -> null }
    }

    fun probe(p: ProotPaths) {
        if (!enabled || !p.rootfs.isDirectory) return
        val m = preferredManifest(p.rootfs, includeSoftware = false) ?: return
        if (kind(m) != DriverKind.HYBRIS && kind(m) != DriverKind.WRAPPER) return
        if (probeState(p.rootfs, m) != null) return
        val lib = m.library ?: return

        val script = "if ldd '$lib' 2>&1 | grep -q 'not found'; then echo MISSING-LIBS; exit 3; fi; " +
            "command -v vulkaninfo >/dev/null 2>&1 || exit 0; " +
            "o=\$(vulkaninfo 2>&1); echo \"\$o\" | grep -q deviceName && echo \"\$o\" | grep -q VK_KHR_wayland_surface"
        val env = hardwareEnv(p.rootfs, m)
        var output = ""
        val ok = runCatching {
            val cmd = ProotLauncher.command(p, p.rootfs, listOf("/bin/sh", "-c", script), guestEnv = env, useGuestEnvWrapper = true)
            val pb = ProcessBuilder(cmd).directory(p.root).redirectErrorStream(true)
            pb.environment().apply { clear(); putAll(ProotLauncher.hostEnv(p)) }
            val proc = pb.start(); runCatching { proc.outputStream.close() }

            val reader = Thread { output = runCatching { proc.inputStream.bufferedReader().readText().takeLast(2000) }.getOrDefault("") }
                .apply { isDaemon = true; start() }
            val done = proc.waitFor(25, java.util.concurrent.TimeUnit.SECONDS)
            if (!done) { proc.destroyForcibly(); reader.join(500); false } else { reader.join(1000); proc.exitValue() == 0 }
        }.onFailure { android.util.Log.w("PlasmaVK", "probe: exceção", it) }.getOrDefault(false)
        if (!ok && output.isNotBlank()) android.util.Log.w("PlasmaVK", "probe falhou:\n$output")
        runCatching {
            probeFile(p.rootfs).apply { parentFile?.mkdirs() }
                .writeText((if (ok) "ok " else "fail ") + probeStamp(p.rootfs, m) + "\n")
        }
        android.util.Log.i("PlasmaVK", "probe hybris ICD: " + if (ok) "ok" else "falhou -> fallback lavapipe")
    }

    fun status(rootfs: File): String {
        if (!enabled) return "desligado"
        val all = manifests(rootfs)
        val usableHardware = preferredManifest(rootfs, includeSoftware = false)
        if (usableHardware != null) {
            return when (kind(usableHardware)) {
                DriverKind.HYBRIS -> "ICD libhybris encontrado; aguardando vulkaninfo"
                DriverKind.PANVK -> "ICD PanVK encontrado; aguardando teste no Mali/kbase"
                DriverKind.TURNIP -> "ICD Turnip encontrado; aguardando teste no Adreno/KGSL"
                DriverKind.WRAPPER -> BionicDeps.check(rootfs).let { r ->
                    if (r.ok) "wrapper e ICD interno encontrados; aguardando vulkaninfo" else "wrapper Bionic incompleto: ${r.describe()}"
                }
                DriverKind.OTHER_HARDWARE -> "ICD de hardware encontrado; aguardando vulkaninfo"
                DriverKind.INJECTED -> "driver injetado selecionado; aguardando vulkaninfo"
                DriverKind.SOFTWARE -> "somente software"
                DriverKind.UNKNOWN -> "ICD não classificado"
            }
        }
        if (driverPreference == "injected_bionic" && !bionicInjected(rootfs)) {
            return "nenhum driver Bionic importado (Configurações → Compositor e GPU → Importar Driver Vulkan)"
        }
        wantedKind()?.let { w ->
            return if (w == DriverKind.INJECTED) "nenhum driver injetado importado (Configurações → Compositor e GPU → Importar Driver Vulkan)"
            else "${w.label} escolhido, mas ausente do runtime embutido; compile com scripts/build-plasmavk-mesa.sh e reempacote"
        }
        val usableSoftware = preferredManifest(rootfs, includeSoftware = true)
        if (usableSoftware != null && kind(usableSoftware) == DriverKind.SOFTWARE) {
            return "somente lavapipe/LLVM (software); GPU real não detectada"
        }
        if (all.any { kind(it) == DriverKind.WRAPPER }) {
            return "wrapper instalado, mas falta um ICD glibc válido no multiarch"
        }
        val unknown = all.firstOrNull { kind(it) == DriverKind.UNKNOWN && resolves(rootfs, it.library) }
        if (unknown != null) return "ICD presente, mas driver não reconhecido: ${unknown.file.name}"
        return if (all.isNotEmpty()) "manifesto ICD sem biblioteca glibc válida no multiarch" else "runtime não instalado"
    }

    private const val ANGLE_FLAG = "var/lib/plasma/incandescent/angle"

    fun angleEnabled(rootfs: File): Boolean =
        runCatching { File(rootfs, ANGLE_FLAG).readText().trim() in setOf("1", "on", "true", "yes") }.getOrDefault(false)

    fun setAngle(rootfs: File, on: Boolean) {
        runCatching {
            val f = File(rootfs, ANGLE_FLAG)
            f.parentFile?.mkdirs()
            f.writeText(if (on) "1\n" else "0\n")
            File(rootfs, "var/lib/plasma/gl-backend").delete()
        }
    }

    fun angleLibsPresent(rootfs: File): Boolean =
        File(rootfs, "usr/lib/plasmavk/bionic/angle/libEGL_angle.so").isFile && File(rootfs, "usr/lib/plasmavk/bionic/angle/libGLESv2_angle.so").isFile

    fun describePlasmaGl(rootfs: File): List<Pair<String, String>> {
        val inc = File(rootfs, "usr/lib/plasmavk/bionic/incandescent/libincandescent.so").isFile
        val angle = angleLibsPresent(rootfs)
        val on = angleEnabled(rootfs)
        val status = runCatching { File(rootfs, "var/lib/plasma/incandescent/status").readLines().firstOrNull { it.isNotBlank() }?.trim() }.getOrNull()
        return listOf(
            "PlasmaGL" to if (inc) "Incandescent instalado (comando plasma-gl no Debian)" else "runtime do PlasmaGL não encontrado no Debian",
            "ANGLE" to when {
                on && angle -> "Ligado (GLES → Vulkan)"
                on -> "Ligado, mas libEGL_angle.so/libGLESv2_angle.so ausentes"
                angle && !File(rootfs, ANGLE_FLAG).isFile -> "Automático (GLES nativo; ANGLE só se o nativo falhar)"
                angle -> "Desligado (usa o GLES nativo do aparelho)"
                else -> "Desligado (não instalado; não é necessário)"
            },
            "Último teste" to (status ?: "—"),
        )
    }

    fun describe(rootfs: File): List<Pair<String, String>> {
        val selected = preferredManifest(rootfs, includeSoftware = true)
        val inner = detectInnerLibrary(rootfs)
        val libraries = LIB_DIRS.filter { File(rootfs, it.removePrefix("/")).isDirectory }
        val selectedKind = selected?.let(::kind)
        val isHardware = selectedKind != null && selectedKind != DriverKind.SOFTWARE
        return listOf(
            "Estado" to status(rootfs),
            "Manifesto ICD" to (selected?.guestPath ?: "—"),
            "Biblioteca ICD" to (selected?.library ?: "—"),
            "Aceleração" to if (isHardware) "ICD de hardware selecionado (aguardando vulkaninfo)" else "não confirmada",
            "Driver escolhido" to when (driverPreference) { "panvk" -> "PanVK"; "turnip" -> "Turnip"; "injected" -> "Injetado (glibc)"; "injected_bionic" -> "Injetado (Bionic)"; else -> "Automático" },
            "Vulkan do aparelho" to androidVulkanSummary(),
            "Ponte libvulkan Android" to AndroidVulkanBridge.describe(rootfs),
            "Biblioteca interna" to (inner ?: "—"),
            "Pastas de libs" to libraries.joinToString(", ").ifEmpty { "—" },
            "Variáveis" to if (detectManifest(rootfs) != null) "VK_DRIVER_FILES, VK_ICD_FILENAMES, LD_LIBRARY_PATH" else "—",
        )
    }
}
