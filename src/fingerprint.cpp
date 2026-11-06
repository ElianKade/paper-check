// =============================================================================
//  fingerprint.cpp —— 文本指纹实现
// =============================================================================
#include "fingerprint.h"

#include <algorithm>
#include <cstring>

namespace pc {
namespace {

// 64 位混合函数（MurmurHash3 的 finalizer），用于打散哈希分布
uint64_t mix64(uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

// MinHash 置换使用的固定随机系数表（保证结果可复现）
const uint64_t kPermA[] = {
    0x9E3779B97F4A7C15ULL, 0xBF58476D1CE4E5B9ULL, 0x94D049BB133111EBULL, 0xD6E8FEB86659FD93ULL,
    0xA24BAED4963EE407ULL, 0x9FB21C651E98DF25ULL, 0xC2B2AE3D27D4EB4FULL, 0x165667B19E3779F9ULL};
const uint64_t kPermB[] = {
    0x7F4A7C159E3779B9ULL, 0x4CE4E5B9BF58476DULL, 0x133111EB94D049BBULL, 0x6659FD93D6E8FEB8ULL,
    0x963EE407A24BAED4ULL, 0x1E98DF259FB21C65ULL, 0x27D4EB4FC2B2AE3DULL, 0x9E3779F9165667B1ULL};

}  // namespace

uint64_t hashBytes64(const void* data, size_t len) {
    const unsigned char* p = static_cast<const unsigned char*>(data);
    uint64_t h = 1469598103934665603ULL;  // FNV offset basis
    for (size_t i = 0; i < len; ++i) {
        h ^= p[i];
        h *= 1099511628211ULL;  // FNV prime
    }
    return mix64(h);
}

std::vector<uint64_t> gramHashes(const std::vector<uint32_t>& chars, int k) {
    std::vector<uint64_t> out;
    if (k <= 0 || chars.size() < static_cast<size_t>(k)) return out;
    out.reserve(chars.size() - static_cast<size_t>(k) + 1);

    // 滚动哈希：h = ((c0 * B + c1) * B + c2) ... ，B 取奇数大素数
    const uint64_t base = 1099511628211ULL;
    // 预先计算 base^(k-1)，用于滚出最左边的字符
    uint64_t powBase = 1;
    for (int i = 0; i < k - 1; ++i) powBase *= base;

    uint64_t h = 0;
    for (int i = 0; i < k; ++i) h = h * base + (chars[static_cast<size_t>(i)] + 1);
    out.push_back(mix64(h));

    for (size_t i = static_cast<size_t>(k); i < chars.size(); ++i) {
        h -= (chars[i - static_cast<size_t>(k)] + 1) * powBase;
        h = h * base + (chars[i] + 1);
        out.push_back(mix64(h));
    }
    return out;
}

std::vector<uint64_t> uniqueSorted(std::vector<uint64_t> hashes) {
    std::sort(hashes.begin(), hashes.end());
    hashes.erase(std::unique(hashes.begin(), hashes.end()), hashes.end());
    return hashes;
}

size_t intersectionSize(const std::vector<uint64_t>& a, const std::vector<uint64_t>& b) {
    size_t i = 0, j = 0, count = 0;
    while (i < a.size() && j < b.size()) {
        if (a[i] < b[j]) ++i;
        else if (a[i] > b[j]) ++j;
        else { ++count; ++i; ++j; }
    }
    return count;
}

double jaccardSimilarity(const std::vector<uint64_t>& a, const std::vector<uint64_t>& b) {
    if (a.empty() && b.empty()) return 0.0;
    size_t inter = intersectionSize(a, b);
    size_t uni = a.size() + b.size() - inter;
    return uni == 0 ? 0.0 : static_cast<double>(inter) / static_cast<double>(uni);
}

double containmentSimilarity(const std::vector<uint64_t>& a, const std::vector<uint64_t>& b) {
    size_t smaller = std::min(a.size(), b.size());
    if (smaller == 0) return 0.0;
    return static_cast<double>(intersectionSize(a, b)) / static_cast<double>(smaller);
}

uint64_t simhash64(const std::vector<uint64_t>& hashes) {
    if (hashes.empty()) return 0;
    // 对每个哈希的每一位做加权投票
    int64_t weights[64] = {0};
    for (uint64_t h : hashes) {
        uint64_t w = mix64(h ^ 0x9E3779B97F4A7C15ULL);
        for (int bit = 0; bit < 64; ++bit) {
            if ((w >> bit) & 1ULL) weights[bit] += 1;
            else weights[bit] -= 1;
        }
    }
    uint64_t fp = 0;
    for (int bit = 0; bit < 64; ++bit) {
        if (weights[bit] > 0) fp |= (1ULL << bit);
    }
    return fp;
}

int hammingDistance64(uint64_t a, uint64_t b) {
    uint64_t x = a ^ b;
    int count = 0;
    while (x) {
        x &= x - 1;
        ++count;
    }
    return count;
}

MinHashSignature minhashSignature(const std::vector<uint64_t>& hashes, int numPerm) {
    MinHashSignature sig;
    if (numPerm <= 0 || hashes.empty()) return sig;
    sig.values.assign(static_cast<size_t>(numPerm), ~0ULL);

    for (int p = 0; p < numPerm; ++p) {
        uint64_t a = kPermA[static_cast<size_t>(p) % 8] + static_cast<uint64_t>(p) * 0x9E3779B1ULL;
        uint64_t b = kPermB[static_cast<size_t>(p) % 8] + static_cast<uint64_t>(p) * 0x85EBCA6BULL;
        uint64_t best = ~0ULL;
        for (uint64_t h : hashes) {
            uint64_t v = mix64(h ^ a) + b;
            if (v < best) best = v;
        }
        sig.values[static_cast<size_t>(p)] = best;
    }
    return sig;
}

double minhashSimilarity(const MinHashSignature& a, const MinHashSignature& b) {
    size_t n = std::min(a.values.size(), b.values.size());
    if (n == 0) return 0.0;
    size_t same = 0;
    for (size_t i = 0; i < n; ++i) {
        if (a.values[i] == b.values[i]) ++same;
    }
    return static_cast<double>(same) / static_cast<double>(n);
}

uint32_t lshBandKey(const MinHashSignature& sig, int band, int rowsPerBand) {
    size_t start = static_cast<size_t>(band) * static_cast<size_t>(rowsPerBand);
    if (start >= sig.values.size()) return 0;
    uint64_t h = 1469598103934665603ULL;
    for (int i = 0; i < rowsPerBand && start + static_cast<size_t>(i) < sig.values.size(); ++i) {
        h ^= sig.values[start + static_cast<size_t>(i)];
        h *= 1099511628211ULL;
    }
    return static_cast<uint32_t>(mix64(h));
}

} // namespace pc
