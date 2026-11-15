# PlasmaVK — Vulkan bridge integration kit for ASDroid

> Documentação em português: [`README.pt-BR.md`](README.pt-BR.md).

**Purpose:** make a Linux/glibc rootfs launched by ASDroid/PRoot use the Android device's Vulkan driver without requiring Termux, while keeping the normal glibc environment intact.

This repository is an integration kit, not a prebuilt GPU driver. It contains an Android-app launch API, guest diagnostics, documentation, and a small patch for the supplied Mesa `wrapper` ICD. The real cross-ABI runtime (`libhybris`) and the device-specific Vulkan stack must be built and tested for the target Android build and GPU.

## Design choice

The primary mode is `SYSTEM_BIONIC_LIBHYBRIS`: use a glibc-side Vulkan loader and a libhybris Vulkan ICD/adapter to reach the native Bionic vendor driver from a glibc process. The Android app remains a native Android app; it starts PRoot directly and does not invoke Termux.

The uploaded Mesa tree is Mesa **24.2.5** and its `src/vulkan/wrapper` is itself a Vulkan ICD (it generates `wrapper_icd.*.json`). It forwards Vulkan calls after loading another Vulkan implementation; it does **not** by itself bridge the Bionic/glibc ABI boundary. The included source patch adds an explicit inner-library path and raw-ICD dispatch support, and removes the need to infer a hook path from Termux. Important: this wrapper source also depends on Android headers/APIs such as `AHardwareBuffer`; this small patch does not port the whole wrapper to glibc. Use the direct libhybris ICD mode first. Wrapper stacking is an experimental follow-up that needs an appropriate glibc build and runtime validation.

## Contents

- `app-api/PlasmaVK.kt` — Kotlin API to add Vulkan environment to a command **inside** the guest, without polluting the Bionic PRoot process environment.
- `scripts/plasmavk-doctor.sh` — diagnostics to run inside the Linux rootfs.
- `patches/0001-mesa-wrapper-configurable-loader.patch` — patch for the supplied Mesa wrapper, including direct ICD interface support.
- `docs/ARCHITECTURE.md` — architecture and limitations.
- `docs/APP_INTEGRATION.md` — how ASDroid can call the API.
- `docs/BUILD_AND_TEST.md` — build/verification steps and acceptance criteria.

## Quick use from ASDroid

```kotlin
val config = PlasmaVK.Config(
    backend = PlasmaVK.Backend.SYSTEM_BIONIC_LIBHYBRIS,
    icdManifestGuestPath = "/usr/share/vulkan/icd.d/<manifest-installed-by-libhybris>.json",
    guestLibraryDirs = listOf("/usr/lib/plasmavk", "/usr/lib/aarch64-linux-gnu/libhybris"),
    enableZink = true, // only if OpenGL should be translated through Vulkan
    debug = false,
)

val argv = PlasmaVK.buildCommand(
    prootBinary = File(filesDir, "bin/proot"),
    rootfs = File(filesDir, "rootfs/debian"),
    guestCommand = listOf("/usr/bin/vulkaninfo", "--summary"),
    guestWorkingDirectory = "/",
    config = config,
)

// Start argv with ProcessBuilder. Do NOT put the guest LD_LIBRARY_PATH into
// ProcessBuilder.environment(): it would affect the native/Bionic PRoot process.
val process = ProcessBuilder(argv).redirectErrorStream(true).start()
```

Replace the manifest placeholder with the actual JSON manifest created by the libhybris Vulkan ICD package inside the rootfs. If using the direct libhybris ICD, leave `innerVulkanLibraryGuestPath` unset. If using the patched Mesa wrapper *as the outer ICD*, set `icdManifestGuestPath` to the generated `wrapper_icd.<arch>.json` and `innerVulkanLibraryGuestPath` to the full guest path of the glibc-loadable raw libhybris ICD (for example `/usr/lib/plasmavk/<glibc-loadable-raw-icd>.so`). Never set the inner path to Android's raw `/system/lib64/libvulkan.so` from a glibc guest.

## Important

A successful build is not equivalent to a working renderer. Validate in this order: guest Vulkan loader → ICD enumeration → physical-device enumeration → device creation → offscreen rendering → presentation/swapchain. A device that enumerates successfully may still fail during window-system integration, memory sharing, synchronization, or surface resize.
