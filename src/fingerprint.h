// =============================================================================
//  fingerprint.h —— 文本指纹：k-gram 哈希、SimHash、MinHash
//
//  查重系统里“表示”的选择直接决定了算法效率：
//    * k-gram 哈希：把连续 k 个内容字符映射为 64 位整数，是后续所有比较的基础；
//    * 集合表示：一篇文章 = 一个 k-gram 哈希集合（去重、排序后可用归并求交集）；
//    * SimHash：把集合压缩成 64 位指纹，用汉明距离快速判断“整体是否雷同”；
//    * MinHash：把集合压缩成 N 个最小哈希值，可近似 Jaccard 相似度并支持 LSH 分桶。
//
//  这些结构都只是“表示”，具体判定规则在 similarity.cpp 中。
// =============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pc {

// 计算字节串的 64 位哈希（FNV-1a 变体再混合，速度快且分布均匀）
uint64_t hashBytes64(const void* data, size_t len);

// 对码点序列做 k-gram 滚动哈希，返回长度为 n-k+1 的哈希数组（n < k 时为空）
std::vector<uint64_t> gramHashes(const std::vector<uint32_t>& chars, int k);

// 对哈希数组去重并升序排序，得到“集合表示”
std::vector<uint64_t> uniqueSorted(std::vector<uint64_t> hashes);

// 两个已排序集合的交集大小
size_t intersectionSize(const std::vector<uint64_t>& a, const std::vector<uint64_t>& b);

// Jaccard 相似度 = |A∩B| / |A∪B|
double jaccardSimilarity(const std::vector<uint64_t>& a, const std::vector<uint64_t>& b);

// 包含度 = |A∩B| / min(|A|,|B|)，对“长文抄短文”的场景更敏感
double containmentSimilarity(const std::vector<uint64_t>& a, const std::vector<uint64_t>& b);

// SimHash：返回 64 位指纹，两篇文章指纹的汉明距离越小越相似
uint64_t simhash64(const std::vector<uint64_t>& hashes);

// 两个 64 位指纹的汉明距离（0~64）
int hammingDistance64(uint64_t a, uint64_t b);

// MinHash 签名：N 个最小值，可近似估计 Jaccard
struct MinHashSignature {
    std::vector<uint64_t> values;
};

// 生成 MinHash 签名（numPerm 个独立置换，使用预置随机系数）
MinHashSignature minhashSignature(const std::vector<uint64_t>& hashes, int numPerm);

// 由两个签名估计 Jaccard 相似度
double minhashSimilarity(const MinHashSignature& a, const MinHashSignature& b);

// 计算 LSH 分桶签名中某一 band 的桶号（用于候选对筛选）
uint32_t lshBandKey(const MinHashSignature& sig, int band, int rowsPerBand);

} // namespace pc
