// =============================================================================
//  common.cpp —— 公共基础设施实现
// =============================================================================
#include "common.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

namespace pc {

// -----------------------------------------------------------------------------
// 文件读写（Win32 宽字符 API，天然支持中文路径）
// -----------------------------------------------------------------------------
bool readFileBytes(const std::string& utf8Path, std::string& out, std::string& err) {
    out.clear();
    std::wstring wide = toWide(utf8Path);
    HANDLE h = CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        err = "无法打开文件（错误码 " + std::to_string(GetLastError()) + "）";
        return false;
    }
    LARGE_INTEGER size;
    if (!GetFileSizeEx(h, &size)) {
        err = "无法获取文件大小";
        CloseHandle(h);
        return false;
    }
    if (size.QuadPart > 512LL * 1024 * 1024) {   // 限制单文件 512MB，防止误传超大文件
        err = "文件过大（超过 512MB）";
        CloseHandle(h);
        return false;
    }
    out.resize(static_cast<size_t>(size.QuadPart));
    size_t total = 0;
    while (total < out.size()) {
        DWORD got = 0;
        DWORD want = static_cast<DWORD>(std::min<size_t>(out.size() - total, 1u << 20));
        if (!ReadFile(h, &out[total], want, &got, nullptr) || got == 0) {
            err = "读取文件失败";
            CloseHandle(h);
            return false;
        }
        total += got;
    }
    CloseHandle(h);
    return true;
}

bool writeFileBytes(const std::string& utf8Path, const std::string& data, std::string& err) {
    std::wstring wide = toWide(utf8Path);
    HANDLE h = CreateFileW(wide.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        err = "无法写入文件（错误码 " + std::to_string(GetLastError()) + "）";
        return false;
    }
    size_t total = 0;
    while (total < data.size()) {
        DWORD wrote = 0;
        DWORD want = static_cast<DWORD>(std::min<size_t>(data.size() - total, 1u << 20));
        if (!WriteFile(h, data.data() + total, want, &wrote, nullptr) || wrote == 0) {
            err = "写入文件失败";
            CloseHandle(h);
            return false;
        }
        total += wrote;
    }
    CloseHandle(h);
    return true;
}

bool fileExists(const std::string& utf8Path) {
    std::wstring wide = toWide(utf8Path);
    DWORD attr = GetFileAttributesW(wide.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

bool isDirectory(const std::string& utf8Path) {
    std::wstring wide = toWide(utf8Path);
    DWORD attr = GetFileAttributesW(wide.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

std::string parentPath(const std::string& utf8Path) {
    size_t pos = utf8Path.find_last_of("\\/");
    if (pos == std::string::npos) return ".";
    if (pos == 0) return utf8Path.substr(0, 1);
    // 跳过连续的分隔符，保留根目录形式（如 "C:\"）
    std::string parent = utf8Path.substr(0, pos);
    if (parent.size() == 2 && parent[1] == ':') parent += "\\";
    if (parent.empty()) parent = ".";
    return parent;
}

std::string joinPath(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    char last = dir.back();
    if (last == '\\' || last == '/') return dir + name;
    return dir + "\\" + name;
}

std::string executableDir() {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])));
    if (n == 0) return ".";
    std::wstring path(buf, n);
    size_t pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return ".";
    return toUtf8(path.substr(0, pos));
}

// -----------------------------------------------------------------------------
// 编码转换
// -----------------------------------------------------------------------------
std::wstring toWide(const std::string& utf8) {
    if (utf8.empty()) return L"";
    int need = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (need <= 0) return L"";
    std::wstring out(static_cast<size_t>(need), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), &out[0], need);
    return out;
}

std::string toUtf8(const std::wstring& wide) {
    if (wide.empty()) return "";
    int need = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                   nullptr, 0, nullptr, nullptr);
    if (need <= 0) return "";
    std::string out(static_cast<size_t>(need), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                        &out[0], need, nullptr, nullptr);
    return out;
}

uint32_t utf8Next(const std::string& s, size_t& pos) {
    if (pos >= s.size()) return 0;
    unsigned char c = static_cast<unsigned char>(s[pos]);
    if (c < 0x80) {
        pos += 1;
        return c;
    }
    int extra = 0;
    uint32_t cp = 0;
    if ((c & 0xE0) == 0xC0) { extra = 1; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { extra = 2; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { extra = 3; cp = c & 0x07; }
    else { pos += 1; return 0xFFFD; }
    // 剩余字节不足，按非法序列处理（吞掉整个字符串尾部）
    if (pos + static_cast<size_t>(extra) >= s.size()) {
        pos = s.size();
        return 0xFFFD;
    }
    for (int i = 1; i <= extra; ++i) {
        unsigned char cc = static_cast<unsigned char>(s[pos + i]);
        if ((cc & 0xC0) != 0x80) { pos += 1; return 0xFFFD; }
        cp = (cp << 6) | (cc & 0x3F);
    }
    pos += static_cast<size_t>(extra) + 1;
    return cp;
}

void utf8Append(std::string& out, uint32_t cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

size_t utf16Length(const std::string& utf8) {
    size_t pos = 0, len = 0;
    while (pos < utf8.size()) {
        uint32_t cp = utf8Next(utf8, pos);
        len += (cp > 0xFFFF) ? 2 : 1;
    }
    return len;
}

// -----------------------------------------------------------------------------
// 字符串工具
// -----------------------------------------------------------------------------
std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return s.substr(b, e - b);
}

std::string toLowerAscii(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

bool endsWithNoCase(const std::string& s, const std::string& suffix) {
    if (s.size() < suffix.size()) return false;
    return toLowerAscii(s.substr(s.size() - suffix.size())) == toLowerAscii(suffix);
}

std::string jsonEscape(const std::string& utf8) {
    std::string out;
    out.reserve(utf8.size() + 16);
    for (unsigned char c : utf8) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    return out;
}

std::string formatBytes(size_t bytes) {
    char buf[64];
    if (bytes < 1024) {
        std::snprintf(buf, sizeof(buf), "%zu B", bytes);
    } else if (bytes < 1024 * 1024) {
        std::snprintf(buf, sizeof(buf), "%.1f KB", bytes / 1024.0);
    } else {
        std::snprintf(buf, sizeof(buf), "%.1f MB", bytes / (1024.0 * 1024.0));
    }
    return buf;
}

// -----------------------------------------------------------------------------
// Stopwatch
// -----------------------------------------------------------------------------
Stopwatch::Stopwatch() { reset(); }

void Stopwatch::reset() {
    startUs_ = std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
                   .count();
}

double Stopwatch::elapsedMs() const {
    long long now = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
    return static_cast<double>(now - startUs_) / 1000.0;
}

} // namespace pc
