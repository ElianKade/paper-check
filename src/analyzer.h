// =============================================================================
//  analyzer.h —— 查重分析编排层：解析文档、两两比对、汇总查重率、生成 JSON
//
//  这一层负责“业务”：把文件读成纯文本，规范化为内容字符序列，调用 similarity
//  做两两比对，再把每篇文档与所有其他文档的重复内容取并集，得到该文档的查重率。
// =============================================================================
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "similarity.h"

namespace pc {

// 输入文档（由服务器或命令行准备好：文件名 + 类型 + 已抽取的纯文本）
struct DocumentInput {
    std::string name;   // 原始文件名（含扩展名）
    std::string type;   // 文件类型：docx / pdf / txt
    std::string text;   // 抽取出的纯文本（UTF-8）
};

// 用于前端展示的一处重复片段：给出两侧的上下文、匹配文本与行号
struct SegmentView {
    int aLine = 0;             // A 文档中匹配内容的起始行号（1 起）
    int bLine = 0;             // B 文档中匹配内容的起始行号（1 起）
    size_t chars = 0;          // 匹配的规范化字符数
    std::string aBefore;       // A 侧匹配文本之前的一段上下文
    std::string aMatch;        // A 侧匹配文本
    std::string aAfter;        // A 侧匹配文本之后的一段上下文
    std::string bBefore;
    std::string bMatch;
    std::string bAfter;
};

// 两篇文档的比对报告
struct PairReport {
    int a = 0;                    // 文档下标（小者）
    int b = 0;                    // 文档下标（大者）
    double similarity = 0.0;      // 综合相似度（两侧覆盖率的平均）
    double rateA = 0.0;           // A 文被 B 文重复的比例
    double rateB = 0.0;           // B 文被 A 文重复的比例
    double jaccard = 0.0;
    double containment = 0.0;
    double minhash = 0.0;
    int simhashDistance = 64;
    size_t dupChars = 0;          // 两侧重复字符数的较小值
    std::vector<SegmentView> segments;
};

// 单篇文档的查重结果
struct DocumentReport {
    int index = 0;
    std::string name;
    std::string type;
    size_t chars = 0;          // 规范化内容字符数
    size_t dupChars = 0;       // 与其他文档重复的字符数（多处重复只算一次）
    double dupRate = 0.0;      // 查重率 = dupChars / chars
    int level = 0;             // 风险等级 0~3
    std::string levelName;     // 等级名称
    double maxPairRate = 0.0;  // 与单篇文档的最高重复比例
    int maxPairIndex = -1;     // 该篇文档在 pairs 中的下标
    std::vector<int> pairIds;  // 涉及该文档的所有 pair 下标
};

// 分析参数
struct AnalysisOptions {
    MatchOptions match;             // 比对参数
    int contextChars = 24;          // 片段展示时左右各取多少字符
    size_t maxSegmentsPerPair = 8;  // 每对文档展示多少个重复片段
    std::vector<int> levelThresholds{15, 25, 40};  // 分界（百分比）
    std::vector<std::string> levelNames{"安全", "警示", "危险", "高危"};
};

// 完整分析结果
struct AnalysisReport {
    std::vector<DocumentReport> documents;
    std::vector<PairReport> pairs;   // 按相似度降序
    double elapsedMs = 0.0;
    size_t totalChars = 0;
    bool ok = true;
    std::string error;
    // 解析失败的文件（文件名: 原因），不影响其他文档继续比对
    std::vector<std::string> failures;
};

// 按扩展名判断文档类型；无法识别时返回空串
std::string detectDocType(const std::string& name);

// 读取单个文件并抽取纯文本；失败时返回 false 并写 error
bool loadDocumentText(const std::string& path, std::string& text, std::string& error);

// 对一批文档做查重分析
AnalysisReport analyzeDocuments(const std::vector<DocumentInput>& inputs,
                                const AnalysisOptions& options);

// 把分析结果序列化为 JSON（前端直接消费）
std::string reportToJson(const AnalysisReport& report);

} // namespace pc
