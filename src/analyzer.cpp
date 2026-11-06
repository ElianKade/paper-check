// =============================================================================
//  analyzer.cpp —— 查重分析编排层实现
// =============================================================================
#include "analyzer.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>

#include "common.h"
#include "docx.h"
#include "fingerprint.h"
#include "pdf.h"
#include "text.h"

namespace pc {
namespace {

// 取文件名最后一段，避免路径干扰类型判断
std::string fileNameOf(const std::string& name) {
    size_t pos = name.find_last_of("/\\");
    return pos == std::string::npos ? name : name.substr(pos + 1);
}

// 取小写扩展名（不含点）
std::string extensionOf(const std::string& name) {
    std::string base = fileNameOf(name);
    size_t pos = base.find_last_of('.');
    if (pos == std::string::npos) return std::string();
    return toLowerAscii(base.substr(pos + 1));
}

// 规范化下标 -> 原文字节偏移（index 可等于 chars.size()，返回原文长度）
size_t byteOffsetOf(const NormalizedText& text, size_t index) {
    if (index >= text.offsets.size()) return text.original.size();
    return text.offsets[index];
}

// 截取规范化区间对应的原文，并把换行等空白压缩成单空格，便于展示
std::string contextOf(const NormalizedText& text, size_t normBegin, size_t normEnd) {
    if (normBegin >= normEnd || normBegin >= text.chars.size()) return std::string();
    if (normEnd > text.chars.size()) normEnd = text.chars.size();
    size_t beginByte = byteOffsetOf(text, normBegin);
    size_t endByte = text.ends[normEnd - 1];
    if (endByte <= beginByte) return std::string();
    return squeezeSpaces(text.original.substr(beginByte, endByte - beginByte));
}

// 计算规范化字符下标所在行号
int lineAt(const NormalizedText& text, size_t index) {
    if (text.offsets.empty()) return 1;
    if (index >= text.offsets.size()) index = text.offsets.size() - 1;
    return lineOfOffset(text.original, text.offsets[index]);
}

// 小数点后固定位数的字符串（避免 std::to_string 输出过长尾数）
std::string fixed(double value, int digits) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", digits, value);
    return std::string(buf);
}

// 生成一处重复片段的展示数据
SegmentView makeSegmentView(const NormalizedText& a, const NormalizedText& b,
                            const MatchSegment& seg, int contextChars) {
    SegmentView view;
    view.chars = seg.length();
    const size_t ctx = contextChars > 0 ? static_cast<size_t>(contextChars) : 0;
    // 片段过长时只展示开头一部分，避免大段相同内容把 JSON 撑得过大
    const size_t kMaxShown = 320;
    const bool truncated = seg.length() > kMaxShown;
    const size_t shownLen = truncated ? kMaxShown : seg.length();

    size_t aBegin = seg.aBegin;
    size_t aShownEnd = seg.aBegin + shownLen;
    size_t beforeBegin = aBegin > ctx ? aBegin - ctx : 0;
    size_t afterEnd = std::min(a.chars.size(), aShownEnd + ctx);
    view.aBefore = contextOf(a, beforeBegin, aBegin);
    view.aMatch = contextOf(a, aBegin, aShownEnd);
    if (truncated) view.aMatch += "……";
    view.aAfter = contextOf(a, aShownEnd, afterEnd);
    view.aLine = lineAt(a, aBegin);

    size_t bBegin = seg.bBegin;
    size_t bShownEnd = seg.bBegin + shownLen;
    size_t bBeforeBegin = bBegin > ctx ? bBegin - ctx : 0;
    size_t bAfterEnd = std::min(b.chars.size(), bShownEnd + ctx);
    view.bBefore = contextOf(b, bBeforeBegin, bBegin);
    view.bMatch = contextOf(b, bBegin, bShownEnd);
    if (truncated) view.bMatch += "……";
    view.bAfter = contextOf(b, bShownEnd, bAfterEnd);
    view.bLine = lineAt(b, bBegin);
    return view;
}

// 依据查重率（0~1）判定风险等级
int levelOf(double rate, const std::vector<int>& thresholds) {
    double percent = rate * 100.0;
    int level = 0;
    for (int t : thresholds) {
        if (percent >= static_cast<double>(t)) ++level;
    }
    return level;
}

}  // namespace

std::string detectDocType(const std::string& name) {
    std::string ext = extensionOf(name);
    if (ext == "docx") return "docx";
    if (ext == "pdf") return "pdf";
    if (ext == "txt" || ext == "md") return "txt";
    return std::string();
}

