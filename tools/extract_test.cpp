// =============================================================================
//  extract_test.cpp —— 文档文本提取的临时测试工具
//  用法: extract_test.exe <任务文件>
//  任务文件为 UTF-8 文本，每行一个待提取的文档路径；提取结果依次写入
//  <任务文件>.0.txt、<任务文件>.1.txt ...，并打印统计信息（ASCII）。
//  这样可以避免在命令行直接传中文路径导致的编码问题。
// =============================================================================
#include "../src/common.h"
#include "../src/docx.h"
#include "../src/pdf.h"

#include <cstdio>
#include <string>
#include <vector>

#include <shellapi.h>

int main() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc < 2) {
        std::printf("usage: extract_test <taskfile>\n");
        return 2;
    }
    std::string taskPath = pc::toUtf8(argv[1]);
    LocalFree(argv);

    std::string list, err;
    if (!pc::readFileBytes(taskPath, list, err)) {
        std::printf("read task file failed: %s\n", err.c_str());
        return 1;
    }

    // 按行拆分任务
    std::vector<std::string> paths;
    {
        std::string cur;
        for (size_t i = 0; i < list.size(); ++i) {
            char c = list[i];
            if (c == '\n') {
                if (!cur.empty()) paths.push_back(cur);
                cur.clear();
            } else if (c != '\r') {
                cur.push_back(c);
            }
        }
        if (!cur.empty()) paths.push_back(cur);
    }

    int index = 0;
    for (const std::string& inPath : paths) {
        std::string bytes;
        pc::Stopwatch sw;
        if (!pc::readFileBytes(inPath, bytes, err)) {
            std::printf("[%d] read failed: %s\n", index, err.c_str());
            ++index;
            continue;
        }

        std::string text;
        bool ok = false;
        if (pc::endsWithNoCase(inPath, ".pdf")) {
            ok = pc::extractPdfText(bytes, text, err);
        } else if (pc::endsWithNoCase(inPath, ".docx")) {
            ok = pc::extractDocxText(bytes, text, err);
        } else {
            std::printf("[%d] unsupported type\n", index);
            ++index;
            continue;
        }

        if (!ok) {
            std::printf("[%d] extract failed (%.1fms): %s\n", index, sw.elapsedMs(), err.c_str());
            ++index;
            continue;
        }

        char outName[64];
        std::snprintf(outName, sizeof(outName), ".%d.txt", index);
        std::string outPath = taskPath + outName;
        std::string werr;
        if (!pc::writeFileBytes(outPath, text, werr)) {
            std::printf("[%d] write failed: %s\n", index, werr.c_str());
            ++index;
            continue;
        }

        int lines = 0;
        for (char c : text) {
            if (c == '\n') ++lines;
        }
        size_t cps = 0;
        {
            size_t p = 0;
            while (p < text.size()) {
                pc::utf8Next(text, p);
                ++cps;
            }
        }
        std::printf("[%d] ok bytes=%zu chars=%zu lines=%d time=%.1fms\n", index, text.size(), cps,
                    lines, sw.elapsedMs());
        ++index;
    }
    return 0;
}
