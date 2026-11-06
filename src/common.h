// =============================================================================
//  common.h —— 项目公共基础设施
//  提供：平台相关的文件读写（支持中文路径）、UTF-8 / UTF-16 转换、
//        字符串工具、计时工具。
// =============================================================================
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace pc {

// -----------------------------------------------------------------------------
// 文件读写：路径一律使用 UTF-8 编码，内部转换为宽字符调用 Win32 API，
// 从而正确支持中文文件名与中文目录名。
// -----------------------------------------------------------------------------

// 读取整个文件为字节串；失败返回 false 并写入 err。
bool readFileBytes(const std::string& utf8Path, std::string& out, std::string& err);

// 将字节串写入文件（覆盖写）；失败返回 false 并写入 err。
bool writeFileBytes(const std::string& utf8Path, const std::string& data, std::string& err);

// 判断文件是否存在（UTF-8 路径）。
bool fileExists(const std::string& utf8Path);

// 判断路径是否为目录。
bool isDirectory(const std::string& utf8Path);

// 取路径的父目录（UTF-8）。
std::string parentPath(const std::string& utf8Path);

// 拼接路径（自动补分隔符）。
std::string joinPath(const std::string& dir, const std::string& name);

// 可执行文件所在的目录（UTF-8），用于定位 web 资源。
std::string executableDir();

// -----------------------------------------------------------------------------
// 编码转换
// -----------------------------------------------------------------------------

std::wstring toWide(const std::string& utf8);      // UTF-8 -> UTF-16
std::string  toUtf8(const std::wstring& wide);     // UTF-16 -> UTF-8

// 从 UTF-8 串的 pos 位置解码一个 Unicode 码点，pos 前进；非法字节返回 0xFFFD。
uint32_t utf8Next(const std::string& s, size_t& pos);

// 将单个码点编码为 UTF-8 追加到 out。
void utf8Append(std::string& out, uint32_t cp);

// 计算 UTF-8 字符串对应的 UTF-16 编码单元数量（JS 字符串下标口径）。
size_t utf16Length(const std::string& utf8);

// -----------------------------------------------------------------------------
// 字符串工具
// -----------------------------------------------------------------------------

std::string trim(const std::string& s);
std::string toLowerAscii(std::string s);

// 忽略大小写判断后缀。
bool endsWithNoCase(const std::string& s, const std::string& suffix);

// 把 UTF-8 文本转义成合法的 JSON 字符串内容（不含两侧引号）。
std::string jsonEscape(const std::string& utf8);

// 把字节数格式化为人类可读形式（例如 "1.2 MB"）。
std::string formatBytes(size_t bytes);

// -----------------------------------------------------------------------------
// 计时器：基于 std::chrono，用于统计分析耗时。
// -----------------------------------------------------------------------------
class Stopwatch {
public:
    Stopwatch();
    void reset();
    // 返回自创建 / 上次 reset 以来经过的毫秒数。
    double elapsedMs() const;

private:
    long long startUs_;
};

} // namespace pc
