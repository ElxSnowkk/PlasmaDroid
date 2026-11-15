# Incandescent

Tradutor **OpenGL → OpenGL ES** (front-end EGL/GL próprio + GLSL→SPIR-V→GLSL ES), usado pelo PlasmaDroid como
backend OpenGL do guest Debian no lugar do Zink. Roda como biblioteca Bionic nativa (`libincandescent.so`) e conversa
com a GPU por **ANGLE (GLES → Vulkan)**.

```
app Qt/KDE (glibc, PRoot)
  └─ libEGL/libGLESv2 do libhybris  ── LIBEGL / LIBGLESV2 = libincandescent.so
        └─ libincandescent.so (Bionic): GL → GLES
              └─ ANGLE (libEGL_angle.so / libGLESv2_angle.so): GLES → Vulkan
                    └─ libvulkan.so do Android (driver do fabricante)
```

Licença: **LGPL-2.1** (`LICENSE`). Os avisos de copyright dos arquivos originais foram mantidos, como a licença exige; veja `NOTICE.md`.

## Build
```
cmake -S incandescent -B build/incandescent -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
      -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29
cmake --build build/incandescent
```
Dependências de `3rdparty/` (glslang, SPIRV-Cross) já estão no repositório; `include/ska/flat_hash_map.hpp` também.

## Variáveis de ambiente
| Variável | Função |
|---|---|
| `INC_DIR_PATH` | pasta de dados/config/cache (padrão `/sdcard/Incandescent`) |
| `INC_ANGLE_DIR` | pasta com `libEGL_angle.so` e `libGLESv2_angle.so` |
| `INC_PLUGIN_STATUS` | arquivo de status que o carregador grava |
| `INC_RENDERER_TEMPLATE` / `INC_RENDERER_STRING` | texto de `GL_RENDERER` (`{GPU}` vira o nome da GPU) |
| `INC_GPU_REPORT` | arquivo onde grava `GPU=`, `GPU_FULL=`, `GL_RENDERER_RAW=` |
| `INC_VK_BRIDGE`, `INC_VK_REPORT` | interceptação opcional do Vulkan do ANGLE (`gles/vk_intercept.*`) |
