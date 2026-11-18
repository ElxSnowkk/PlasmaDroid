

#include "cache.h"
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>

using namespace std;

static const uint32_t k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

namespace {
    inline uint32_t rotr(uint32_t x, uint32_t n) {
        return (x >> n) | (x << (32 - n));
    }
    inline uint32_t sigma0(uint32_t x) {
        return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3);
    }
    inline uint32_t sigma1(uint32_t x) {
        return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10);
    }
    inline uint32_t Sigma0(uint32_t x) {
        return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22);
    }
    inline uint32_t Sigma1(uint32_t x) {
        return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25);
    }
    inline uint32_t ch(uint32_t x, uint32_t y, uint32_t z) {
        return (x & y) ^ (~x & z);
    }
    inline uint32_t maj(uint32_t x, uint32_t y, uint32_t z) {
        return (x & y) ^ (x & z) ^ (y & z);
    }

    void sha256_compress(uint32_t h[8], const uint8_t* block) {
        uint32_t w[64];
        for (int t = 0; t < 16; ++t) {

            w[t] = (static_cast<uint32_t>(block[t * 4]) << 24) | (static_cast<uint32_t>(block[t * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(block[t * 4 + 2]) << 8) | static_cast<uint32_t>(block[t * 4 + 3]);
        }

        for (int t = 16; t < 64; ++t) {
            w[t] = sigma1(w[t - 2]) + w[t - 7] + sigma0(w[t - 15]) + w[t - 16];
        }

        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];

        for (int t = 0; t < 64; ++t) {
            uint32_t T1 = hh + Sigma1(e) + ch(e, f, g) + k[t] + w[t];
            uint32_t T2 = Sigma0(a) + maj(a, b, c);
            hh = g;
            g = f;
            f = e;
            e = d + T1;
            d = c;
            c = b;
            b = a;
            a = T1 + T2;
        }

        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += hh;
    }
}

array<uint8_t, 32> Cache::computeSHA256(const uint8_t* data, size_t length) {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

    size_t offset = 0;
    for (; length - offset >= 64; offset += 64) {
        sha256_compress(h, data + offset);
    }

    const size_t remainder = length - offset;
    uint8_t tail[128] = {0};
    if (remainder > 0) memcpy(tail, data + offset, remainder);
    tail[remainder] = 0x80;
    const size_t tail_size = (remainder < 56) ? 64 : 128;

    const uint64_t bit_length = static_cast<uint64_t>(length) * 8;
    for (int i = 0; i < 8; ++i) {
        tail[tail_size - 8 + i] = static_cast<uint8_t>(bit_length >> (56 - i * 8));
    }
    for (size_t i = 0; i < tail_size; i += 64) {
        sha256_compress(h, tail + i);
    }

    array<uint8_t, 32> hash{};
    for (int i = 0; i < 8; ++i) {
        hash[i * 4] = static_cast<uint8_t>(h[i] >> 24);
        hash[i * 4 + 1] = static_cast<uint8_t>(h[i] >> 16);
        hash[i * 4 + 2] = static_cast<uint8_t>(h[i] >> 8);
        hash[i * 4 + 3] = static_cast<uint8_t>(h[i]);
    }
    return hash;
}

size_t Cache::SHA256Hash::operator()(const array<uint8_t, 32>& key) const {
    size_t hash = 0;
    for (int i = 0; i < 4; ++i) {
        hash = (hash << 8) | key[i];
    }
    for (int i = 28; i < 32; ++i) {
        hash = (hash << 8) | key[i];
    }
    return hash;
}

namespace {
    constexpr int kPendingEntriesBeforeSave = 16;
    constexpr int64_t kSaveIntervalNs = 5LL * 1000 * 1000 * 1000;

    int64_t monotonic_now_ns() {

        timespec ts{};
        if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
        return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + static_cast<int64_t>(ts.tv_nsec);
    }

    thread_local string g_hash_memo_source;
    thread_local array<uint8_t, 32> g_hash_memo_digest{};
    thread_local bool g_hash_memo_valid = false;
}

Cache::Cache() {
    load();
    lastSaveNs = monotonic_now_ns();
}

Cache::~Cache() {

    if (pendingEntries > 0) save();
}

void Cache::flushIfDue() {
    if (pendingEntries == 0) return;
    if (pendingEntries < kPendingEntriesBeforeSave && (monotonic_now_ns() - lastSaveNs) < kSaveIntervalNs) return;
    save();
}

