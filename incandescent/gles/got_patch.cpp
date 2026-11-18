
#include "got_patch.h"

#include <elf.h>
#include <link.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <string>
#include <vector>

#if defined(__LP64__) && (defined(__aarch64__) || defined(__x86_64__))
#define GP_SUPPORTED 1
#else
#define GP_SUPPORTED 0
#endif

#ifndef DT_ANDROID_RELA
#define DT_ANDROID_RELA 0x60000011
#define DT_ANDROID_RELASZ 0x60000012
#endif

namespace {

#if defined(__aarch64__)
constexpr uint32_t kRelAbs = 257, kRelGlobDat = 1025, kRelJumpSlot = 1026;
#else
constexpr uint32_t kRelAbs = 1, kRelGlobDat = 6, kRelJumpSlot = 7;
#endif

struct Sleb {
    const uint8_t *p, *end;
    bool bad = false;
    int64_t next() {
        int64_t v = 0;
        unsigned shift = 0;
        uint8_t b = 0;
        do {
            if (p >= end) { bad = true; return 0; }
            b = *p++;
            if (shift < 64) v |= static_cast<int64_t>(b & 127) << shift;
            shift += 7;
        } while (b & 128);
        if (shift < 64 && (b & 64)) v |= -(static_cast<int64_t>(1) << shift);
        return v;
    }
};

constexpr int64_t kGroupedByInfo = 1, kGroupedByDelta = 2, kGroupedByAddend = 4, kGroupHasAddend = 8;

}

extern "C" int gp_decode_aps2(const uint8_t *buf, size_t len, gp_reloc_cb cb, void *ctx) {
    if (!buf || len < 4 || memcmp(buf, "APS2", 4) != 0) return -1;
    Sleb d{buf + 4, buf + len};
    const int64_t total = d.next();
    uint64_t off = static_cast<uint64_t>(d.next()), info = 0;
    int64_t addend = 0;
    if (d.bad || total < 0) return -1;
    int64_t done = 0;
    while (done < total) {
        const int64_t gsize = d.next(), flags = d.next();
        if (d.bad || gsize <= 0 || done + gsize > total) return -1;
        int64_t delta = 0;
        if (flags & kGroupedByDelta) delta = d.next();
        if (flags & kGroupedByInfo) info = static_cast<uint64_t>(d.next());
        const int64_t fr = flags & (kGroupHasAddend | kGroupedByAddend);
        if (fr == kGroupHasAddend) {  }
        else if (fr == (kGroupHasAddend | kGroupedByAddend)) addend += d.next();
        else addend = 0;
        for (int64_t i = 0; i < gsize; i++) {
            off += (flags & kGroupedByDelta) ? static_cast<uint64_t>(delta) : static_cast<uint64_t>(d.next());
            if (!(flags & kGroupedByInfo)) info = static_cast<uint64_t>(d.next());
            if (fr == kGroupHasAddend) addend += d.next();
            if (d.bad) return -1;
            if (cb) cb(off, info, addend, ctx);
        }
        done += gsize;
    }
    return static_cast<int>(done);
}

#if GP_SUPPORTED
namespace {

using Dyn = ElfW(Dyn);
using Sym = ElfW(Sym);
using Rela = ElfW(Rela);

struct ModInfo {
    std::string path;
    uintptr_t bias;
    const ElfW(Phdr) *phdr;
    int phnum;
};

struct MapRange { uintptr_t lo, hi; int prot; };

std::vector<MapRange> read_maps() {
    std::vector<MapRange> v;
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return v;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        unsigned long lo = 0, hi = 0;
        char r = 0, w = 0, x = 0;
        if (sscanf(line, "%lx-%lx %c%c%c", &lo, &hi, &r, &w, &x) != 5) continue;
        v.push_back({lo, hi, (r == 'r' ? PROT_READ : 0) | (w == 'w' ? PROT_WRITE : 0) | (x == 'x' ? PROT_EXEC : 0)});
    }
    fclose(f);
    return v;
}

int prot_at(const std::vector<MapRange> &m, uintptr_t a) {
    for (const auto &r : m)
        if (a >= r.lo && a < r.hi) return r.prot;
    return -1;
}

bool write_slot(const std::vector<MapRange> &maps, void **slot, void *value) {
    const uintptr_t a = reinterpret_cast<uintptr_t>(slot);
    const int prot = prot_at(maps, a);
    if (prot < 0 || !(prot & PROT_READ)) return false;
    if (prot & PROT_WRITE) { __atomic_store_n(slot, value, __ATOMIC_RELEASE); return true; }
    const long ps = sysconf(_SC_PAGESIZE);
    void *page = reinterpret_cast<void *>(a & ~static_cast<uintptr_t>(ps - 1));
    if (mprotect(page, static_cast<size_t>(ps), prot | PROT_WRITE) != 0) return false;
    __atomic_store_n(slot, value, __ATOMIC_RELEASE);
    mprotect(page, static_cast<size_t>(ps), prot);
    return true;
}

uintptr_t fix(uintptr_t p, uintptr_t bias) { return (bias && p < bias) ? p + bias : p; }