bool loadDocumentText(const std::string& path, std::string& text, std::string& error) {
    std::string bytes;
    if (!readFileBytes(path, bytes, error)) return false;
    if (bytes.empty()) {
        error = "文件内容为空";
        return false;
    }
    std::string type = detectDocType(path);
    if (type == "docx") {
        return extractDocxText(bytes, text, error);
    }
    if (type == "pdf") {
        return extractPdfText(bytes, text, error);
    }
    if (type == "txt") {
        // 处理 UTF-8 BOM
        if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
            static_cast<unsigned char>(bytes[1]) == 0xBB && static_cast<unsigned char>(bytes[2]) == 0xBF) {
            text = bytes.substr(3);
        } else {
            text = bytes;
        }
        return true;
    }
    error = "不支持的文件类型（仅支持 .docx / .pdf / .txt）";
    return false;
}

AnalysisReport analyzeDocuments(const std::vector<DocumentInput>& inputs,
                                const AnalysisOptions& options) {
    AnalysisReport report;
    Stopwatch watch;

    const size_t count = inputs.size();
    if (count < 2) {
        report.ok = false;
        report.error = "至少需要两篇文档才能比对";
        report.elapsedMs = watch.elapsedMs();
        return report;
    }

    // ---- 1) 规范化：每篇文档得到“内容字符序列 + 原文偏移映射” ----
    std::vector<NormalizedText> normalized(count);
    report.documents.resize(count);
    for (size_t i = 0; i < count; ++i) {
        normalized[i] = normalizeText(inputs[i].text);
        DocumentReport& doc = report.documents[i];
        doc.index = static_cast<int>(i);
        doc.name = inputs[i].name;
        doc.type = inputs[i].type;
        doc.chars = normalized[i].chars.size();
        doc.levelName = options.levelNames.empty() ? std::string() : options.levelNames[0];
        report.totalChars += doc.chars;
    }

    // ---- 2) 两两比对 ----
    // chunks[i] 收集第 i 篇文档在所有比对中被判定为重复的区间，最后求并集得到查重率
    std::vector<std::vector<Interval>> chunks(count);
    for (size_t i = 0; i < count; ++i) {
        for (size_t j = i + 1; j < count; ++j) {
            if (normalized[i].chars.empty() || normalized[j].chars.empty()) continue;
            PairStats stats = compareDocuments(normalized[i].chars, normalized[j].chars, options.match);
            if (stats.dupCharsA == 0 && stats.dupCharsB == 0) continue;

            PairReport pair;
            pair.a = static_cast<int>(i);
            pair.b = static_cast<int>(j);
            pair.jaccard = stats.jaccard;
            pair.containment = stats.containment;
            pair.minhash = stats.minhashSim;
            pair.simhashDistance = stats.simhashDistance;
            pair.dupChars = std::min(stats.dupCharsA, stats.dupCharsB);
            pair.rateA = stats.totalA == 0 ? 0.0
                                          : static_cast<double>(stats.dupCharsA) / static_cast<double>(stats.totalA);
            pair.rateB = stats.totalB == 0 ? 0.0
                                          : static_cast<double>(stats.dupCharsB) / static_cast<double>(stats.totalB);
            pair.similarity = (pair.rateA + pair.rateB) * 0.5;

            const size_t take = std::min(stats.segments.size(), options.maxSegmentsPerPair);
            pair.segments.reserve(take);
            for (size_t s = 0; s < take; ++s) {
                pair.segments.push_back(
                    makeSegmentView(normalized[i], normalized[j], stats.segments[s], options.contextChars));
            }

            // 文档级重复字符统计：把所有相关比对的重复区间汇总，最后求并集
            for (const Interval& it : stats.dupIntervalsA) chunks[i].push_back(it);
            for (const Interval& it : stats.dupIntervalsB) chunks[j].push_back(it);

            report.pairs.push_back(std::move(pair));
        }
    }

    // ---- 3) 计算每篇文档的查重率（对重复内容求并集，重复出现只计一次）----
    for (size_t i = 0; i < count; ++i) {
        DocumentReport& doc = report.documents[i];
        std::vector<Interval> merged = mergeIntervals(chunks[i]);
        doc.dupChars = totalLength(merged);
        doc.dupRate = doc.chars == 0 ? 0.0 : static_cast<double>(doc.dupChars) / static_cast<double>(doc.chars);
        doc.level = levelOf(doc.dupRate, options.levelThresholds);
        if (doc.level < static_cast<int>(options.levelNames.size())) {
            doc.levelName = options.levelNames[static_cast<size_t>(doc.level)];
        }
    }

    // ---- 4) 排序与文档关联 ----
    std::sort(report.pairs.begin(), report.pairs.end(), [](const PairReport& x, const PairReport& y) {
        if (x.similarity != y.similarity) return x.similarity > y.similarity;
        if (x.a != y.a) return x.a < y.a;
        return x.b < y.b;
    });

    for (size_t p = 0; p < report.pairs.size(); ++p) {
        const PairReport& pair = report.pairs[p];
        for (int docIndex : {pair.a, pair.b}) {
            DocumentReport& doc = report.documents[static_cast<size_t>(docIndex)];
            doc.pairIds.push_back(static_cast<int>(p));
            double rate = docIndex == pair.a ? pair.rateA : pair.rateB;
            if (rate > doc.maxPairRate) {
                doc.maxPairRate = rate;
                doc.maxPairIndex = static_cast<int>(p);
            }
        }
    }

    report.elapsedMs = watch.elapsedMs();
    return report;
}

