// =============================================================================
//  analyze_test.cpp —— 命令行走查重流程（开发期自测用）
//
//  用法：analyze_test.exe <任务文件> <输出 JSON 路径>
//  任务文件为 UTF-8 文本，每行一个文档路径（避免命令行中文参数乱码问题）。
// =============================================================================
#include <cstdio>
#include <string>
#include <vector>

#include "analyzer.h"
#include "common.h"
#include "text.h"

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: analyze_test.exe <task-file> <out-json>\n");
        return 2;
    }
    std::string listBytes;
    std::string err;
    if (!pc::readFileBytes(argv[1], listBytes, err)) {
        std::printf("read task file failed: %s\n", err.c_str());
        return 1;
    }
    std::vector<std::string> paths;
    std::string current;
    for (char c : listBytes) {
        if (c == '\r') continue;
        if (c == '\n') {
            if (!current.empty()) paths.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) paths.push_back(current);

    std::vector<pc::DocumentInput> inputs;
    for (const std::string& path : paths) {
        pc::DocumentInput doc;
        size_t pos = path.find_last_of("/\\");
        doc.name = pos == std::string::npos ? path : path.substr(pos + 1);
        doc.type = pc::detectDocType(doc.name);
        std::string extractError;
        if (!pc::loadDocumentText(path, doc.text, extractError)) {
            std::printf("load failed: %s (%s)\n", doc.name.c_str(), extractError.c_str());
            continue;
        }
        inputs.push_back(doc);
        std::printf("loaded %s type=%s bytes=%zu\n", doc.name.c_str(), doc.type.c_str(),
                    doc.text.size());
    }
    if (inputs.size() < 2) {
        std::printf("need at least 2 documents\n");
        return 1;
    }

    pc::AnalysisOptions options;
    pc::AnalysisReport report = pc::analyzeDocuments(inputs, options);
    std::string json = pc::reportToJson(report);
    if (!pc::writeFileBytes(argv[2], json, err)) {
        std::printf("write json failed: %s\n", err.c_str());
        return 1;
    }
    std::printf("elapsed=%.1fms totalChars=%zu pairs=%zu json=%zu bytes\n", report.elapsedMs,
                report.totalChars, report.pairs.size(), json.size());
    for (const pc::DocumentReport& doc : report.documents) {
        std::printf("  [%d] %s chars=%zu dup=%zu rate=%.2f%% level=%s maxPair=%.2f%%\n", doc.index,
                    doc.name.c_str(), doc.chars, doc.dupChars, doc.dupRate * 100.0,
                    doc.levelName.c_str(), doc.maxPairRate * 100.0);
    }
    for (const pc::PairReport& pair : report.pairs) {
        std::printf("  pair %d-%d sim=%.2f%% rateA=%.2f%% rateB=%.2f%% J=%.3f C=%.3f MH=%.3f "
                    "simhash=%d segs=%zu\n",
                    pair.a, pair.b, pair.similarity * 100.0, pair.rateA * 100.0, pair.rateB * 100.0,
                    pair.jaccard, pair.containment, pair.minhash, pair.simhashDistance,
                    pair.segments.size());
    }
    return 0;
}
