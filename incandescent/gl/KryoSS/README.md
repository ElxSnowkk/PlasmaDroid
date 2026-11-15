# KryoSS v5

KryoSS is a standalone spatial upscaler module in Incandescent; it is not part of the FSR1 implementation.

- `KryoSS.frag`: GLES 3.0+ compatible fragment fallback.
- `KryoSS.comp`: GLES 3.1+ compute path (8×8 workgroups, shared input tile up to 12×12).
- `KryoSSParams.h`: preset and parameter definitions.
- `KryoSSShaderSource.h` / `KryoSSComputeSource.h`: generated embedded shader headers; do not edit manually.
- `KryoSS.cpp`: source selection and uniform binding.

Regenerate headers from the shader source files with `python3 tools/gen_kryoss_header.py` at the project root. The generic upscaler dispatcher is in `gl/upscaling/`.

See `docs/KRYOSS_V5_COMPUTE_VALIDATION.md` for the 10-round validation, measured parity, test environment, limitations, and reproducible commands.

## Resolução mínima base, saída e janela

- **Janela** = o que o jogo vê (ex.: 720p no Zalith). O fim da cadeia é sempre ajustado a ela (`glBlitFramebuffer` com destino = janela); copiar a saída 1:1 mostrava só um canto (o "zoom").
- **Resolução mínima base** (`upscalerMinBase`, px) = a resolução em que o jogo desenha e que o KryoSS escala; nunca acima da janela. Cada modo seleciona a sua (360/480/540/720). Viewport, scissor e blits do jogo no framebuffer 0 são reduzidos na mesma proporção (`FSR1.cpp`, `framebuffer.cpp`).
- **Saída** = a própria janela (resolução do Zalith). O tamanho da janela é relido a cada 60 quadros; ao mudar, render e saída se adaptam sozinhos.

Chave ausente (0) mantém o comportamento antigo. Não verificado em aparelho.
