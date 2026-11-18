
package org.plasma.proot

import java.io.File

object PlasmaVK {
    enum class Backend(val wireName: String) {
        DIRECT_ICD("direct-icd"),

        SYSTEM_BIONIC_LIBHYBRIS("system-bionic-libhybris"),

        MESA_TURNIP("mesa-turnip"),

        MESA_PANVK_KBASE("mesa-panvk-kbase"),

        LAVAPIPE("lavapipe"),
    }

    data class Config(

        val backend: Backend = Backend.SYSTEM_BIONIC_LIBHYBRIS,

        val icdManifestGuestPath: String? = null,

        val innerVulkanLibraryGuestPath: String? = null,

        val guestLibraryDirs: List<String> = listOf("/usr/lib/plasmavk"),

        val enableIncandescent: Boolean = false,

        val enableAngle: Boolean = false,

        val incandescentLibGuestPath: String = "/usr/lib/plasmavk/bionic/incandescent/libincandescent.so",

        val angleDirGuestPath: String = "/usr/lib/plasmavk/bionic/angle",

        val incandescentDataGuestPath: String = "/var/lib/plasma/incandescent",
        val debug: Boolean = false,
        val setLegacyIcdVariable: Boolean = true,

        val envExecutableGuestPath: String = "/usr/bin/env",

        val extraEnvironment: Map<String, String> = emptyMap(),
    )

    data class ValidationResult(
        val ready: Boolean,
        val errors: List<String>,
        val warnings: List<String>,
    )

    fun buildGuestEnvironment(
        config: Config,
        baseGuestEnvironment: Map<String, String> = emptyMap(),
    ): Map<String, String> {
        val env = LinkedHashMap<String, String>()
        env.putAll(baseGuestEnvironment)

        val existingLd = env["LD_LIBRARY_PATH"].orEmpty()
            .split(':')
            .filter { it.isNotBlank() }
        val guestLd = (config.guestLibraryDirs + existingLd)
            .map { normalizeGuestAbsolutePath(it) }
            .distinct()
        if (guestLd.isNotEmpty()) {
            env["LD_LIBRARY_PATH"] = guestLd.joinToString(":")
        }

        env["PLASMAVK_BACKEND"] = config.backend.wireName
        env["PLASMAVK_ENABLE"] = "1"

        config.icdManifestGuestPath?.let { manifest ->
            val guestManifest = normalizeGuestAbsolutePath(manifest)
            env["VK_DRIVER_FILES"] = guestManifest
            if (config.setLegacyIcdVariable) {

                env["VK_ICD_FILENAMES"] = guestManifest
            }
        }

        config.innerVulkanLibraryGuestPath?.let { library ->
            env["PLASMAVK_VULKAN_LOADER"] = normalizeGuestAbsolutePath(library)

            env["PLASMAVK_STRICT_VULKAN_LOADER"] = "1"
        }

        if (config.debug) {
            env["VK_LOADER_DEBUG"] = "all"
            env["PLASMAVK_DEBUG"] = "1"
        }
        if (config.enableIncandescent) {
            val lib = normalizeGuestAbsolutePath(config.incandescentLibGuestPath)
            val data = normalizeGuestAbsolutePath(config.incandescentDataGuestPath)
            env["LIBEGL"] = lib
            env["LIBGLESV2"] = lib

            env["INC_ANGLE_DIR"] = if (config.enableAngle) normalizeGuestAbsolutePath(config.angleDirGuestPath) else ""
            env["INC_ANGLE"] = if (config.enableAngle) "1" else "0"
            env["INC_DIR_PATH"] = data
            env["INC_PLUGIN_STATUS"] = "$data/status"
            env["INC_GPU_REPORT"] = "$data/gpu"
        }

        env.putAll(config.extraEnvironment)
        env.forEach { (key, value) ->
            require(ENV_KEY.matches(key)) { "Invalid environment variable name: $key" }
            require('\u0000' !in value) { "Environment value for $key contains NUL" }
        }
        return env
    }

