// =============================================================================
//  main.cpp —— 程序入口：Web 服务模式与命令行模式
//
//  用法：
//    pc_check.exe                       启动 Web 服务（默认 http://127.0.0.1:8080）
//    pc_check.exe --port 9000           指定端口
//    pc_check.exe --cli a.docx b.pdf    命令行直接查重并打印报告
// =============================================================================
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "analyzer.h"
#include "common.h"
#include "docx.h"
#include "http_server.h"
#include "pdf.h"
#include "text.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <shellapi.h>
#include <windows.h>

namespace {

// 用 Unicode 版本重新解析命令行参数，避免中文路径在 GBK 代码页下被截断或乱码
std::vector<std::string> unicodeArguments(int argc, char** argv) {
    std::vector<std::string> args;
    int wideCount = 0;
    LPWSTR* wideArgs = ::CommandLineToArgvW(::GetCommandLineW(), &wideCount);
    if (wideArgs) {
        for (int i = 0; i < wideCount; ++i) args.push_back(pc::toUtf8(wideArgs[i]));
        ::LocalFree(wideArgs);
        if (!args.empty()) return args;
    }
    for (int i = 0; i < argc; ++i) args.push_back(argv[i] ? argv[i] : "");
    return args;
}

// 应用配置：Web 资源目录与示例语料目录
struct AppConfig {
    std::string webDir;
    std::string samplesDir;
    std::string host;
    int port = 8080;
};

// 在候选目录中查找包含指定文件 / 子目录的位置
std::string findExistingDir(const std::vector<std::string>& candidates, const std::string& probeFile) {
    for (const std::string& candidate : candidates) {
        if (probeFile.empty()) {
            if (pc::isDirectory(candidate)) return candidate;
        } else {
            if (pc::isDirectory(candidate) && pc::fileExists(pc::joinPath(candidate, probeFile))) {
                return candidate;
            }
        }
    }
    return std::string();
}

// 解析示例语料目录：服务启动后用户可能新建 samples/，因此每次请求时按需重新探测
std::string resolveSamplesDir(const AppConfig& config) {
    if (pc::isDirectory(config.samplesDir)) return config.samplesDir;
    std::string exeDir = pc::executableDir();
    std::string parentDir = pc::parentPath(exeDir);
    std::string found = findExistingDir({pc::joinPath(parentDir, "samples"), pc::joinPath(exeDir, "samples")}, "");
    return found.empty() ? config.samplesDir : found;
}

// 把字节数变成人类可读字符串
std::string humanSize(size_t bytes) {
    char buf[64];
    if (bytes >= 1024 * 1024) {
        std::snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else if (bytes >= 1024) {
        std::snprintf(buf, sizeof(buf), "%.1f KB", static_cast<double>(bytes) / 1024.0);
    } else {
        std::snprintf(buf, sizeof(buf), "%zu B", bytes);
    }
    return std::string(buf);
}

// 从查询串中取值（形如 a=1&b=2），未找到返回空串
std::string queryValue(const std::string& query, const std::string& key) {
    size_t pos = 0;
    while (pos < query.size()) {
        size_t amp = query.find('&', pos);
        std::string item = query.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        size_t eq = item.find('=');
        if (eq != std::string::npos && item.substr(0, eq) == key) return item.substr(eq + 1);
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return std::string();
}

// 解析整数参数并限制在合理区间
int intParam(const std::string& query, const std::string& key, int fallback, int low, int high) {
    std::string value = queryValue(query, key);
    if (value.empty()) return fallback;
    int parsed = std::atoi(value.c_str());
    if (parsed < low) return low;
    if (parsed > high) return high;
    return parsed;
}

// 列出示例语料目录下的文档
struct SampleInfo {
    std::string name;
    unsigned long long size = 0;
    std::string type;
};

std::vector<SampleInfo> listSamples(const std::string& dir) {
    std::vector<SampleInfo> result;
    if (!pc::isDirectory(dir)) return result;
    std::wstring pattern = pc::toWide(dir);
    if (!pattern.empty() && pattern.back() != L'\\' && pattern.back() != L'/') pattern += L'\\';
    pattern += L"*";

    WIN32_FIND_DATAW data;
    HANDLE handle = ::FindFirstFileW(pattern.c_str(), &data);
    if (handle == INVALID_HANDLE_VALUE) return result;
    do {
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::string name = pc::toUtf8(data.cFileName);
        std::string type = pc::detectDocType(name);
        if (type.empty()) continue;
        SampleInfo info;
        info.name = name;
        info.size = (static_cast<unsigned long long>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
        info.type = type;
        result.push_back(info);
    } while (::FindNextFileW(handle, &data));
    ::FindClose(handle);

    std::sort(result.begin(), result.end(),
              [](const SampleInfo& a, const SampleInfo& b) { return a.name < b.name; });
    return result;
}

// 校验示例文件名，防止目录穿越
bool isSafeSampleName(const std::string& name) {
    return !name.empty() && name.find("..") == std::string::npos && name.find('/') == std::string::npos &&
           name.find('\\') == std::string::npos;
}

// 把失败原因汇总成 JSON 数组项
void appendFailure(pc::AnalysisReport& report, const std::string& name, const std::string& reason) {
    report.failures.push_back(name + "：" + reason);
}

// 分析一组“已就绪”的文档输入
pc::AnalysisReport runAnalysis(const std::vector<pc::DocumentInput>& inputs, const std::string& query) {
    pc::AnalysisOptions options;
    options.match.minMatch = intParam(query, "minMatch", 20, 8, 80);
    options.match.shingleK = intParam(query, "shingleK", 13, 6, 30);
    return pc::analyzeDocuments(inputs, options);
}

// 处理 /api/analyze-samples 的请求体：支持 JSON 字符串数组与“每行一个文件名”两种格式
std::vector<std::string> parseSampleRequest(const std::string& body) {
    std::vector<std::string> names;
    // JSON 数组形式："a.docx","b.pdf"
    bool quoted = body.find('"') != std::string::npos;
    if (quoted) {
        size_t pos = 0;
        while (true) {
            size_t open = body.find('"', pos);
            if (open == std::string::npos) break;
            size_t close = body.find('"', open + 1);
            if (close == std::string::npos) break;
            std::string item = body.substr(open + 1, close - open - 1);
            if (!item.empty()) names.push_back(item);
            pos = close + 1;
        }
        return names;
    }
    std::string current;
    for (char c : body) {
        if (c == '\r') continue;
        if (c == '\n') {
            if (!current.empty()) names.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) names.push_back(current);
    return names;
}

// Web 路由：静态文件 + 四个 API
void handleRequest(const AppConfig& config, const pc::HttpRequest& request, pc::HttpResponse& response) {
    const std::string& path = request.path;

    // ---- 健康检查 ----
    if (path == "/api/health") {
        response.setJson("{\"ok\":true,\"name\":\"PaperCheck\",\"version\":\"1.0\"}");
        return;
    }

    // ---- 示例语料列表 ----
    if (path == "/api/samples") {
        const std::string samplesDir = resolveSamplesDir(config);
        std::vector<SampleInfo> samples = listSamples(samplesDir);
        std::string json = "{\"ok\":true,\"dir\":\"";
        json += pc::jsonEscape(samplesDir);
        json += "\",\"files\":[";
        for (size_t i = 0; i < samples.size(); ++i) {
            if (i) json += ',';
            json += "{\"name\":\"";
            json += pc::jsonEscape(samples[i].name);
            json += "\",\"size\":";
            json += std::to_string(samples[i].size);
            json += ",\"sizeText\":\"";
            json += humanSize(static_cast<size_t>(samples[i].size));
            json += "\",\"type\":\"";
            json += pc::jsonEscape(samples[i].type);
            json += "\"}";
        }
        json += "]}";
        response.setJson(json);
        return;
    }

    // ---- 上传文件查重 ----
    if (path == "/api/analyze" && request.method == "POST") {
        std::vector<pc::DocumentInput> inputs;
        pc::AnalysisReport failedReport;
        for (const pc::UploadedFile& file : request.files) {
            if (file.data.empty()) continue;
            pc::DocumentInput doc;
            doc.name = file.filename.empty() ? std::string("未命名文档") : file.filename;
            doc.type = pc::detectDocType(doc.name);
            std::string error;
            if (doc.type.empty()) {
                appendFailure(failedReport, doc.name, "不支持的文件类型（仅支持 .docx / .pdf / .txt）");
                continue;
            }
            if (doc.type == "docx") {
                if (!pc::extractDocxText(file.data, doc.text, error)) {
                    appendFailure(failedReport, doc.name, error);
                    continue;
                }
            } else if (doc.type == "pdf") {
                if (!pc::extractPdfText(file.data, doc.text, error)) {
                    appendFailure(failedReport, doc.name, error);
                    continue;
                }
            } else {
                doc.text = file.data;
                if (doc.text.size() >= 3 && static_cast<unsigned char>(doc.text[0]) == 0xEF &&
                    static_cast<unsigned char>(doc.text[1]) == 0xBB &&
                    static_cast<unsigned char>(doc.text[2]) == 0xBF) {
                    doc.text = doc.text.substr(3);
                }
            }
            inputs.push_back(std::move(doc));
        }

        if (inputs.size() < 2) {
            std::string json = "{\"ok\":false,\"error\":\"";
            json += inputs.empty() ? "没有读取到可用的文档，请检查文件格式" : "至少需要两篇可解析的文档才能查重";
            json += "\",\"failures\":[";
            for (size_t i = 0; i < failedReport.failures.size(); ++i) {
                if (i) json += ',';
                json += "\"" + pc::jsonEscape(failedReport.failures[i]) + "\"";
            }
            json += "]}";
            response.setJson(json);
            return;
        }

        pc::AnalysisReport report = runAnalysis(inputs, request.query);
        for (const std::string& item : failedReport.failures) report.failures.push_back(item);
        report.ok = true;
        response.setJson(pc::reportToJson(report));
        return;
    }

    // ---- 示例语料查重 ----
    if (path == "/api/analyze-samples" && request.method == "POST") {
        std::vector<std::string> names = parseSampleRequest(request.body);
        const std::string samplesDir = resolveSamplesDir(config);
        std::vector<pc::DocumentInput> inputs;
        pc::AnalysisReport failedReport;
        for (const std::string& name : names) {
            if (!isSafeSampleName(name)) {
                appendFailure(failedReport, name, "非法的文件名");
                continue;
            }
            pc::DocumentInput doc;
            doc.name = name;
            doc.type = pc::detectDocType(name);
            std::string error;
            if (!pc::loadDocumentText(pc::joinPath(samplesDir, name), doc.text, error)) {
                appendFailure(failedReport, name, error);
                continue;
            }
            inputs.push_back(std::move(doc));
        }
        if (inputs.size() < 2) {
            std::string json = "{\"ok\":false,\"error\":\"至少需要两篇可解析的示例文档\",\"failures\":[";
            for (size_t i = 0; i < failedReport.failures.size(); ++i) {
                if (i) json += ',';
                json += "\"" + pc::jsonEscape(failedReport.failures[i]) + "\"";
            }
            json += "]}";
            response.setJson(json);
            return;
        }
        pc::AnalysisReport report = runAnalysis(inputs, request.query);
        for (const std::string& item : failedReport.failures) report.failures.push_back(item);
        report.ok = true;
        response.setJson(pc::reportToJson(report));
        return;
    }

    if (path.rfind("/api/", 0) == 0) {
        response.status = 404;
        response.setJson("{\"ok\":false,\"error\":\"接口不存在\"}");
        return;
    }

    // ---- 静态资源 ----
    std::string error;
    if (!pc::serveStaticFile(config.webDir, path, response, error)) {
        response.status = 404;
        response.contentType = "text/plain; charset=utf-8";
        response.body = "404 Not Found: " + path;
    }
}

// 命令行模式：直接对给定文件查重并打印摘要
int runCli(const std::vector<std::string>& files) {
    std::vector<pc::DocumentInput> inputs;
    for (const std::string& path : files) {
        pc::DocumentInput doc;
        size_t pos = path.find_last_of("/\\");
        doc.name = pos == std::string::npos ? path : path.substr(pos + 1);
        doc.type = pc::detectDocType(doc.name);
        std::string error;
        if (!pc::loadDocumentText(path, doc.text, error)) {
            std::printf("[跳过] %s：%s\n", doc.name.c_str(), error.c_str());
            continue;
        }
        std::printf("[读取] %s（%zu 字节文本）\n", doc.name.c_str(), doc.text.size());
        inputs.push_back(std::move(doc));
    }
    if (inputs.size() < 2) {
        std::printf("至少需要两篇可解析的文档\n");
        return 1;
    }

    pc::AnalysisReport report = pc::analyzeDocuments(inputs, pc::AnalysisOptions());
    std::printf("\n===== 查重结果（耗时 %.1f ms）=====\n", report.elapsedMs);
    for (const pc::DocumentReport& doc : report.documents) {
        std::printf("[%d] %s\n    字符数 %zu，重复 %zu，查重率 %.2f%%（%s）\n", doc.index + 1,
                    doc.name.c_str(), doc.chars, doc.dupChars, doc.dupRate * 100.0,
                    doc.levelName.c_str());
    }
    std::printf("\n----- 相似文档对 -----\n");
    for (const pc::PairReport& pair : report.pairs) {
        std::printf("%s  <->  %s  相似度 %.2f%%（片段 %zu 处）\n",
                    report.documents[static_cast<size_t>(pair.a)].name.c_str(),
                    report.documents[static_cast<size_t>(pair.b)].name.c_str(), pair.similarity * 100.0,
                    pair.segments.size());
    }
    return 0;
}

// 控制台 UTF-8 输出设置
void setupConsole() {
    ::SetConsoleOutputCP(CP_UTF8);
    ::SetConsoleCP(CP_UTF8);
}

}  // namespace

int main(int argc, char** argv) {
    setupConsole();

    const std::vector<std::string> args = unicodeArguments(argc, argv);
    AppConfig config;
    std::vector<std::string> cliFiles;
    bool cliMode = false;

    for (size_t i = 1; i < args.size(); ++i) {
        const std::string& arg = args[i];
        const bool hasNext = (i + 1 < args.size());
        if (arg == "--port" && hasNext) {
            config.port = std::atoi(args[++i].c_str());
        } else if (arg == "--host" && hasNext) {
            config.host = args[++i];
        } else if (arg == "--web" && hasNext) {
            config.webDir = args[++i];
        } else if (arg == "--samples" && hasNext) {
            config.samplesDir = args[++i];
        } else if (arg == "--cli") {
            cliMode = true;
            for (size_t k = i + 1; k < args.size(); ++k) cliFiles.push_back(args[k]);
            break;
        } else if (arg == "--help" || arg == "-h") {
            std::printf("用法：pc_check.exe [--port 8080] [--host 127.0.0.1] [--web 目录] [--samples 目录]\n");
            std::printf("      pc_check.exe --cli a.docx b.pdf ...\n");
            return 0;
        }
    }

    std::string exeDir = pc::executableDir();
    std::string parentDir = pc::parentPath(exeDir);

    if (config.webDir.empty()) {
        config.webDir = findExistingDir({pc::joinPath(exeDir, "web"), pc::joinPath(parentDir, "web")}, "index.html");
        if (config.webDir.empty()) config.webDir = pc::joinPath(exeDir, "web");
    }
    if (config.samplesDir.empty()) {
        config.samplesDir = findExistingDir({pc::joinPath(parentDir, "samples"), pc::joinPath(exeDir, "samples")}, "");
        if (config.samplesDir.empty()) config.samplesDir = pc::joinPath(parentDir, "samples");
    }
    if (config.host.empty()) config.host = "127.0.0.1";

    if (cliMode) return runCli(cliFiles);

    // ---- Web 服务模式 ----
    if (!pc::fileExists(pc::joinPath(config.webDir, "index.html"))) {
        std::printf("警告：未找到前端资源目录 %s，页面将无法访问\n", config.webDir.c_str());
    }
    if (!pc::isDirectory(config.samplesDir)) {
        std::printf("提示：示例语料目录不存在 %s\n", config.samplesDir.c_str());
    }

    pc::HttpServer server;
    std::string error;
    if (!server.start(config.host, config.port, error)) {
        std::printf("启动失败：%s\n", error.c_str());
        return 1;
    }

    std::printf("文章查重系统已启动\n");
    std::printf("  访问地址：http://%s:%d\n", config.host.c_str(), server.port());
    std::printf("  前端目录：%s\n", config.webDir.c_str());
    std::printf("  示例语料：%s\n", config.samplesDir.c_str());
    std::printf("按 Ctrl+C 退出\n\n");

    server.run([&config](const pc::HttpRequest& request, pc::HttpResponse& response) {
        handleRequest(config, request, response);
    });

    server.stop();
    return 0;
}
