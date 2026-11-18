
package org.plasma.proot

import android.system.Os
import java.io.File
import java.nio.file.Files

object ProotLauncher {
    const val GUEST_PATH = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"

    @Synchronized fun prepare(p: ProotPaths) {
        p.root.mkdirs(); p.tmp.mkdirs(); p.libLinks.mkdirs()

        val link = File(p.libLinks, "libtalloc.so.2")
        val wanted = p.talloc.path
        val current = runCatching { Files.readSymbolicLink(link.toPath()).toString() }.getOrNull()
        if (current != wanted || !link.exists()) {
            runCatching { Files.deleteIfExists(link.toPath()) }
            runCatching { Os.symlink(wanted, link.path) }
                .onFailure { runCatching { if (p.talloc.exists()) p.talloc.copyTo(link, overwrite = true) } }
        }
        for (f in listOf(p.proot, p.loader, p.talloc, p.shmem)) if (f.exists() && !f.canExecute()) f.setExecutable(true, false)
    }

    fun hostEnv(p: ProotPaths, forceNoSeccomp: Boolean = false, honorFlag: Boolean = true): Map<String, String> {
        val env = linkedMapOf(
            "PROOT_LOADER" to p.loader.path,
            "PROOT_TMP_DIR" to p.tmp.path,
            "LD_LIBRARY_PATH" to p.libLinks.path + ":" + p.nativeLibDir.path,
            "TMPDIR" to p.tmp.path,
            "HOME" to p.root.path,
            "PATH" to GUEST_PATH,

            "PROOT_IGNORE_MISSING_BINDINGS" to "1",
        )
        if (forceNoSeccomp || (honorFlag && p.noSeccompFlag.exists())) env["PROOT_NO_SECCOMP"] = "1"
        return env
    }

    fun command(
        p: ProotPaths,
        rootfs: File,
        args: List<String>,
        binds: List<Pair<File, String>> = emptyList(),
        guestEnv: Map<String, String> = emptyMap(),
        workdir: String = "/root",
        useGuestEnvWrapper: Boolean = true,
        verbose: Boolean = false,
    ): List<String> {
        val cmd = ArrayList<String>()
        cmd += p.proot.path
        if (verbose) cmd += listOf("-v", "1")

        cmd += listOf("--kill-on-exit", "-0")
        if (p.link2symlinkFlag.exists()) cmd += "--link2symlink"
        if (p.fexTransparentFlag.exists() && File(rootfs, "usr/local/lib/plasma/fex/.ready").exists() &&
            File(rootfs, "usr/local/bin/fex-run").exists()) cmd += listOf("-q", "/usr/local/bin/fex-run")
        cmd += listOf("-r", rootfs.path, "-b", "/dev", "-b", "/proc", "-b", "/sys")

        if (File("/sdcard").exists()) cmd += listOf("-b", "/sdcard:/sdcard")

        File(rootfs, "usr/local/lib/plasma/native").mkdirs()
        cmd += listOf("-b", "${p.nativeLibDir.path}:/usr/local/lib/plasma/native")
        if (p.fexHost.exists()) cmd += listOf("-b", "${p.fexHost.path}:/usr/local/lib/plasma/fex-host")

        val androidSystemPaths = listOf("/system", "/vendor", "/apex", "/odm", "/product", "/system_ext")
        val allBinds = ArrayList(binds)
        for (path in androidSystemPaths) {
            val host = File(path)
            if (!host.exists()) continue
            val guestTarget = File(rootfs, path.removePrefix("/"))
            if (!guestTarget.exists()) guestTarget.mkdirs()
            if (allBinds.none { it.second == path }) allBinds += host to path
        }

        allBinds += PlasmaVKSupport.extraBinds(rootfs)

        for ((hostPath, name) in listOf("/dev/dri" to "dev-dri", "/sys/class/drm" to "sys-class-drm")) {
            if (File(hostPath).isDirectory || allBinds.any { it.second == hostPath }) continue
            val fake = File(rootfs.parentFile ?: rootfs, name)
            if (fake.isDirectory || fake.mkdirs()) allBinds += fake to hostPath
        }
        for ((host, guest) in allBinds) cmd += listOf("-b", "${host.path}:$guest")
        cmd += listOf("-w", workdir)
        if (useGuestEnvWrapper) {
            cmd += listOf("/usr/bin/env", "-i", "HOME=/root", "PATH=$GUEST_PATH", "TERM=xterm-256color", "LANG=C.UTF-8",
                "COLORTERM=truecolor")

            val extra = LinkedHashMap<String, String>(PlasmaVKSupport.guestEnv(rootfs))
            for ((k, v) in SysVShm.guestEnv(p, rootfs)) extra[k] = if (k == "LD_PRELOAD") SysVShm.mergePreload(extra[k], v) else v
            for ((k, v) in guestEnv) extra[k] = if (k == "LD_PRELOAD") SysVShm.mergePreload(extra[k], v) else v
            for ((k, v) in extra) cmd += "$k=$v"
        }
        cmd += args
        return cmd
    }
}