struct Walk {
    gp_visit_fn visit;
    void *vctx;
    int write;
    gp_stats *st;
    const ModInfo *mod;
    const char *strtab;
    const Sym *symtab;
    const std::vector<MapRange> *maps;
    bool dead;
};

void on_reloc(Walk &w, uint64_t off, uint64_t info, int64_t addend) {
    const uint32_t type = ELF64_R_TYPE(info), sym = ELF64_R_SYM(info);
    if (!sym) return;
    if (type != kRelGlobDat && type != kRelJumpSlot && !(type == kRelAbs && addend == 0)) return;
    const char *name = w.strtab + w.symtab[sym].st_name;
    if (!*name) return;
    if (w.st) w.st->imports++;
    void **slot = reinterpret_cast<void **>(w.mod->bias + off);
    void *now = *slot;
    void *want = w.visit(w.mod->path.c_str(), name, now, w.vctx);
    if (!want || want == now) return;
    if (!w.write) { if (w.st) w.st->patched++; return; }
    if (write_slot(*w.maps, slot, want)) { if (w.st) w.st->patched++; }
    else if (w.st) w.st->failed++;
}

void walk_module(const ModInfo &m, gp_visit_fn visit, void *vctx, int write, gp_stats *st, const std::vector<MapRange> &maps) {
    const Dyn *dyn = nullptr;
    for (int i = 0; i < m.phnum; i++)
        if (m.phdr[i].p_type == PT_DYNAMIC) { dyn = reinterpret_cast<const Dyn *>(m.bias + m.phdr[i].p_vaddr); break; }
    if (!dyn) { if (st) st->unsupported++; return; }

    uintptr_t strtab = 0, symtab = 0, jmprel = 0, rela = 0, arela = 0;
    size_t jmprelsz = 0, relasz = 0, arelasz = 0;
    std::vector<size_t> needed;
    for (const Dyn *d = dyn; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
            case DT_STRTAB: strtab = fix(d->d_un.d_ptr, m.bias); break;
            case DT_SYMTAB: symtab = fix(d->d_un.d_ptr, m.bias); break;
            case DT_JMPREL: jmprel = fix(d->d_un.d_ptr, m.bias); break;
            case DT_PLTRELSZ: jmprelsz = d->d_un.d_val; break;
            case DT_RELA: rela = fix(d->d_un.d_ptr, m.bias); break;
            case DT_RELASZ: relasz = d->d_un.d_val; break;
            case DT_ANDROID_RELA: arela = fix(d->d_un.d_ptr, m.bias); break;
            case DT_ANDROID_RELASZ: arelasz = d->d_un.d_val; break;
            case DT_NEEDED: needed.push_back(d->d_un.d_val); break;
            default: break;
        }
    }
    if (!strtab || !symtab) { if (st) st->unsupported++; return; }
    for (size_t off : needed)
        if (st && strncmp(reinterpret_cast<const char *>(strtab) + off, "libvulkan.so", 12) == 0) st->needs_libvulkan = 1;

    Walk w{visit, vctx, write, st, &m, reinterpret_cast<const char *>(strtab), reinterpret_cast<const Sym *>(symtab), &maps, false};
    auto plain = [&](uintptr_t tab, size_t sz) {
        if (!tab) return;
        const Rela *r = reinterpret_cast<const Rela *>(tab);
        for (size_t i = 0; i < sz / sizeof(Rela); i++) on_reloc(w, r[i].r_offset, r[i].r_info, r[i].r_addend);
    };
    plain(jmprel, jmprelsz);
    plain(rela, relasz);
    if (arela && arelasz) {
        struct Cb { Walk *w; } cb{&w};
        const int n = gp_decode_aps2(reinterpret_cast<const uint8_t *>(arela), arelasz,
            [](uint64_t off, uint64_t info, int64_t addend, void *c) { on_reloc(*static_cast<Cb *>(c)->w, off, info, addend); }, &cb);
        if (n < 0 && st) st->unsupported++;
    }
}

}
#endif

extern "C" int gp_walk(gp_match_fn match, void *match_ctx, gp_visit_fn visit, void *visit_ctx, int write, gp_stats *st) {
    gp_stats local;
    if (!st) st = &local;
    memset(st, 0, sizeof *st);
#if GP_SUPPORTED
    if (!match || !visit) return 0;

    struct Collect { std::vector<ModInfo> mods; gp_match_fn match; void *ctx; } c{{}, match, match_ctx};
    dl_iterate_phdr([](struct dl_phdr_info *i, size_t, void *p) -> int {
        auto *cc = static_cast<Collect *>(p);
        if (!i->dlpi_name || !*i->dlpi_name) return 0;
        if (cc->match(i->dlpi_name, cc->ctx)) cc->mods.push_back({i->dlpi_name, static_cast<uintptr_t>(i->dlpi_addr), i->dlpi_phdr, i->dlpi_phnum});
        return 0;
    }, &c);
    if (c.mods.empty()) return 0;
    const std::vector<MapRange> maps = read_maps();
    int patched = 0;
    for (const ModInfo &m : c.mods) {
        st->modules++;
        const int before = st->patched;
        walk_module(m, visit, visit_ctx, write, st, maps);
        patched += st->patched - before;
    }
    return patched;
#else
    (void)match_ctx; (void)visit_ctx; (void)write;
    return 0;
#endif
}
