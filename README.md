# PlasmaDroid

PlasmaDroid is a desktop environment for Android, built with Kotlin, Jetpack Compose and the NDK. The idea is simple: plug in a phone or tablet, open a KDE-style desktop, and run real Android apps and Linux programs in windows you can move, resize and tile.

It started as a personal experiment to see how far a phone could go as a workstation. It is still early and rough in places, but the core pieces already work together.

## What is inside

The desktop shell has a panel, a Kickoff-style launcher, a task list, a pager for virtual desktops, a system tray with network, Bluetooth, volume and brightness controls, and a few widgets. Android apps open in their own windows. Linux apps run inside a Debian environment, and their windows are composited next to the Android ones.

Linux support comes from PRoot, which gives the guest a Debian root filesystem without needing root on the device. On top of that, PlasmaVK handles the graphics side, so Vulkan and OpenGL calls from guest apps can reach the phone's GPU.

## Features

- [x] Debian PRoot environment
- [x] Plasma package manager (`plasma install <app>`, which installs a package and applies the patches it needs)
- [x] Wayland host with a compositor that brings Linux windows onto the desktop
- [x] Android windowed apps
- [x] Shizuku input service for mouse, keyboard and touch input in any app window

### Hardware acceleration

| API    | Status | Notes                                                                 |
|--------|--------|-----------------------------------------------------------------------|
| OpenGL | ✅ Yes  | Host compositing uses EGL and GLES. The Incandescent layer adds GL support for guest apps. |
| Vulkan | ✅ Yes  | Guest apps reach the GPU through PlasmaVK, with Wayland WSI support. |

## Getting started

You need Android SDK 35, NDK 29 and CMake 3.22.1.

```
sdkmanager "platforms;android-35" "build-tools;35.0.0" "cmake;3.22.1" "ndk;29.0.14206865"
echo "sdk.dir=$ANDROID_HOME" > local.properties
./build.sh
```

If the Maven `aapt2` fails on your machine, set `android.aapt2FromMavenOverride` in `gradle.properties`.

On first launch, the setup screen asks for file access, notification access and Shizuku. Shizuku is what lets PlasmaDroid send input to windows from any app, so it is worth setting up. Start it, then authorize the privileged service.

## Sharing the desktop

Open Settings and choose "Share desktop" to stream the screen. On a computer you can view it with:

```
python3 tools/plasma_receiver.py IP PIN | ffplay -fflags nobuffer -f h264 -i -
```

## Status

This is a work in progress. The latest changes have not been compiled or tested on a device yet, and it has not been tried on every GPU, so expect rough edges. If something breaks on your hardware, an issue with your device model and the Android version helps a lot.

## Credits

- **PlasmaVK** (`proot/` module): based on the **Proot** project.
- **Incandescent** (`incandescent/`): based on the **Mobile Glues** project.

## License

PlasmaDroid is released under GPL-3.0-or-later. See `LICENSE`. The Shizuku API is Apache-2.0.
