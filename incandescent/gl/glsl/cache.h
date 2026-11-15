// Parte do PlasmaDroid — Incandescent (upscaler/renderer).
// Créditos: baseado no projeto Mobile Glues.

// Incandescent - gl/glsl/cache.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header

#ifndef INCANDESCENT_PLUGIN_CACHE_H
#define INCANDESCENT_PLUGIN_CACHE_H

#include "../inc.h"
#include "../../config/config.h"
#include "../../config/settings.h"

#include <list>
#include <array>
#include <string>
#include <cstdint>

class Cache {
public:
    Cache();
    ~Cache();

    Cache(const Cache&) = delete;
    Cache& operator=(const Cache&) = delete;

    const char* get(const char* glsl);
    void put(const char* glsl, const char* essl);
    bool load();

    void save();

    static Cache& get_instance();

private:
    struct CacheEntry {
        std::array<uint8_t, 32> sha256;
        std::string essl;
        size_t size;
    };

    struct SHA256Hash {
        size_t operator()(const std::array<uint8_t, 32>& key) const;
    };

    std::list<CacheEntry> cacheList;
    using ListIterator = std::list<CacheEntry>::iterator;
    UnorderedMap<std::array<uint8_t, 32>, ListIterator, SHA256Hash> cacheMap;
    size_t cacheSize = 0;

    int pendingEntries = 0;
    int64_t lastSaveNs = 0;

    static std::array<uint8_t, 32> computeSHA256(const uint8_t* data, size_t length);
    void maintainCacheSize();
    void flushIfDue();
};

#endif
