# Third-party notices

Piperella Wayland Compositor is licensed under the **GNU General Public License
version 3** — see [`LICENSE`](./LICENSE). Copyright © 2026 Yassine Koubry.

This file lists the third-party components redistributed in this repository,
their licenses, and their copyright holders. Full license texts are in
[`licenses/`](./licenses/).

Everything below is compatible with GPL-3.0 as combined here. Apache-2.0 is
one-way compatible with GPL-3.0 (Apache-2.0 code may be included in a GPL-3.0
work); MIT, BSD-2-Clause, BSD-3-Clause and zlib are permissive and impose only
notice requirements. The fonts are separate, non-linked data files under their
own terms.

---

## Code this project is derived from

### wlroots-android-bridge — GPL-3.0

The AIDL binder input path is derived from
[Xtr126/wlroots-android-bridge](https://github.com/Xtr126/wlroots-android-bridge),
Copyright © 2025 Xtr126 `<k.gunetilleke@gmail.com>`, licensed under the GNU
General Public License version 3.

Derived files:

- `android/src/main/cpp/input-queue.cpp`
- `android/src/main/aidl/com/xtr/tinywl/*.aidl` (7 files)
- `android/src/main/java/com/xtr/tinywl/SurfaceServiceKt.kt`

This is the reason the project as a whole is distributed under GPL-3.0.
See [`LICENSE`](./LICENSE).

---

## Android Open Source Project — Apache-2.0

Copyright © The Android Open Source Project (2007, 2010, 2022, 2023, 2024).
Licensed under the Apache License, Version 2.0 —
[`licenses/Apache-2.0.txt`](./licenses/Apache-2.0.txt).

- `android/src/main/aidl/android/hardware/input/common/*.aidl` (15 files)
- `android/src/main/aidl/com/android/server/inputflinger/*.aidl` (7 files)
- `android/src/main/aidl/android/view/Surface.aidl`
- `struct BitSet64` inside `android/src/main/cpp/input-queue.cpp`, derived from
  `system/core/libutils/include/utils/BitSet.h`
- `examples/vkcube/app/src/main/cpp/upstream/gettime.h`

## Wayland — MIT

Copyright © 2008 Kristian Høgsberg; © 2010–2012 Intel Corporation;
© 2012–2013 Collabora, Ltd.; © 2011 Benjamin Franzke.
[`licenses/MIT.txt`](./licenses/MIT.txt)

- `android/src/main/cpp/prebuilts/arm64-v8a/lib/libwayland-{server,client,cursor,egl}.so`
- `android/src/main/jniLibs/arm64-v8a/libwayland-*.so`
- `android/src/main/cpp/prebuilts/arm64-v8a/include/wayland-*.h`

## libxkbcommon — MIT / X11

Copyright © 1985–1998 The Open Group; © 2008 Dan Nicholson; © 1993 Silicon
Graphics Computer Systems, Inc.; © 2012 Intel Corporation; © 2012 Daniel Stone;
© 2013 Ran Benita; © 1987 Digital Equipment Corporation.
Some notices are the older X11 notice-only form; all are retained in the headers.
[`licenses/MIT.txt`](./licenses/MIT.txt)

- `android/src/main/cpp/prebuilts/arm64-v8a/lib/libxkbcommon.so`
- `android/src/main/jniLibs/arm64-v8a/libxkbcommon.so`
- `android/src/main/cpp/prebuilts/arm64-v8a/include/xkbcommon/*.h`

## libffi 3.4.6 — MIT

Copyright © 1996–2024 Anthony Green; © 1996–2008 Red Hat, Inc.;
© 2009–2012 ARM Ltd. [`licenses/MIT.txt`](./licenses/MIT.txt)

- `android/src/main/cpp/prebuilts/arm64-v8a/lib/libffi.a`
- `android/src/main/cpp/prebuilts/arm64-v8a/include/ffi.h`, `ffitarget.h`

## wayland-protocols — MIT (text-input is X11-style)

Generated protocol sources in `android/src/main/cpp/protocols/` and the
client-side copies under `examples/*/app/src/main/cpp/protocols-client/`.
Copyright notices are retained verbatim in each generated file.
[`licenses/MIT.txt`](./licenses/MIT.txt)

Copyright holders across these files: Kristian Høgsberg; Intel Corporation;
Collabora, Ltd.; Red Hat, Inc.; Samsung Electronics Co., Ltd; Jonas Ådahl;
Simon Ser; Xaver Hugl; The Chromium Authors; Kenny Levinsen; Martin Gräßlin;
Emmanuel Gil Peyrot; Carlos Garnacho; Aleix Pol Gonzalez; Jasper St. Pierre;
Rafael Antognolli; Jan Arne Petersen; Purism SPC.

> `pointer-gestures-protocol.c/.h` carry no copyright block because the
> upstream `pointer-gestures-unstable-v1.xml` has none.

`android/src/main/cpp/protocols/zwp-gamepad-v1.xml` and its generated output are
**not** third-party — they are this project's own protocol, Copyright © 2024
Yassine Koubry, offered under MIT so that clients may implement it freely.

## Weston 9.0 (`simple-egl`) — MIT

Copyright © 2011 Benjamin Franzke; © 2015 Samsung Electronics Co., Ltd;
© 2015 Collabora, Ltd.; © 2008 Tungsten Graphics, Inc.
[`licenses/MIT.txt`](./licenses/MIT.txt)

- `examples/weston-simple-egl/app/src/main/cpp/upstream/`

## Vulkan-Tools `vkcube` (sdk-1.3.250.1) — Apache-2.0

Copyright © 2015–2019 The Khronos Group Inc.; Valve Corporation; LunarG, Inc.
[`licenses/Apache-2.0.txt`](./licenses/Apache-2.0.txt)

- `examples/vkcube/app/src/main/cpp/upstream/`

`lunarg.ppm.h` embeds the LunarG logo, which is additionally a LunarG trademark.

## zstd 1.5.6 and xxHash — BSD-3-Clause

Copyright © Meta Platforms, Inc. and affiliates; Yann Collet. zstd is offered
under BSD-3-Clause **or** GPLv2 at the recipient's option; **this project elects
the BSD-3-Clause option**. [`licenses/BSD-3-Clause.txt`](./licenses/BSD-3-Clause.txt)

- `examples/{SteamTest,SteamTestVk,android-notepad++}/app/src/main/cpp/zstd/`

## stb_truetype v1.26 — public domain / MIT

Copyright © 2017 Sean Barrett. Dual-offered as MIT or public domain
(Unlicense); this project relies on the public-domain option.

- `examples/test-app/app/src/main/cpp/stb_truetype.h`

## AdrenoTools — BSD-2-Clause

Copyright © 2021 Billy Laws. `SPDX-License-Identifier: BSD-2-Clause`.
Redistributed here in **binary** form, which BSD-2-Clause §2 permits provided
this notice accompanies it. [`licenses/BSD-2-Clause.txt`](./licenses/BSD-2-Clause.txt)

- `examples/SteamTestVk/adrenotools/arm64-v8a/*.{a,so}`
- `examples/weston-simple-egl/adrenotools-staging/arm64-v8a/*.so`
- `examples/SteamTestVk/adrenotools/include/adrenotools/*.h`

## Mesa — Turnip Vulkan driver — MIT

Copyright © Mesa/freedreno contributors. Redistributed in binary form.
[`licenses/MIT.txt`](./licenses/MIT.txt)

- `examples/SteamTestVk/turnip/arm64-v8a/libvulkan_freedreno.so`
- `examples/weston-simple-egl/turnip-staging/arm64-v8a/libvulkan_freedreno.so`

## DXVK — zlib

Copyright © Philip Rebohle and the DXVK contributors. Redistributed in binary
form. [`licenses/Zlib.txt`](./licenses/Zlib.txt)

- `examples/SteamTestVk/app/src/main/assets/dxvk/{d3d11,d3d10core,dxgi}.dll`

## Liberation Fonts — SIL OFL 1.1

Copyright © 2012 Red Hat, Inc.; digitized data © 2010 Google Corporation.
[`licenses/OFL-1.1.txt`](./licenses/OFL-1.1.txt)

- `examples/{SteamTest,SteamTestVk}/app/src/main/assets/fonts/Liberation*.ttf`

## DejaVu Sans — Bitstream Vera + Arev licenses

Copyright © 2003 Bitstream, Inc.; © 2006 Tavmjong Bah. DejaVu changes are public
domain. "Bitstream Vera" is a Bitstream trademark and the license restricts
reuse of the names. [`licenses/Bitstream-Vera-and-Arev.txt`](./licenses/Bitstream-Vera-and-Arev.txt)

- `examples/{SteamTest,SteamTestVk}/app/src/main/assets/fonts/DejaVuSans*.ttf`

## Inter — SIL OFL 1.1

Copyright © 2016 The Inter Project Authors.
[`licenses/OFL-1.1.txt`](./licenses/OFL-1.1.txt)

- `examples/test-app/app/src/main/assets/fonts/Inter Variable.ttf`

## JetBrains Mono — SIL OFL 1.1

Copyright © 2020 The JetBrains Mono Project Authors.
[`licenses/OFL-1.1.txt`](./licenses/OFL-1.1.txt)

- `examples/test-app/app/src/main/assets/fonts/JetBrainsMono-Regular.ttf`

## xkeyboard-config data — MIT / X11

Keyboard rules and the generated US keymap derive from xkeyboard-config.
[`licenses/MIT.txt`](./licenses/MIT.txt)

- `examples/*/app/src/main/assets/xkb/rules/*.xml`, `xkb.dtd`
- `android/src/main/cpp/xkb_keymap_us.h` (generated with `xkbcomp`)

## Gradle wrapper — Apache-2.0

Copyright © 2015 the original authors. `SPDX-License-Identifier: Apache-2.0`.
[`licenses/Apache-2.0.txt`](./licenses/Apache-2.0.txt)

- `examples/*/gradlew`, `gradlew.bat`, `gradle/wrapper/gradle-wrapper.jar`

---

## Downloaded at runtime, not redistributed here

The `android-notepad++`, `SteamTest` and `SteamTestVk` examples download a
Wine/Hangover build at runtime from
[Piperella-Hangover-Bionic](https://github.com/piperella/Piperella-Hangover-Bionic).
Wine and Hangover are **LGPL-2.1-or-later**; anyone distributing an application
that bundles them takes on those obligations directly.

## Unresolved provenance

`examples/SteamTestVk/app/src/main/assets/vktest/*.exe` (three mingw-w64-built
Windows test binaries) and `examples/test-app/app/src/main/res/raw/test_audio.mp3`
have no stated origin or license in this repository. They are development test
fixtures. If you redistribute this repository, consider removing them until
their provenance is established.
