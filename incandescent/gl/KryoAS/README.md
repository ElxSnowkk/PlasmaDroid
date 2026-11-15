# KryoAS prototype

Two GLSL ES 3.0 passes: `KryoAS_Reconstruct.frag` then `KryoAS_Anomaly.frag`. The renderer must provide two intermediate targets and ping-pong history. The existing one-pass upscaler dispatcher is intentionally unchanged until that lifecycle is extended safely. See `docs/KRYOAS_KRYOSS_V6.md` for uniforms, history rules, training and validation.
