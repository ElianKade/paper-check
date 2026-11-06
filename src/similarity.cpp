// =============================================================================
//  similarity.cpp —— 文档比对：种子哈希 + 最长公共子串扩展 + 覆盖率
// =============================================================================
#include "similarity.h"

#include "fingerprint.h"

#include <algorithm>
#include <unordered_map>

namespace pc {
namespace {

constexpr uint64_t kRollBase = 1099511628211ULL;

// 计算 s 上每个长度为 k 的窗口的滚动哈希（与 gramHashes 相同的混合方式）
std::vector<uint64_t> rollingHashes(const std::vector<uint32_t>& s, size_t k) {
    std::vector<uint64_t> out;
    if (k == 0 || s.size() < k) return out;
    out.reserve(s.size() - k + 1);

    uint64_t powBase = 1;
    for (size_t i = 0; i + 1 < k; ++i) powBase *= kRollBase;

    uint64_t h = 0;
    for (size_t i = 0; i < k; ++i) h = h * kRollBase + (s[i] + 1);
    out.push_back(h);

    for (size_t i = k; i < s.size(); ++i) {
        h -= (s[i - k] + 1) * powBase;
        h = h * kRollBase + (s[i] + 1);
        out.push_back(h);
    }
    return out;
}

// 两个区间是否相交（半开区间）
bool overlaps(const Interval& x, const Interval& y) {
    return x.first < y.second && y.first < x.second;
}

}  // namespace

std::vector<Interval> mergeIntervals(std::vector<Interval> intervals) {
    std::vector<Interval> result;
    if (intervals.empty()) return result;
    std::sort(intervals.begin(), intervals.end());
    result.push_back(intervals[0]);
    for (size_t i = 1; i < intervals.size(); ++i) {
        Interval& last = result.back();
        if (intervals[i].first <= last.second) {
            if (intervals[i].second > last.second) last.second = intervals[i].second;
        } else {
            result.push_back(intervals[i]);
        }
    }
    return result;
}

size_t totalLength(const std::vector<Interval>& intervals) {
    size_t sum = 0;
    for (const Interval& it : intervals) sum += it.second - it.first;
    return sum;
}

PairStats compareDocuments(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b,
                           const MatchOptions& options) {
    PairStats stats;
    stats.totalA = a.size();
    stats.totalB = b.size();

    // ---- 第一步：集合表示层面的相似度（快，先给出整体判断）----
    const int shingleK = options.shingleK > 0 ? options.shingleK : 13;
    std::vector<uint64_t> gramsA = uniqueSorted(gramHashes(a, shingleK));
    std::vector<uint64_t> gramsB = uniqueSorted(gramHashes(b, shingleK));
    stats.jaccard = jaccardSimilarity(gramsA, gramsB);
    stats.containment = containmentSimilarity(gramsA, gramsB);
    const int kPerm = 64;
    stats.minhashSim =
        minhashSimilarity(minhashSignature(gramsA, kPerm), minhashSignature(gramsB, kPerm));
    stats.simhashDistance = hammingDistance64(simhash64(gramsA), simhash64(gramsB));

    // ---- 第二步：种子 + 扩展，定位具体重复片段 ----
    int seedLen = options.minMatch > 0 ? options.minMatch : 20;
    if (seedLen < 8) seedLen = 8;
    const size_t k = static_cast<size_t>(seedLen);
    if (a.size() < k || b.size() < k) return stats;

    std::vector<uint64_t> hashesB = rollingHashes(b, k);
    std::unordered_map<uint64_t, std::vector<uint32_t>> index;
    index.reserve(hashesB.size() * 2);
    for (size_t j = 0; j < hashesB.size(); ++j) {
        std::vector<uint32_t>& bucket = index[hashesB[j]];
        if (bucket.size() < static_cast<size_t>(options.maxCandidates)) {
            bucket.push_back(static_cast<uint32_t>(j));
        }
    }

    std::vector<uint64_t> hashesA = rollingHashes(a, k);
    std::vector<MatchSegment> found;

    const size_t n = a.size();
    const size_t m = b.size();
    // coveredA[i] 表示 A 的第 i 个字符已经落入某个已发现的长片段：
    // 这既避免了在完全相同文本上做 O(n^2) 的重复扩展，又保证不会重复记录同一处内容。
    std::vector<char> coveredA(n, 0);
    for (size_t i = 0; i < hashesA.size(); ++i) {
        if (coveredA[i]) continue;
        auto it = index.find(hashesA[i]);
        if (it == index.end()) continue;
        for (uint32_t j : it->second) {
            if (found.size() > 40000) break;  // 极端输入的安全阀
            size_t jj = j;
            // 向左扩展（上限 1024 字符，避免病态输入导致的平方复杂度）
            size_t left = 0;
            while (i > left && jj > left && left < 1024 && a[i - left - 1] == b[jj - left - 1]) {
                ++left;
            }
            // 向右扩展
            size_t beginA = i - left;
            size_t beginB = jj - left;
            size_t len = left + k;
            while (beginA + len < n && beginB + len < m && a[beginA + len] == b[beginB + len]) {
                ++len;
            }
            if (len < k) continue;
            MatchSegment seg;
            seg.aBegin = beginA;
            seg.aEnd = beginA + len;
            seg.bBegin = beginB;
            seg.bEnd = beginB + len;
            found.push_back(seg);
            // 足够长的匹配把覆盖区间标记出来，后续种子直接跳过
            if (len >= k * 3) {
                for (size_t c = beginA; c < beginA + len; ++c) coveredA[c] = 1;
            }
        }
        if (found.size() > 40000) break;
    }

    // ---- 第三步：去重 + 贪心挑选互不重叠的长片段（用于展示）----
    std::sort(found.begin(), found.end(), [](const MatchSegment& x, const MatchSegment& y) {
        if (x.length() != y.length()) return x.length() > y.length();
        if (x.aBegin != y.aBegin) return x.aBegin < y.aBegin;
        return x.bBegin < y.bBegin;
    });

    std::vector<Interval> acceptedA;
    for (const MatchSegment& seg : found) {
        Interval ia(seg.aBegin, seg.aEnd);
        bool clash = false;
        for (const Interval& other : acceptedA) {
            if (overlaps(ia, other)) { clash = true; break; }
        }
        if (clash) continue;
        acceptedA.push_back(ia);
        if (stats.segments.size() < static_cast<size_t>(options.maxSegments)) {
            stats.segments.push_back(seg);
        }
    }

    // ---- 第四步：覆盖率（用全部命中片段合并，不因展示数量上限而丢失统计）----
    std::vector<Interval> allA;
    std::vector<Interval> allB;
    allA.reserve(found.size());
    allB.reserve(found.size());
    for (const MatchSegment& seg : found) {
        allA.emplace_back(seg.aBegin, seg.aEnd);
        allB.emplace_back(seg.bBegin, seg.bEnd);
    }
    std::vector<Interval> mergedA = mergeIntervals(allA);
    std::vector<Interval> mergedB = mergeIntervals(allB);
    stats.dupCharsA = totalLength(mergedA);
    stats.dupCharsB = totalLength(mergedB);
    stats.coverageA = stats.dupCharsA;
    stats.dupIntervalsA = mergedA;
    stats.dupIntervalsB = mergedB;
    return stats;
}

} // namespace pc
