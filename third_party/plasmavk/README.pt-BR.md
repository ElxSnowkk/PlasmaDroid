# PlasmaVK — kit de integração Vulkan para ASDroid

**Objetivo:** permitir que programas Linux/glibc dentro do PRoot usem o driver Vulkan do Android sem depender do Termux e sem alterar o ambiente Bionic do processo que inicia o PRoot.

Este kit fornece uma API Kotlin para o app, um script de diagnóstico, documentação e um patch experimental para o `src/vulkan/wrapper` do Mesa 24.2.5 enviado. **Não inclui binários `.so` prontos**: o runtime que faz a ponte ABI precisa ser compilado para a arquitetura e a versão Android do aparelho.

## Caminho recomendado primeiro

Use o Vulkan loader glibc da distro com um ICD/adapter glibc-side baseado no fork [taowen/libhybris](https://github.com/taowen/libhybris), que foi desenvolvido para processos glibc em Android sem modificar a imagem do sistema. A discussão [DroidDeck #165](https://github.com/Droid-Deck/DroidDeck/issues/165) relata testes desse caminho em Mali e também documenta uma alternativa Venus/virtio.

O loader Vulkan Linux descobre ICDs usando manifests JSON (`VK_DRIVER_FILES`, ou `VK_ICD_FILENAMES` em loaders antigos); o Android descobre o driver do SoC por meio do runtime/HAL nativo. Por isso, apontar o loader glibc diretamente para `/system/lib64/libvulkan.so` **não** resolve a diferença entre Bionic e glibc. Veja a [interface ICD do Khronos](https://github.com/KhronosGroup/Vulkan-Loader/blob/main/docs/LoaderDriverInterface.md) e a [arquitetura Vulkan do Android](https://source.android.com/docs/core/graphics/arch-vulkan).

## Uso no app

Copie `app-api/PlasmaVK.kt` para um módulo Kotlin do ASDroid e ajuste o `package`. Configure o manifesto **real** gerado pelo ICD instalado no rootfs:

```kotlin
val config = PlasmaVK.Config(
    backend = PlasmaVK.Backend.SYSTEM_BIONIC_LIBHYBRIS,
    icdManifestGuestPath = "/usr/share/vulkan/icd.d/<manifest-real>.json",
    guestLibraryDirs = listOf(
        "/usr/lib/plasmavk",
        "/usr/lib/aarch64-linux-gnu/libhybris",
    ),
    enableZink = true, // opcional: OpenGL sobre Vulkan
    debug = false,
)

val argv = PlasmaVK.buildCommand(
    prootBinary = File(filesDir, "bin/proot"),
    rootfs = File(filesDir, "rootfs/debian"),
    guestCommand = listOf("/usr/bin/vulkaninfo", "--summary"),
    config = config,
)

// Executa no processo Android nativo; as variáveis Vulkan/LD_LIBRARY_PATH
// são definidas no processo convidado via /usr/bin/env, não no PRoot Bionic.
val process = ProcessBuilder(argv).redirectErrorStream(true).start()
```

O caminho do manifest é o caminho **dentro da distro**, não o caminho Android do arquivo. Não use `/system/lib64` como `LD_LIBRARY_PATH` da distro e não substitua a glibc do rootfs.

## Wrapper Mesa enviado

O patch `patches/0001-mesa-wrapper-configurable-loader.patch` adiciona `PLASMAVK_VULKAN_LOADER`, modo estrito, seleção de hooks independente do Termux e suporte ao ponto de entrada ICD `vk_icdGetInstanceProcAddr`. Ele pode ajudar se existir uma build glibc-compatível do wrapper.

**Limite importante:** o wrapper do ZIP usa APIs/headers Android, incluindo `AHardwareBuffer`, e o Meson tem dependências específicas do alvo Android. O patch não porta todo o wrapper para glibc. Portanto, comece pelo ICD direto do libhybris; a combinação `Mesa wrapper → libhybris ICD` é experimental e exige trabalho adicional para compilar o wrapper no lado glibc.

## Testes

O kit foi validado localmente com:

- compilação e execução dos testes Kotlin da API;
- `bash -n` no script `scripts/plasmavk-doctor.sh`;
- aplicação em modo `--dry-run` do patch contra o `wrapper_instance.c` original do Mesa enviado.

Não foi feita compilação nativa completa do Mesa nem teste no aparelho, e não há alegação de renderização end-to-end. O próximo critério de aceitação é `vulkaninfo --summary` listar a GPU real, seguido de um teste Vulkan offscreen e, depois, WSI/apresentação.

## Alternativa quando libhybris não servir

Mesa Venus/virtio pode serializar chamadas do guest por um socket local para um serviço `virglrenderer` compilado com o NDK, que carrega o `libvulkan.so` nativo do Android. Essa arquitetura atende literalmente ao modelo de serviço Bionic separado, mas requer um serviço host e correções específicas para memória compartilhada, dma-buf, sincronização e WSI. **O serviço Venus host não está implementado neste kit.**
