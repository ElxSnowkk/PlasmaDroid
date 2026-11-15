// Parte do PlasmaDroid — módulo PlasmaVK.
// Créditos: baseado no projeto Proot.

package org.plasma.proot

import java.io.File

class ProotPaths(val filesDir: File, val nativeLibDir: File) {
    val root = File(filesDir, "proot")
    private val base = File(root, "debian")
    val rootfs = File(base, "rootfs")
    val staging = File(base, "rootfs-staging")
    val previous = File(base, "rootfs-old")
    val downloads = File(root, "downloads")
    val tmp = File(root, "tmp")

    val libLinks = File(root, "lib")
    val marker = File(base, ".rootfs-installed")
    val setupDone = File(base, ".setup-complete")
    val noSeccompFlag = File(base, ".no-seccomp")
    fun stepMarker(id: String) = File(base, ".step-$id")

    val proot = File(nativeLibDir, "libproot.so")
    val loader = File(nativeLibDir, "libproot-loader.so")
    val talloc = File(nativeLibDir, "libtalloc.so")
    val shmem = File(nativeLibDir, "libandroid-shmem.so")

    val sysvshmServer = File(nativeLibDir, "libsysvshm-server.so")

    fun missingBinaries(): List<String> =
        listOf(proot, loader, talloc).filter { !it.exists() }.map { it.name }
}
