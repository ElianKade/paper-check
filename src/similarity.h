// =============================================================================
//  similarity.h —— 文档内容比对：公共片段查找与覆盖率统计
//
//  思路（种子 + 扩展）：
//    1. 把 B 文所有长度为 k 的字符片段（k = minMatch）建成哈希表：哈希 -> 出现位置；
//    2. 顺序扫描 A 文，用同样的滚动哈希查表，命中即为“种子”；
//    3. 从种子向左右两侧扩展，得到一段最长公共子串（这就是一处重复内容）；
//    4. 对命中区间做贪心筛选（长的优先、区间不重叠），再去重合并，得到覆盖率。
//  覆盖率 = 被重复内容覆盖的字符数 / 全文内容字符数，即“总文字复制比”。
// =============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace pc {

// 规范化下标区间 [begin, end)
using Interval = std::pair<size_t, size_t>;

// 一处匹配片段（A、B 两侧的规范化下标区间）
struct MatchSegment {
    size_t aBegin = 0;
    size_t aEnd = 0;
    size_t bBegin = 0;
    size_t bEnd = 0;

    size_t length() const { return aEnd - aBegin; }
};

// 一对文档的完整比对结果
struct PairStats {
    double jaccard = 0.0;        // k-gram 集合 Jaccard
    double containment = 0.0;    // k-gram 集合包含度（交集 / 较小集合）
    double minhashSim = 0.0;     // MinHash 估计的 Jaccard
    int simhashDistance = 64;    // SimHash 汉明距离
    size_t dupCharsA = 0;        // A 中被重复内容覆盖的字符数
    size_t dupCharsB = 0;        // B 中被重复内容覆盖的字符数
    size_t coverageA = 0;        // A 的 k-gram 覆盖率（0~1 的分子）
    size_t totalA = 0;
    size_t totalB = 0;
    std::vector<Interval> dupIntervalsA;  // A 侧去重合并后的重复区间（用于文档级求并集）
    std::vector<Interval> dupIntervalsB;  // B 侧去重合并后的重复区间
    std::vector<MatchSegment> segments;  // 长度降序的匹配片段
};

// 比对参数
struct MatchOptions {
    int shingleK = 13;        // 集合表示 / 相似度估计使用的 k
    int minMatch = 20;        // 认定为“重复内容”的最短长度（内容字符数）
    int maxSegments = 80;     // 每对文档最多保留的匹配片段数
    int maxCandidates = 24;   // 单个种子哈希最多展开的候选位置数
};

// 合并重叠区间，返回互不重叠的区间（升序）
std::vector<Interval> mergeIntervals(std::vector<Interval> intervals);

// 计算若干互不重叠区间的总长度
size_t totalLength(const std::vector<Interval>& intervals);

// 比对两篇文档的规范化码点序列，给出相似度与重复片段
PairStats compareDocuments(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b,
                           const MatchOptions& options);

} // namespace pc