std::string reportToJson(const AnalysisReport& report) {
    std::string out;
    out.reserve(8192);
    out += "{\"ok\":";
    out += report.ok ? "true" : "false";
    out += ",\"error\":\"";
    out += jsonEscape(report.error);
    out += "\",\"elapsedMs\":";
    out += fixed(report.elapsedMs, 1);
    out += ",\"totalChars\":";
    out += std::to_string(report.totalChars);
    out += ",\"failures\":[";
    for (size_t i = 0; i < report.failures.size(); ++i) {
        if (i) out += ',';
        out += "\"";
        out += jsonEscape(report.failures[i]);
        out += "\"";
    }
    out += "]";
    out += ",\"documents\":[";
    for (size_t i = 0; i < report.documents.size(); ++i) {
        const DocumentReport& doc = report.documents[i];
        if (i) out += ',';
        out += "{\"index\":";
        out += std::to_string(doc.index);
        out += ",\"name\":\"";
        out += jsonEscape(doc.name);
        out += "\",\"type\":\"";
        out += jsonEscape(doc.type);
        out += "\",\"chars\":";
        out += std::to_string(doc.chars);
        out += ",\"dupChars\":";
        out += std::to_string(doc.dupChars);
        out += ",\"dupRate\":";
        out += fixed(doc.dupRate, 6);
        out += ",\"level\":";
        out += std::to_string(doc.level);
        out += ",\"levelName\":\"";
        out += jsonEscape(doc.levelName);
        out += "\",\"maxPairRate\":";
        out += fixed(doc.maxPairRate, 6);
        out += ",\"maxPairIndex\":";
        out += std::to_string(doc.maxPairIndex);
        out += ",\"pairIds\":[";
        for (size_t k = 0; k < doc.pairIds.size(); ++k) {
            if (k) out += ',';
            out += std::to_string(doc.pairIds[k]);
        }
        out += "]}";
    }
    out += "],\"pairs\":[";
    for (size_t i = 0; i < report.pairs.size(); ++i) {
        const PairReport& pair = report.pairs[i];
        if (i) out += ',';
        out += "{\"id\":";
        out += std::to_string(i);
        out += ",\"a\":";
        out += std::to_string(pair.a);
        out += ",\"b\":";
        out += std::to_string(pair.b);
        out += ",\"similarity\":";
        out += fixed(pair.similarity, 6);
        out += ",\"rateA\":";
        out += fixed(pair.rateA, 6);
        out += ",\"rateB\":";
        out += fixed(pair.rateB, 6);
        out += ",\"jaccard\":";
        out += fixed(pair.jaccard, 6);
        out += ",\"containment\":";
        out += fixed(pair.containment, 6);
        out += ",\"minhash\":";
        out += fixed(pair.minhash, 6);
        out += ",\"simhashDistance\":";
        out += std::to_string(pair.simhashDistance);
        out += ",\"dupChars\":";
        out += std::to_string(pair.dupChars);
        out += ",\"segments\":[";
        for (size_t s = 0; s < pair.segments.size(); ++s) {
            const SegmentView& seg = pair.segments[s];
            if (s) out += ',';
            out += "{\"chars\":";
            out += std::to_string(seg.chars);
            out += ",\"aLine\":";
            out += std::to_string(seg.aLine);
            out += ",\"bLine\":";
            out += std::to_string(seg.bLine);
            out += ",\"aBefore\":\"";
            out += jsonEscape(seg.aBefore);
            out += "\",\"aMatch\":\"";
            out += jsonEscape(seg.aMatch);
            out += "\",\"aAfter\":\"";
            out += jsonEscape(seg.aAfter);
            out += "\",\"bBefore\":\"";
            out += jsonEscape(seg.bBefore);
            out += "\",\"bMatch\":\"";
            out += jsonEscape(seg.bMatch);
            out += "\",\"bAfter\":\"";
            out += jsonEscape(seg.bAfter);
            out += "\"}";
        }
        out += "]}";
    }
    out += "]}";
    return out;
}

} // namespace pc