    fun buildCommand(
        prootBinary: File,
        rootfs: File,
        guestCommand: List<String>,
        config: Config,
        guestWorkingDirectory: String = "/",
        baseGuestEnvironment: Map<String, String> = emptyMap(),
    ): List<String> {
        require(prootBinary.isFile) { "PRoot binary not found: ${prootBinary.absolutePath}" }
        require(prootBinary.canExecute()) { "PRoot binary is not executable: ${prootBinary.absolutePath}" }
        require(rootfs.isDirectory) { "Rootfs directory not found: ${rootfs.absolutePath}" }
        require(guestCommand.isNotEmpty()) { "guestCommand cannot be empty" }
        require(guestWorkingDirectory.startsWith('/')) {
            "guestWorkingDirectory must be an absolute guest path"
        }
        require(config.envExecutableGuestPath.startsWith('/')) {
            "envExecutableGuestPath must be an absolute guest path"
        }

        val env = buildGuestEnvironment(config, baseGuestEnvironment)
        val argv = mutableListOf(
            prootBinary.absolutePath,
            "-r", rootfs.absolutePath,
            "-w", normalizeGuestAbsolutePath(guestWorkingDirectory),
            config.envExecutableGuestPath,
        )
        env.toSortedMap().forEach { (key, value) -> argv += "$key=$value" }
        argv += guestCommand
        return argv
    }

    fun validateRootfs(rootfs: File, config: Config): ValidationResult {
        val errors = mutableListOf<String>()
        val warnings = mutableListOf<String>()

        if (!rootfs.isDirectory) {
            errors += "Rootfs directory does not exist: ${rootfs.absolutePath}"
            return ValidationResult(false, errors, warnings)
        }

        val envBinary = File(rootfs, config.envExecutableGuestPath.removePrefix("/"))
        if (!envBinary.exists()) {
            errors += "Guest env executable not found: ${config.envExecutableGuestPath}"
        }

        val manifest = config.icdManifestGuestPath
        if (manifest.isNullOrBlank()) {
            warnings += "No ICD manifest was specified; the guest loader will use its default discovery paths."
        } else {
            val manifestFile = File(rootfs, normalizeGuestAbsolutePath(manifest).removePrefix("/"))
            if (!manifestFile.isFile) {
                errors += "ICD manifest not found inside rootfs: ${normalizeGuestAbsolutePath(manifest)}"
            } else {
                val body = runCatching { manifestFile.readText() }.getOrNull().orEmpty()
                if (!body.contains("library_path")) {
                    warnings += "ICD manifest does not appear to contain a library_path field."
                }
            }
        }

        config.innerVulkanLibraryGuestPath?.let { library ->
            val normalized = normalizeGuestAbsolutePath(library)
            val libraryFile = File(rootfs, normalized.removePrefix("/"))
            if (!libraryFile.isFile) {
                errors += "Inner Vulkan ICD/library not found inside rootfs: $normalized"
            }
        }

        if (config.innerVulkanLibraryGuestPath != null && config.icdManifestGuestPath == null) {
            warnings += "An inner Vulkan ICD path is set without an explicit outer ICD manifest; ensure the patched Mesa wrapper manifest is installed and selected."
        }
        if (config.backend == Backend.SYSTEM_BIONIC_LIBHYBRIS && config.guestLibraryDirs.isEmpty()) {
            warnings += "No explicit guest runtime library directory was configured for libhybris."
        }
        config.guestLibraryDirs.forEach { path ->
            val dir = File(rootfs, normalizeGuestAbsolutePath(path).removePrefix("/"))
            if (!dir.isDirectory) {
                warnings += "Configured guest library directory is not present yet: ${normalizeGuestAbsolutePath(path)}"
            }
        }

        return ValidationResult(errors.isEmpty(), errors, warnings)
    }

    private fun normalizeGuestAbsolutePath(path: String): String {
        require(path.startsWith('/')) { "Expected an absolute guest path: $path" }
        require('\u0000' !in path) { "Path contains NUL" }
        val parts = path.split('/').filter { it.isNotEmpty() && it != "." }
        require(parts.none { it == ".." }) { "Parent path segments are not allowed: $path" }
        return "/" + parts.joinToString("/")
    }

    private val ENV_KEY = Regex("[A-Za-z_][A-Za-z0-9_]*")
}
