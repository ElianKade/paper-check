// =============================================================================
//  text.h —— 文本预处理：归一化、偏移映射、片段截取
//
//  查重的第一步是“规范化”：把全角字符转成半角、英文统一小写、去掉标点与空白，
//  只保留真正参与比较的“内容字符”（中文、字母、数字）。
//  同时记录每个规范化字符在原文中的字节偏移，这样命中重复片段时才能回到原文
//  定位并展示原始文字。
// =============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace pc {

// 规范化文本及其与原文的对应关系
struct NormalizedText {
    std::string norm;               // 规范化后的文本（UTF-8）
    std::vector<uint32_t> chars;    // 规范化文本的码点序列（供 k-gram 哈希使用）
    std::vector<uint32_t> offsets;  // chars[i] 在原文中的起始字节偏移
    std::vector<uint32_t> ends;     // chars[i] 在原文中的结束字节偏移（不含）
    std::string original;           // 原文（用于回溯展示）
};

// 文本规范化：全角 -> 半角、英文小写、丢弃标点与空白。
// 返回的 NormalizedText 中 chars 与 offsets 一一对应。
NormalizedText normalizeText(const std::string& raw);

// 判断码点是否是需要保留的内容字符（字母 / 数字 / 中日韩文字）。
bool isContentChar(uint32_t cp);

// 取 UTF-8 字符串的码点总数。
size_t countCodePoints(const std::string& s);

// 按字节区间截取 UTF-8 字符串（自动修正到码点边界，避免截断半个字符）。
std::string sliceUtf8(const std::string& s, size_t begin, size_t end);

// 计算字节偏移所在的行号（行号从 1 开始）。
int lineOfOffset(const std::string& s, size_t offset);

// 把连续空白压缩为单个空格，便于结果展示。
std::string squeezeSpaces(const std::string& s);

// 把规范化区间转换回原文区间：返回 [原文起始字节, 原文结束字节)。
// 若区间为空则返回 {0, 0}。
std::pair<size_t, size_t> normRangeToOriginal(const NormalizedText& text, size_t normBegin,
                                              size_t normEnd);

} // namespace pc
