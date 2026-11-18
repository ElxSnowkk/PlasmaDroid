
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void *(*gp_visit_fn)(const char *module_path, const char *sym, void *current, void *ctx);

typedef int (*gp_match_fn)(const char *module_path, void *ctx);

typedef struct {
    int modules;
    int imports;
    int patched;
    int failed;
    int needs_libvulkan;
    int unsupported;
} gp_stats;

int gp_walk(gp_match_fn match, void *match_ctx, gp_visit_fn visit, void *visit_ctx, int write, gp_stats *st);

typedef void (*gp_reloc_cb)(uint64_t off, uint64_t info, int64_t addend, void *ctx);
int gp_decode_aps2(const uint8_t *buf, size_t len, gp_reloc_cb cb, void *ctx);

#ifdef __cplusplus
}
#endif
