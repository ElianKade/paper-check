// =============================================================================
//  text.cpp —— 文本规范化与偏移映射的实现
// =============================================================================
#include "text.h"

#include <cctype>

#include "common.h"

namespace pc {

bool isContentChar(uint32_t cp) {
    // ASCII 字母与数字
    if ((cp >= '0' && cp <= '9') || (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z')) return true;
    // 中日韩统一表意文字（含扩展 A 区）
    if (cp >= 0x3400 && cp <= 0x4DBF) return true;
    if (cp >= 0x4E00 && cp <= 0x9FFF) return true;
    // 中日韩兼容表意文字
    if (cp >= 0xF900 && cp <= 0xFAFF) return true;
    // 扩展 B 区及以后（生僻字）
    if (cp >= 0x20000 && cp <= 0x2FA1F) return true;
    // 日文假名、韩文（便于处理混排文本）
    if (cp >= 0x3040 && cp <= 0x30FF) return true;
    if (cp >= 0xAC00 && cp <= 0xD7AF) return true;
    return false;
}

NormalizedText normalizeText(const std::string& raw) {
    NormalizedText result;
    result.original = raw;
    result.norm.reserve(raw.size());
    result.chars.reserve(raw.size() / 2);
    result.offsets.reserve(raw.size() / 2);
    result.ends.reserve(raw.size() / 2);

    size_t pos = 0;
    while (pos < raw.size()) {
        size_t begin = pos;
        uint32_t cp = utf8Next(raw, pos);

        // 全角 ASCII（！ 到 ～）转半角
        if (cp >= 0xFF01 && cp <= 0xFF5E) cp -= 0xFEE0;
        // 全角空格转普通空格
        else if (cp == 0x3000) cp = ' ';
        // 常见的“弯引号/破折号”等统一成等价半角符号，便于后续丢弃
        else if (cp == 0x2018 || cp == 0x2019 || cp == 0x201C || cp == 0x201D) cp = '\'';
        else if (cp == 0x2013 || cp == 0x2014) cp = '-';

        // 英文统一小写
        if (cp >= 'A' && cp <= 'Z') cp = cp - 'A' + 'a';

        if (!isContentChar(cp)) continue;  // 标点、空格、换行等直接丢弃

        result.chars.push_back(cp);
        result.offsets.push_back(static_cast<uint32_t>(begin));
        result.ends.push_back(static_cast<uint32_t>(pos));
        utf8Append(result.norm, cp);
    }
    return result;
}

size_t countCodePoints(const std::string& s) {
    size_t count = 0;
    size_t pos = 0;
    while (pos < s.size()) {
        utf8Next(s, pos);
        ++count;
    }
    return count;
}

std::string sliceUtf8(const std::string& s, size_t begin, size_t end) {
    if (begin >= s.size()) return std::string();
    if (end > s.size()) end = s.size();
    if (begin >= end) return std::string();
    // 起点向前修正到码点起始字节
    while (begin > 0 && (static_cast<unsigned char>(s[begin]) & 0xC0) == 0x80) --begin;
    // 终点向后修正到码点边界
    while (end < s.size() && (static_cast<unsigned char>(s[end]) & 0xC0) == 0x80) ++end;
    return s.substr(begin, end - begin);
}

int lineOfOffset(const std::string& s, size_t offset) {
    int line = 1;
    size_t limit = offset < s.size() ? offset : s.size();
    for (size_t i = 0; i < limit; ++i) {
        if (s[i] == '\n') ++line;
    }
    return line;
}

std::string squeezeSpaces(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool lastSpace = false;
    for (char c : s) {
        bool isSpace = (c == ' ' || c == '\t' || c == '\r' || c == '\n');
        if (isSpace) {
            if (!lastSpace && !out.empty()) out.push_back(' ');
            lastSpace = true;
        } else {
            out.push_back(c);
            lastSpace = false;
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

std::pair<size_t, size_t> normRangeToOriginal(const NormalizedText& text, size_t normBegin,
                                              size_t normEnd) {
    if (text.offsets.empty() || normBegin >= text.offsets.size() || normBegin >= normEnd) {
        return {0, 0};
    }
    if (normEnd > text.offsets.size()) normEnd = text.offsets.size();
    size_t begin = text.offsets[normBegin];
    size_t end = text.ends[normEnd - 1];
    // 把紧跟在后面的标点/空白一并纳入，展示时更自然；但遇到换行就停下
    while (end < text.original.size()) {
        unsigned char c = static_cast<unsigned char>(text.original[end]);
        if (c == '\n' || c == '\r') break;
        if (c < 0x80) { ++end; continue; }
        // 中文标点（3 字节）也可以并入，遇到下一个内容字符则停止
        size_t probe = end;
        uint32_t cp = utf8Next(text.original, probe);
        if (isContentChar(cp)) break;
        end = probe;
    }
    return {begin, end};
}

} // namespace pc