const char* Cache::get(const char* glsl) {
    if (global_settings.max_glsl_cache_size <= 0) return nullptr;
    flushIfDue();

    const size_t length = strlen(glsl);
    auto hash = computeSHA256(reinterpret_cast<const uint8_t*>(glsl), length);
    auto it = cacheMap.find(hash);
    if (it == cacheMap.end()) {

        g_hash_memo_valid = false;
        g_hash_memo_source.assign(glsl, length);
        g_hash_memo_digest = hash;
        g_hash_memo_valid = true;
        return nullptr;
    }

    cacheList.splice(cacheList.end(), cacheList, it->second);
    return it->second->essl.c_str();
}

void Cache::put(const char* glsl, const char* essl) {
    if (global_settings.max_glsl_cache_size <= 0) return;

    const size_t length = strlen(glsl);
    array<uint8_t, 32> hash;
    if (g_hash_memo_valid && g_hash_memo_source.size() == length &&
        memcmp(g_hash_memo_source.data(), glsl, length) == 0) {
        hash = g_hash_memo_digest;
    } else {
        hash = computeSHA256(reinterpret_cast<const uint8_t*>(glsl), length);
    }
    size_t esslStrSize = strlen(essl) + 1;

    size_t entryMemory = sizeof(CacheEntry::sha256) + sizeof(size_t) + esslStrSize;

    if (auto it = cacheMap.find(hash); it != cacheMap.end()) {
        cacheSize -= (sizeof(CacheEntry::sha256) + sizeof(size_t) + it->second->size);
        cacheList.erase(it->second);
        cacheMap.erase(it);
    }

    cacheList.emplace_back(CacheEntry{hash, essl, esslStrSize});
    cacheMap[hash] = prev(cacheList.end());
    cacheSize += entryMemory;

    maintainCacheSize();
    ++pendingEntries;
    flushIfDue();
}

void Cache::maintainCacheSize() {
    if (global_settings.max_glsl_cache_size <= 0) return;
    while (cacheSize > global_settings.max_glsl_cache_size && !cacheList.empty()) {
        const auto& oldEntry = cacheList.front();
        size_t removedMemory = sizeof(CacheEntry::sha256) + sizeof(size_t) + oldEntry.size;
        cacheSize -= removedMemory;
        cacheMap.erase(oldEntry.sha256);
        cacheList.pop_front();
    }
}

bool Cache::load() {
    try {

        if (!glsl_cache_file_path) return false;
        ifstream file(glsl_cache_file_path, ios::binary);
        if (!file) return false;

        size_t count;
        file.read(reinterpret_cast<char*>(&count), sizeof(count));

        while (count--) {
            array<uint8_t, 32> hash{};
            size_t esslSize;

            file.read(reinterpret_cast<char*>(hash.data()), hash.size());
            file.read(reinterpret_cast<char*>(&esslSize), sizeof(esslSize));

            string essl(esslSize, '\0');
            file.read(essl.data(), (long)esslSize);

            if (cacheMap.count(hash)) continue;

            size_t entryMemory = sizeof(CacheEntry::sha256) + sizeof(size_t) + esslSize;
            cacheSize += entryMemory;

            cacheList.emplace_back(CacheEntry{hash, move(essl), esslSize});
            cacheMap[hash] = prev(cacheList.end());
        }

        maintainCacheSize();
        return true;
    }
    catch (...) {
        LOG_W_FORCE("Error while loading glsl cache file. Clearing it...")
        cacheMap.clear();
        cacheSize = 0;
        cacheList.clear();
        save();
        return false;
    }
}

void Cache::save() {
    if (global_settings.max_glsl_cache_size <= 0) return;
    if (!glsl_cache_file_path) return;

    pendingEntries = 0;
    lastSaveNs = monotonic_now_ns();

    const string temp_path = string(glsl_cache_file_path) + ".new";
    {
        ofstream file(temp_path, ios::binary);
        if (!file) return;

        size_t count = cacheList.size();
        file.write(reinterpret_cast<const char*>(&count), sizeof(count));

        for (const auto& entry : cacheList) {
            file.write(reinterpret_cast<const char*>(entry.sha256.data()), (long)entry.sha256.size());
            size_t esslSize = entry.size;
            file.write(reinterpret_cast<const char*>(&esslSize), sizeof(esslSize));
            file.write(entry.essl.data(), (long)esslSize);
        }

        file.flush();
        if (!file) {

            file.close();
            std::remove(temp_path.c_str());
            return;
        }
    }

    if (std::rename(temp_path.c_str(), glsl_cache_file_path) != 0) {
        std::remove(temp_path.c_str());
    }
}

Cache& Cache::get_instance() {
    static Cache s_cache;
    return s_cache;
}
