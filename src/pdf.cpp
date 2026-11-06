// =============================================================================
//  pdf.cpp —— PDF 文本提取实现
//
//  整体流程：
//    1. 顺序扫描文件，解析所有 "N G obj ... endobj" 间接对象（不依赖 xref 表，
//       对损坏/线性化的文件更宽容）；
//    2. 展开对象流（/Type /ObjStm，PDF 1.5+ 常见的压缩对象）；
//    3. 从 /Catalog 的页面树收集页面，继承 /Resources；
//    4. 拼接页面内容流并按 /Filter 解码（FlateDecode / ASCIIHex / ASCII85）；
//    5. 解析字体资源：ToUnicode CMap、Identity-H、UniGB-UCS2-H 等编码；
//    6. 解释内容流中的文本算子（BT/Tf/Tm/Td/Tj/TJ/'/"），还原文本并做换行推断。
//
//  说明：本项目面向“文字型 PDF”，对扫描件（纯图片）只能返回错误提示。
// =============================================================================
#include "pdf.h"

#include "common.h"
#include "inflate.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>
#include <vector>

namespace pc {
namespace {

// =============================================================================
//  一、PDF 对象模型
// =============================================================================
struct PdfObj;
using PdfPtr = std::shared_ptr<PdfObj>;

enum class ObjType { Null, Bool, Number, String, Name, Array, Dict, Ref };

struct PdfObj {
    ObjType type = ObjType::Null;
    bool boolean = false;
    double number = 0.0;
    std::string text;                       // 字符串内容 / 名字
    std::vector<PdfPtr> items;              // 数组元素
    std::map<std::string, PdfPtr> dict;     // 字典键值
    int refId = 0;                          // 间接引用编号
    std::vector<uint8_t> raw;               // 流的原始（未解码）数据
    bool hasStream = false;
};

PdfPtr makeNull() { return std::make_shared<PdfObj>(); }

PdfPtr makeBool(bool v) {
    auto o = std::make_shared<PdfObj>();
    o->type = ObjType::Bool;
    o->boolean = v;
    return o;
}

PdfPtr makeNumber(double v) {
    auto o = std::make_shared<PdfObj>();
    o->type = ObjType::Number;
    o->number = v;
    return o;
}

PdfPtr makeName(const std::string& n) {
    auto o = std::make_shared<PdfObj>();
    o->type = ObjType::Name;
    o->text = n;
    return o;
}

PdfPtr makeString(const std::string& s) {
    auto o = std::make_shared<PdfObj>();
    o->type = ObjType::String;
    o->text = s;
    return o;
}

// =============================================================================
//  二、词法与语法解析
// =============================================================================
inline bool isPdfWhite(unsigned char c) {
    return c == 0 || c == 9 || c == 10 || c == 12 || c == 13 || c == ' ';
}

inline bool isPdfDelim(unsigned char c) {
    return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' ||
           c == '{' || c == '}' || c == '/' || c == '%';
}

// 把 #XX 形式的字节转义还原（PDF 名字中的编码）。
std::string decodeNameEscapes(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '#' && i + 2 < raw.size() &&
            std::isxdigit(static_cast<unsigned char>(raw[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(raw[i + 2]))) {
            int v = 0;
            for (int k = 1; k <= 2; ++k) {
                char c = raw[i + k];
                v = v * 16 + (c >= '0' && c <= '9' ? c - '0'
                                : (c >= 'a' && c <= 'f' ? c - 'a' + 10 : c - 'A' + 10));
            }
            out.push_back(static_cast<char>(v));
            i += 2;
        } else {
            out.push_back(raw[i]);
        }
    }
    return out;
}

// PDF 对象语法解析器（在给定缓冲区上工作）。
class ObjParser {
public:
    ObjParser(const std::string& buf, size_t start = 0) : buf_(buf), pos_(start) {}

    // 解析一个 PDF 值；失败返回 nullptr。
    PdfPtr parseValue(int depth = 0);

    size_t pos() const { return pos_; }
    void setPos(size_t p) { pos_ = p; }
    bool eof() const { return pos_ >= buf_.size(); }

    // 跳过空白与注释
    void skipWhitespace();

    // 尝试读取关键字（后面必须是分隔符）
    bool matchKeyword(const char* kw);

    // 读取一个整数（成功返回 true）
    bool readInt(int& out);

private:
    PdfPtr parseDict(int depth);
    PdfPtr parseArray(int depth);
    PdfPtr parseName();
    PdfPtr parseLiteralString();
    PdfPtr parseHexString();
    PdfPtr parseNumberOrRef();
    bool parseNumber(double& out, bool& isInteger, int& intValue);

    const std::string& buf_;
    size_t pos_;
};

void ObjParser::skipWhitespace() {
    while (pos_ < buf_.size()) {
        unsigned char c = static_cast<unsigned char>(buf_[pos_]);
        if (isPdfWhite(c)) {
            ++pos_;
        } else if (c == '%') {           // 注释：直到行尾
            while (pos_ < buf_.size() && buf_[pos_] != '\n' && buf_[pos_] != '\r') ++pos_;
        } else {
            break;
        }
    }
}

bool ObjParser::matchKeyword(const char* kw) {
    size_t n = std::strlen(kw);
    if (pos_ + n > buf_.size()) return false;
    if (buf_.compare(pos_, n, kw) != 0) return false;
    if (pos_ + n < buf_.size()) {
        unsigned char c = static_cast<unsigned char>(buf_[pos_ + n]);
        if (!isPdfWhite(c) && !isPdfDelim(c)) return false;
    }
    pos_ += n;
    return true;
}

bool ObjParser::readInt(int& out) {
    skipWhitespace();
    size_t start = pos_;
    bool neg = false;
    if (pos_ < buf_.size() && (buf_[pos_] == '-' || buf_[pos_] == '+')) {
        neg = buf_[pos_] == '-';
        ++pos_;
    }
    long long v = 0;
    bool any = false;
    while (pos_ < buf_.size() && buf_[pos_] >= '0' && buf_[pos_] <= '9') {
        v = v * 10 + (buf_[pos_] - '0');
        ++pos_;
        any = true;
        if (v > 0x7fffffffLL) return false;
    }
    if (!any) {
        pos_ = start;
        return false;
    }
    out = static_cast<int>(neg ? -v : v);
    return true;
}

bool ObjParser::parseNumber(double& out, bool& isInteger, int& intValue) {
    skipWhitespace();
    size_t start = pos_;
    bool neg = false;
    if (pos_ < buf_.size() && (buf_[pos_] == '-' || buf_[pos_] == '+')) {
        neg = buf_[pos_] == '-';
        ++pos_;
    }
    bool any = false;
    bool hasDot = false;
    double v = 0.0;
    while (pos_ < buf_.size()) {
        char c = buf_[pos_];
        if (c >= '0' && c <= '9') {
            v = v * 10.0 + (c - '0');
            ++pos_;
            any = true;
        } else if (c == '.') {
            double frac = 0.1;
            hasDot = true;
            ++pos_;
            while (pos_ < buf_.size() && buf_[pos_] >= '0' && buf_[pos_] <= '9') {
                v += (buf_[pos_] - '0') * frac;
                frac *= 0.1;
                ++pos_;
                any = true;
            }
            break;
        } else {
            break;
        }
    }
    if (!any) {
        pos_ = start;
        return false;
    }
    out = neg ? -v : v;
    isInteger = !hasDot;
    intValue = static_cast<int>(v);
    return true;
}

PdfPtr ObjParser::parseName() {
    if (pos_ >= buf_.size() || buf_[pos_] != '/') return nullptr;
    ++pos_;
    size_t start = pos_;
    while (pos_ < buf_.size()) {
        unsigned char c = static_cast<unsigned char>(buf_[pos_]);
        if (isPdfWhite(c) || isPdfDelim(c)) break;
        ++pos_;
    }
    return makeName(decodeNameEscapes(buf_.substr(start, pos_ - start)));
}

PdfPtr ObjParser::parseLiteralString() {
    if (pos_ >= buf_.size() || buf_[pos_] != '(') return nullptr;
    ++pos_;
    std::string out;
    int depth = 1;
    while (pos_ < buf_.size() && depth > 0) {
        char c = buf_[pos_++];
        if (c == '\\') {
            if (pos_ >= buf_.size()) break;
            char e = buf_[pos_++];
            switch (e) {
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case '(': out.push_back('('); break;
                case ')': out.push_back(')'); break;
                case '\\': out.push_back('\\'); break;
                case '\r':                      // 续行
                    if (pos_ < buf_.size() && buf_[pos_] == '\n') ++pos_;
                    break;
                case '\n': break;
                default:
                    if (e >= '0' && e <= '7') {  // 八进制转义
                        int v = e - '0';
                        for (int k = 0; k < 2 && pos_ < buf_.size() &&
                                        buf_[pos_] >= '0' && buf_[pos_] <= '7'; ++k) {
                            v = v * 8 + (buf_[pos_++] - '0');
                        }
                        out.push_back(static_cast<char>(v & 0xFF));
                    } else {
                        out.push_back(e);
                    }
            }
        } else if (c == '(') {
            ++depth;
            out.push_back(c);
        } else if (c == ')') {
            --depth;
            if (depth > 0) out.push_back(c);
        } else {
            out.push_back(c);
        }
    }
    return makeString(out);
}

PdfPtr ObjParser::parseHexString() {
    if (pos_ >= buf_.size() || buf_[pos_] != '<') return nullptr;
    ++pos_;
    std::string hex;
    while (pos_ < buf_.size() && buf_[pos_] != '>') {
        char c = buf_[pos_++];
        if (std::isxdigit(static_cast<unsigned char>(c))) hex.push_back(c);
    }
    if (pos_ < buf_.size()) ++pos_;  // 跳过 '>'
    if (hex.size() % 2 != 0) hex.push_back('0');
    std::string out;
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        auto nib = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            return c - 'A' + 10;
        };
        out.push_back(static_cast<char>((nib(hex[i]) << 4) | nib(hex[i + 1])));
    }
    return makeString(out);
}

PdfPtr ObjParser::parseArray(int depth) {
    if (pos_ >= buf_.size() || buf_[pos_] != '[') return nullptr;
    ++pos_;
    auto arr = std::make_shared<PdfObj>();
    arr->type = ObjType::Array;
    for (;;) {
        skipWhitespace();
        if (pos_ >= buf_.size()) return nullptr;
        if (buf_[pos_] == ']') {
            ++pos_;
            break;
        }
        PdfPtr v = parseValue(depth + 1);
        if (!v) return nullptr;
        arr->items.push_back(v);
        if (arr->items.size() > 100000) return nullptr;  // 防御异常数据
    }
    return arr;
}

PdfPtr ObjParser::parseDict(int depth) {
    if (pos_ + 1 >= buf_.size() || buf_[pos_] != '<' || buf_[pos_ + 1] != '<') return nullptr;
    pos_ += 2;
    auto obj = std::make_shared<PdfObj>();
    obj->type = ObjType::Dict;
    for (;;) {
        skipWhitespace();
        if (pos_ >= buf_.size()) return nullptr;
        if (buf_[pos_] == '>' && pos_ + 1 < buf_.size() && buf_[pos_ + 1] == '>') {
            pos_ += 2;
            break;
        }
        if (buf_[pos_] != '/') {
            // 容忍残缺字典：跳过无法识别的记号
            ++pos_;
            continue;
        }
        PdfPtr key = parseName();
        if (!key) return nullptr;
        PdfPtr val = parseValue(depth + 1);
        if (!val) return nullptr;
        obj->dict[key->text] = val;
    }
    return obj;
}

PdfPtr ObjParser::parseNumberOrRef() {
    size_t start = pos_;
    double num = 0.0;
    bool isInt = false;
    int intVal = 0;
    if (!parseNumber(num, isInt, intVal)) return nullptr;

    // 尝试识别 "num gen R" 形式的间接引用
    if (isInt && intVal >= 0) {
        size_t afterNum = pos_;
        int gen = 0;
        skipWhitespace();
        if (readInt(gen) && gen >= 0) {
            skipWhitespace();
            if (pos_ < buf_.size() && buf_[pos_] == 'R') {
                size_t afterR = pos_ + 1;
                bool boundary = true;
                if (afterR < buf_.size()) {
                    unsigned char c = static_cast<unsigned char>(buf_[afterR]);
                    if (!isPdfWhite(c) && !isPdfDelim(c)) boundary = false;
                }
                if (boundary) {
                    pos_ = afterR;
                    auto ref = std::make_shared<PdfObj>();
                    ref->type = ObjType::Ref;
                    ref->refId = intVal;
                    ref->number = gen;
                    return ref;
                }
            }
        }
        pos_ = afterNum;  // 回退：不是引用
    }
    (void)start;
    return makeNumber(num);
}

PdfPtr ObjParser::parseValue(int depth) {
    if (depth > 64) return nullptr;
    skipWhitespace();
    if (pos_ >= buf_.size()) return nullptr;
    char c = buf_[pos_];
    if (c == '/') return parseName();
    if (c == '(') return parseLiteralString();
    if (c == '<') {
        if (pos_ + 1 < buf_.size() && buf_[pos_ + 1] == '<') return parseDict(depth);
        return parseHexString();
    }
    if (c == '[') return parseArray(depth);
    if (c == '-' || c == '+' || c == '.' || (c >= '0' && c <= '9')) return parseNumberOrRef();
    if (matchKeyword("true")) return makeBool(true);
    if (matchKeyword("false")) return makeBool(false);
    if (matchKeyword("null")) return makeNull();
    return nullptr;
}

// =============================================================================
//  三、编码工具：UTF-16BE -> UTF-8、WinAnsi 码表
// =============================================================================

// 把 UTF-16BE 字节序列解码为 UTF-8（处理代理对）。
std::string utf16beToUtf8(const std::string& bytes) {
    std::string out;
    size_t i = 0;
    while (i + 1 < bytes.size()) {
        uint32_t u = (static_cast<uint32_t>(static_cast<unsigned char>(bytes[i])) << 8) |
                     static_cast<unsigned char>(bytes[i + 1]);
        i += 2;
        if (u >= 0xD800 && u <= 0xDBFF && i + 1 < bytes.size()) {
            uint32_t lo = (static_cast<uint32_t>(static_cast<unsigned char>(bytes[i])) << 8) |
                          static_cast<unsigned char>(bytes[i + 1]);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                i += 2;
                u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
            }
        }
        utf8Append(out, u);
    }
    return out;
}

// WinAnsiEncoding（近似 CP1252）码表：0x00-0xFF -> Unicode。
uint32_t winAnsiChar(unsigned char code) {
    static const uint32_t kHigh[32] = {
        0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
        0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
        0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
        0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178};
    if (code >= 0x80 && code <= 0x9F) return kHigh[code - 0x80];
    return code;
}

// =============================================================================
//  四、字体信息：ToUnicode CMap 与编码方式
// =============================================================================
struct FontInfo {
    bool twoByte = false;    // 双字节编码（Type0 复合字体）
    bool ucs2 = false;       // 直接使用 UCS-2/UTF-16BE 编码
    std::map<uint32_t, std::string> toUnicode;  // 字符码 -> UTF-8 文本
    std::map<uint32_t, double> widths;          // 字符码 -> 字宽（1/1000 em）
    double defaultWidth = 500.0;                // 缺省字宽（1/1000 em）
    bool hasWidths = false;                     // 是否读到了 /Widths 或 /W
};

// 简易 CMap 词法单元
struct CMapToken {
    enum Kind { Hex, Name, ArrayStart, ArrayEnd, Word } kind;
    std::string text;
};

// 把 ToUnicode CMap 文本切分为词法单元。
std::vector<CMapToken> tokenizeCMap(const std::string& s) {
    std::vector<CMapToken> out;
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (isPdfWhite(c)) { ++i; continue; }
        if (c == '%') {
            while (i < s.size() && s[i] != '\n' && s[i] != '\r') ++i;
            continue;
        }
        if (c == '<') {
            // 注意：CMap 里不存在字典，这里一律视为十六进制串
            ++i;
            std::string hex;
            while (i < s.size() && s[i] != '>') {
                if (std::isxdigit(static_cast<unsigned char>(s[i]))) hex.push_back(s[i]);
                ++i;
            }
            if (i < s.size()) ++i;
            if (hex.size() % 2) hex.push_back('0');
            std::string bytes;
            for (size_t k = 0; k + 1 < hex.size(); k += 2) {
                auto nib = [](char ch) -> int {
                    if (ch >= '0' && ch <= '9') return ch - '0';
                    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
                    return ch - 'A' + 10;
                };
                bytes.push_back(static_cast<char>((nib(hex[k]) << 4) | nib(hex[k + 1])));
            }
            out.push_back({CMapToken::Hex, bytes});
            continue;
        }
        if (c == '[') { out.push_back({CMapToken::ArrayStart, ""}); ++i; continue; }
        if (c == ']') { out.push_back({CMapToken::ArrayEnd, ""}); ++i; continue; }
        if (c == '/') {
            ++i;
            std::string name;
            while (i < s.size() && !isPdfWhite(static_cast<unsigned char>(s[i])) &&
                   !isPdfDelim(static_cast<unsigned char>(s[i]))) {
                name.push_back(s[i++]);
            }
            out.push_back({CMapToken::Name, name});
            continue;
        }
        // 普通单词
        std::string word;
        while (i < s.size() && !isPdfWhite(static_cast<unsigned char>(s[i])) &&
               !isPdfDelim(static_cast<unsigned char>(s[i]))) {
            word.push_back(s[i++]);
        }
        if (!word.empty()) out.push_back({CMapToken::Word, word});
        else ++i;
    }
    return out;
}

// 将十六进制串按大端整数解释，并加上增量（bfrange 用）。
std::string incrementBigEndian(const std::string& bytes, uint32_t add) {
    if (bytes.empty()) return bytes;
    std::vector<unsigned char> tmp(bytes.begin(), bytes.end());
    uint64_t carry = add;
    for (size_t i = tmp.size(); i-- > 0;) {
        uint64_t v = tmp[i] + carry;
        tmp[i] = static_cast<unsigned char>(v & 0xFF);
        carry = v >> 8;
    }
    return std::string(tmp.begin(), tmp.end());
}

// 解析 ToUnicode CMap，填充 FontInfo。
void parseToUnicode(const std::string& cmap, FontInfo& font) {
    std::vector<CMapToken> toks = tokenizeCMap(cmap);
    size_t i = 0;
    while (i < toks.size()) {
        if (toks[i].kind != CMapToken::Word) { ++i; continue; }
        if (toks[i].text == "beginbfchar") {
            ++i;
            while (i + 1 < toks.size() &&
                   !(toks[i].kind == CMapToken::Word && toks[i].text == "endbfchar")) {
                if (toks[i].kind != CMapToken::Hex || toks[i + 1].kind != CMapToken::Hex) {
                    ++i;
                    continue;
                }
                uint32_t code = 0;
                for (unsigned char b : toks[i].text) code = (code << 8) | b;
                font.toUnicode[code] = utf16beToUtf8(toks[i + 1].text);
                i += 2;
            }
            ++i;
        } else if (toks[i].text == "beginbfrange") {
            ++i;
            while (i < toks.size() &&
                   !(toks[i].kind == CMapToken::Word && toks[i].text == "endbfrange")) {
                if (i + 2 >= toks.size() || toks[i].kind != CMapToken::Hex ||
                    toks[i + 1].kind != CMapToken::Hex) {
                    ++i;
                    continue;
                }
                std::string loBytes = toks[i].text;
                std::string hiBytes = toks[i + 1].text;
                uint32_t lo = 0, hi = 0;
                for (unsigned char b : loBytes) lo = (lo << 8) | b;
                for (unsigned char b : hiBytes) hi = (hi << 8) | b;
                size_t j = i + 2;
                if (j < toks.size() && toks[j].kind == CMapToken::Hex) {
                    std::string dst = toks[j].text;
                    if (hi >= lo && hi - lo < 65536) {
                        for (uint32_t k = 0; k <= hi - lo; ++k) {
                            font.toUnicode[lo + k] = utf16beToUtf8(incrementBigEndian(dst, k));
                        }
                    }
                    i = j + 1;
                } else if (j < toks.size() && toks[j].kind == CMapToken::ArrayStart) {
                    ++j;
                    uint32_t k = 0;
                    while (j < toks.size() && toks[j].kind != CMapToken::ArrayEnd) {
                        if (toks[j].kind == CMapToken::Hex) {
                            font.toUnicode[lo + k] = utf16beToUtf8(toks[j].text);
                            ++k;
                        }
                        ++j;
                    }
                    i = j + 1;
                } else {
                    ++i;
                }
            }
            ++i;
        } else {
            ++i;
        }
    }
}

// =============================================================================
//  五、PDF 文档对象：对象表、流解码、页面树
// =============================================================================
class PdfDocument {
public:
    bool load(std::string bytes, std::string& err);
    bool extractText(std::string& text, std::string& err);

    // 供内容流解释器调用的公共包装（内部实现见下方）
    bool decodeStreamPublic(const PdfPtr& obj, std::vector<uint8_t>& out) {
        return decodeStream(obj, out);
    }
    void loadFontPublic(const PdfPtr& dict, FontInfo& info) { loadFont(dict, info); }
    PdfPtr resolvePublic(const PdfPtr& obj) { return resolve(obj); }

private:
    // 解析所有间接对象
    bool parseObjects();
    // 展开对象流中的压缩对象
    void expandObjectStreams();
    // 解引用（可递归）
    PdfPtr resolve(const PdfPtr& obj, int depth = 0);
    PdfPtr getObject(int id);
    // 按 /Filter 解码流数据
    bool decodeStream(const PdfPtr& obj, std::vector<uint8_t>& out);
    // 收集页面（递归遍历页面树）
    void collectPages(const PdfPtr& node, const PdfPtr& inheritedResources,
                      std::vector<std::pair<PdfPtr, PdfPtr>>& pages, std::set<int>& visited,
                      int depth);
    // 解析字体字典
    void loadFont(const PdfPtr& fontDict, FontInfo& info);

    std::string buf_;
    std::unordered_map<int, PdfPtr> objects_;
};

PdfPtr PdfDocument::getObject(int id) {
    auto it = objects_.find(id);
    if (it == objects_.end()) return nullptr;
    return it->second;
}

PdfPtr PdfDocument::resolve(const PdfPtr& obj, int depth) {
    if (!obj || depth > 32) return obj;
    if (obj->type == ObjType::Ref) {
        return resolve(getObject(obj->refId), depth + 1);
    }
    return obj;
}

bool PdfDocument::parseObjects() {
    size_t p = 0;
    const size_t n = buf_.size();
    int guard = 0;
    while (p < n && guard < 2000000) {
        ++guard;
        if (!(buf_[p] >= '0' && buf_[p] <= '9')) {
            ++p;
            continue;
        }
        ObjParser parser(buf_, p);
        int objNum = 0, gen = 0;
        if (!parser.readInt(objNum) || objNum < 0) { ++p; continue; }
        if (!parser.readInt(gen)) { ++p; continue; }
        parser.skipWhitespace();
        if (!parser.matchKeyword("obj")) { ++p; continue; }

        PdfPtr value = parser.parseValue();
        if (!value) { ++p; continue; }
        size_t afterValue = parser.pos();

        // 处理流对象
        size_t q = afterValue;
        ObjParser probe(buf_, q);
        probe.skipWhitespace();
        if (value->type == ObjType::Dict && probe.matchKeyword("stream")) {
            q = probe.pos();
            // 跳过 stream 关键字后的 EOL
            if (q < n && buf_[q] == '\r') ++q;
            if (q < n && buf_[q] == '\n') ++q;
            size_t dataStart = q;
            size_t dataLen = 0;
            bool lengthOk = false;

            auto lenIt = value->dict.find("Length");
            if (lenIt != value->dict.end() && lenIt->second &&
                lenIt->second->type == ObjType::Number) {
                dataLen = static_cast<size_t>(lenIt->second->number);
                if (dataStart + dataLen <= n) {
                    size_t t = dataStart + dataLen;
                    ObjParser endProbe(buf_, t);
                    endProbe.skipWhitespace();
                    if (endProbe.matchKeyword("endstream")) lengthOk = true;
                }
            }
            if (!lengthOk) {
                size_t e = buf_.find("endstream", dataStart);
                if (e == std::string::npos) { ++p; continue; }
                dataLen = e - dataStart;
                // 去掉 endstream 前的换行
                if (dataLen > 0 && buf_[dataStart + dataLen - 1] == '\n') --dataLen;
                if (dataLen > 0 && buf_[dataStart + dataLen - 1] == '\r') --dataLen;
            }
            value->raw.assign(buf_.begin() + static_cast<long long>(dataStart),
                              buf_.begin() + static_cast<long long>(dataStart + dataLen));
            value->hasStream = true;

            size_t e = buf_.find("endstream", dataStart + dataLen);
            q = (e == std::string::npos) ? (dataStart + dataLen) : e + 9;
        }

        // 只保留第一个出现的同名对象（增量更新时后出现的会覆盖，这里取后者）
        objects_[objNum] = value;

        size_t end = buf_.find("endobj", q);
        p = (end == std::string::npos) ? (q + 1) : end + 6;
    }
    return !objects_.empty();
}

// 解析对象流（ObjStm）：流内包含多个压缩对象。
void PdfDocument::expandObjectStreams() {
    std::vector<std::pair<int, PdfPtr>> found;
    for (const auto& kv : objects_) {
        PdfPtr obj = kv.second;
        if (!obj || obj->type != ObjType::Dict || !obj->hasStream) continue;
        auto t = obj->dict.find("Type");
        if (t == obj->dict.end() || !t->second || t->second->type != ObjType::Name ||
            t->second->text != "ObjStm") {
            continue;
        }
        found.push_back({kv.first, obj});
    }
    for (auto& item : found) {
        PdfPtr obj = item.second;
        std::vector<uint8_t> data;
        if (!decodeStream(obj, data)) continue;
        std::string content(reinterpret_cast<const char*>(data.data()), data.size());

        auto nIt = obj->dict.find("N");
        auto fIt = obj->dict.find("First");
        if (nIt == obj->dict.end() || fIt == obj->dict.end()) continue;
        int count = static_cast<int>(nIt->second->number);
        int first = static_cast<int>(fIt->second->number);
        if (count <= 0 || count > 100000 || first < 0 ||
            static_cast<size_t>(first) >= content.size()) {
            continue;
        }
        ObjParser header(content, 0);
        for (int i = 0; i < count; ++i) {
            int objNum = 0, offset = 0;
            if (!header.readInt(objNum) || !header.readInt(offset)) break;
            ObjParser parser(content, static_cast<size_t>(first + offset));
            PdfPtr value = parser.parseValue();
            if (value) {
                if (objects_.find(objNum) == objects_.end()) objects_[objNum] = value;
            }
        }
    }
}

// 按 /Filter 解码流。
bool PdfDocument::decodeStream(const PdfPtr& obj, std::vector<uint8_t>& out) {
    out.clear();
    if (!obj || !obj->hasStream) return false;
    PdfPtr filter = nullptr;
    auto it = obj->dict.find("Filter");
    if (it != obj->dict.end()) filter = resolve(it->second);

    std::vector<std::string> filters;
    if (filter) {
        if (filter->type == ObjType::Name) {
            filters.push_back(filter->text);
        } else if (filter->type == ObjType::Array) {
            for (auto& f : filter->items) {
                PdfPtr rf = resolve(f);
                if (rf && rf->type == ObjType::Name) filters.push_back(rf->text);
            }
        }
    }

    out = obj->raw;
    for (const std::string& f : filters) {
        if (f == "FlateDecode" || f == "Fl") {
            std::vector<uint8_t> inflated;
            std::string err;
            if (!inflateZlib(out.data(), out.size(), inflated, err)) {
                std::string err2;
                if (!inflateRaw(out.data(), out.size(), inflated, err2)) return false;
            }
            out.swap(inflated);
        } else if (f == "ASCIIHexDecode" || f == "AHx") {
            std::string hex;
            for (unsigned char c : out) {
                if (c == '>') break;
                if (std::isxdigit(c)) hex.push_back(static_cast<char>(c));
            }
            if (hex.size() % 2) hex.push_back('0');
            std::vector<uint8_t> bin;
            for (size_t i = 0; i + 1 < hex.size(); i += 2) {
                auto nib = [](char ch) -> int {
                    if (ch >= '0' && ch <= '9') return ch - '0';
                    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
                    return ch - 'A' + 10;
                };
                bin.push_back(static_cast<uint8_t>((nib(hex[i]) << 4) | nib(hex[i + 1])));
            }
            out.swap(bin);
        } else if (f == "ASCII85Decode" || f == "A85") {
            // ASCII85：每 5 个字符表示 4 字节，'z' 表示 4 个 0
            std::vector<uint8_t> bin;
            uint32_t tuple = 0;
            int count = 0;
            for (size_t i = 0; i < out.size(); ++i) {
                unsigned char c = out[i];
                if (c == '~') break;
                if (c == 'z' && count == 0) {
                    bin.insert(bin.end(), {0, 0, 0, 0});
                    continue;
                }
                if (c < '!' || c > 'u') continue;
                tuple = tuple * 85 + (c - '!');
                if (++count == 5) {
                    for (int k = 3; k >= 0; --k) bin.push_back(static_cast<uint8_t>((tuple >> (k * 8)) & 0xFF));
                    tuple = 0;
                    count = 0;
                }
            }
            if (count > 0) {
                for (int k = count; k < 5; ++k) tuple = tuple * 85 + 84;
                for (int k = 3; k >= 4 - count; --k)
                    bin.push_back(static_cast<uint8_t>((tuple >> (k * 8)) & 0xFF));
            }
            out.swap(bin);
        } else {
            return false;  // 不支持的滤镜（如 DCTDecode 图片流）
        }
    }
    return true;
}

void PdfDocument::loadFont(const PdfPtr& fontDict, FontInfo& info) {
    info = FontInfo();
    if (!fontDict || fontDict->type != ObjType::Dict) return;

    auto subtypeIt = fontDict->dict.find("Subtype");
    std::string subtype;
    if (subtypeIt != fontDict->dict.end() && subtypeIt->second &&
        subtypeIt->second->type == ObjType::Name) {
        subtype = subtypeIt->second->text;
    }
    info.twoByte = (subtype == "Type0");

    if (info.twoByte) {
        auto encIt = fontDict->dict.find("Encoding");
        if (encIt != fontDict->dict.end()) {
            PdfPtr enc = resolve(encIt->second);
            if (enc && enc->type == ObjType::Name) {
                const std::string& name = enc->text;
                if (name.find("UCS2") != std::string::npos ||
                    name.find("UTF16") != std::string::npos) {
                    info.ucs2 = true;
                }
            }
        }
    }

    auto tuIt = fontDict->dict.find("ToUnicode");
    if (tuIt != fontDict->dict.end()) {
        PdfPtr tu = resolve(tuIt->second);
        std::vector<uint8_t> data;
        if (tu && decodeStream(tu, data)) {
            std::string cmap(reinterpret_cast<const char*>(data.data()), data.size());
            parseToUnicode(cmap, info);
        }
    }

    // ---- 字宽表 ----
    info.defaultWidth = info.twoByte ? 1000.0 : 500.0;
    auto wIt = fontDict->dict.find("Widths");
    if (!info.twoByte && wIt != fontDict->dict.end()) {
        PdfPtr widths = resolve(wIt->second);
        int firstChar = 0;
        auto fcIt = fontDict->dict.find("FirstChar");
        if (fcIt != fontDict->dict.end()) {
            PdfPtr fc = resolve(fcIt->second);
            if (fc && fc->type == ObjType::Number) firstChar = static_cast<int>(fc->number);
        }
        if (widths && widths->type == ObjType::Array) {
            for (size_t i = 0; i < widths->items.size(); ++i) {
                PdfPtr w = resolve(widths->items[i]);
                if (w && w->type == ObjType::Number) {
                    info.widths[static_cast<uint32_t>(firstChar + static_cast<int>(i))] = w->number;
                }
            }
            info.hasWidths = !info.widths.empty();
        }
    }

    if (info.twoByte) {
        // Type0 复合字体：宽度表在子字体字典的 /W 与 /DW 中
        PdfPtr desc;
        auto dIt = fontDict->dict.find("DescendantFonts");
        if (dIt != fontDict->dict.end()) {
            PdfPtr arr = resolve(dIt->second);
            if (arr && arr->type == ObjType::Array && !arr->items.empty()) {
                desc = resolve(arr->items[0]);
            } else if (arr && arr->type == ObjType::Dict) {
                desc = arr;
            }
        }
        if (desc && desc->type == ObjType::Dict) {
            auto dwIt = desc->dict.find("DW");
            if (dwIt != desc->dict.end()) {
                PdfPtr dw = resolve(dwIt->second);
                if (dw && dw->type == ObjType::Number) info.defaultWidth = dw->number;
            }
            auto wArrIt = desc->dict.find("W");
            if (wArrIt != desc->dict.end()) {
                PdfPtr war = resolve(wArrIt->second);
                if (war && war->type == ObjType::Array) {
                    size_t i = 0;
                    while (i < war->items.size()) {
                        PdfPtr first = resolve(war->items[i]);
                        if (!first || first->type != ObjType::Number) { ++i; continue; }
                        uint32_t c0 = static_cast<uint32_t>(first->number);
                        ++i;
                        if (i >= war->items.size()) break;
                        PdfPtr second = resolve(war->items[i]);
                        if (second && second->type == ObjType::Array) {
                            // 形式一：c [w1 w2 ...]
                            for (size_t k = 0; k < second->items.size(); ++k) {
                                PdfPtr w = resolve(second->items[k]);
                                if (w && w->type == ObjType::Number) {
                                    info.widths[c0 + static_cast<uint32_t>(k)] = w->number;
                                }
                            }
                            ++i;
                        } else if (second && second->type == ObjType::Number) {
                            // 形式二：cFirst cLast w
                            uint32_t c1 = static_cast<uint32_t>(second->number);
                            ++i;
                            if (i >= war->items.size()) break;
                            PdfPtr w = resolve(war->items[i]);
                            double wv = (w && w->type == ObjType::Number) ? w->number
                                                                         : info.defaultWidth;
                            if (c1 >= c0 && c1 - c0 < 65536) {
                                for (uint32_t c = c0; c <= c1; ++c) info.widths[c] = wv;
                            }
                            ++i;
                        } else {
                            ++i;
                        }
                    }
                    info.hasWidths = !info.widths.empty();
                }
            }
        }
    }
}

void PdfDocument::collectPages(const PdfPtr& nodeIn, const PdfPtr& inheritedResources,
                               std::vector<std::pair<PdfPtr, PdfPtr>>& pages,
                               std::set<int>& visited, int depth) {
    if (!nodeIn || depth > 64) return;
    if (nodeIn->type == ObjType::Ref && visited.count(nodeIn->refId)) return;
    if (nodeIn->type == ObjType::Ref) visited.insert(nodeIn->refId);

    PdfPtr node = resolve(nodeIn);
    if (!node || node->type != ObjType::Dict) return;

    // 继承资源字典
    PdfPtr resources = inheritedResources;
    auto resIt = node->dict.find("Resources");
    if (resIt != node->dict.end()) resources = resolve(resIt->second);

    std::string type;
    auto typeIt = node->dict.find("Type");
    if (typeIt != node->dict.end() && typeIt->second && typeIt->second->type == ObjType::Name) {
        type = typeIt->second->text;
    }

    if (type == "Pages" || (type.empty() && node->dict.count("Kids"))) {
        auto kidsIt = node->dict.find("Kids");
        if (kidsIt == node->dict.end()) return;
        PdfPtr kids = resolve(kidsIt->second);
        if (!kids || kids->type != ObjType::Array) return;
        for (auto& kid : kids->items) {
            collectPages(kid, resources, pages, visited, depth + 1);
            if (pages.size() > 5000) return;
        }
        return;
    }

    if (type == "Page" || node->dict.count("Contents") || node->dict.count("MediaBox")) {
        // 存入解析后的字典本身（而不是引用节点），方便后续直接读取 Contents
        pages.emplace_back(node, resources);
    }
}

// =============================================================================
//  六、内容流解释：把文本算子还原为字符串
// =============================================================================

struct ContentToken {
    enum Kind { Number, String, Name, ArrayStart, ArrayEnd, Operator, End } kind = End;
    double num = 0;
    std::string text;
    std::vector<ContentToken> items;  // 数组内容
};

// 内容流词法分析器
class ContentLexer {
public:
    explicit ContentLexer(const std::string& s) : s_(s) {}

    // 读取下一个记号
    bool next(ContentToken& tok) {
        skipWhitespace();
        if (pos_ >= s_.size()) {
            tok.kind = ContentToken::End;
            return false;
        }
        unsigned char c = static_cast<unsigned char>(s_[pos_]);
        if (c == '(') {
            tok.kind = ContentToken::String;
            tok.text = readLiteralString();
            return true;
        }
        if (c == '<') {
            if (pos_ + 1 < s_.size() && s_[pos_ + 1] == '<') {
                // 内容流中极少出现字典，跳过到 ">>"
                size_t e = s_.find(">>", pos_ + 2);
                pos_ = (e == std::string::npos) ? s_.size() : e + 2;
                tok.kind = ContentToken::Operator;
                tok.text = "dict";
                return true;
            }
            tok.kind = ContentToken::String;
            tok.text = readHexString();
            return true;
        }
        if (c == '[') { ++pos_; tok.kind = ContentToken::ArrayStart; return true; }
        if (c == ']') { ++pos_; tok.kind = ContentToken::ArrayEnd; return true; }
        if (c == '/') {
            ++pos_;
            std::string name;
            while (pos_ < s_.size() && !isPdfWhite(static_cast<unsigned char>(s_[pos_])) &&
                   !isPdfDelim(static_cast<unsigned char>(s_[pos_]))) {
                name.push_back(s_[pos_++]);
            }
            tok.kind = ContentToken::Name;
            tok.text = decodeNameEscapes(name);
            return true;
        }
        if (c == '-' || c == '+' || c == '.' || (c >= '0' && c <= '9')) {
            tok.kind = ContentToken::Number;
            tok.num = readNumber();
            return true;
        }
        // 其余情况按操作符处理
        std::string op;
        while (pos_ < s_.size() && !isPdfWhite(static_cast<unsigned char>(s_[pos_])) &&
               !isPdfDelim(static_cast<unsigned char>(s_[pos_]))) {
            op.push_back(s_[pos_++]);
        }
        if (op.empty()) { ++pos_; return next(tok); }
        tok.kind = ContentToken::Operator;
        tok.text = op;
        return true;
    }

    // 跳过内嵌图像（BI ... ID <二进制> EI）
    void skipInlineImage() {
        size_t id = s_.find("ID", pos_);
        if (id == std::string::npos) { pos_ = s_.size(); return; }
        size_t p = id + 3;
        while (p + 2 <= s_.size()) {
            if (s_[p] == 'E' && s_[p + 1] == 'I') {
                bool leftOk = (p == 0) || isPdfWhite(static_cast<unsigned char>(s_[p - 1]));
                bool rightOk = (p + 2 >= s_.size()) ||
                               isPdfWhite(static_cast<unsigned char>(s_[p + 2])) ||
                               isPdfDelim(static_cast<unsigned char>(s_[p + 2]));
                if (leftOk && rightOk) { pos_ = p + 2; return; }
            }
            ++p;
        }
        pos_ = s_.size();
    }

private:
    void skipWhitespace() {
        while (pos_ < s_.size()) {
            unsigned char c = static_cast<unsigned char>(s_[pos_]);
            if (isPdfWhite(c)) { ++pos_; }
            else if (c == '%') {
                while (pos_ < s_.size() && s_[pos_] != '\n' && s_[pos_] != '\r') ++pos_;
            } else break;
        }
    }

    std::string readLiteralString() {
        ++pos_;  // '('
        std::string out;
        int depth = 1;
        while (pos_ < s_.size() && depth > 0) {
            char c = s_[pos_++];
            if (c == '\\') {
                if (pos_ >= s_.size()) break;
                char e = s_[pos_++];
                switch (e) {
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case '(': out.push_back('('); break;
                    case ')': out.push_back(')'); break;
                    case '\\': out.push_back('\\'); break;
                    case '\r':
                        if (pos_ < s_.size() && s_[pos_] == '\n') ++pos_;
                        break;
                    case '\n': break;
                    default:
                        if (e >= '0' && e <= '7') {
                            int v = e - '0';
                            for (int k = 0; k < 2 && pos_ < s_.size() &&
                                            s_[pos_] >= '0' && s_[pos_] <= '7'; ++k) {
                                v = v * 8 + (s_[pos_++] - '0');
                            }
                            out.push_back(static_cast<char>(v & 0xFF));
                        } else {
                            out.push_back(e);
                        }
                }
            } else if (c == '(') {
                ++depth;
                out.push_back(c);
            } else if (c == ')') {
                --depth;
                if (depth > 0) out.push_back(c);
            } else {
                out.push_back(c);
            }
        }
        return out;
    }

    std::string readHexString() {
        ++pos_;  // '<'
        std::string hex;
        while (pos_ < s_.size() && s_[pos_] != '>') {
            if (std::isxdigit(static_cast<unsigned char>(s_[pos_]))) hex.push_back(s_[pos_]);
            ++pos_;
        }
        if (pos_ < s_.size()) ++pos_;
        if (hex.size() % 2) hex.push_back('0');
        std::string out;
        auto nib = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            return c - 'A' + 10;
        };
        for (size_t i = 0; i + 1 < hex.size(); i += 2) {
            out.push_back(static_cast<char>((nib(hex[i]) << 4) | nib(hex[i + 1])));
        }
        return out;
    }

    double readNumber() {
        size_t start = pos_;
        if (pos_ < s_.size() && (s_[pos_] == '-' || s_[pos_] == '+')) ++pos_;
        while (pos_ < s_.size() &&
               ((s_[pos_] >= '0' && s_[pos_] <= '9') || s_[pos_] == '.')) {
            ++pos_;
        }
        std::string text = s_.substr(start, pos_ - start);
        return std::atof(text.c_str());
    }

    const std::string& s_;
    size_t pos_ = 0;
};

// 判断字符是否为 ASCII 字母或数字（用于决定是否补空格）。
bool isAsciiAlnum(uint32_t cp) {
    return (cp >= '0' && cp <= '9') || (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z');
}

// 把字节串按字体编码解码为 UTF-8 文本。
std::string decodeFontString(const std::string& raw, const FontInfo& font) {
    std::string out;
    if (font.twoByte) {
        size_t i = 0;
        while (i + 1 < raw.size()) {
            uint32_t code = (static_cast<uint32_t>(static_cast<unsigned char>(raw[i])) << 8) |
                            static_cast<unsigned char>(raw[i + 1]);
            i += 2;
            auto it = font.toUnicode.find(code);
            if (it != font.toUnicode.end()) {
                out += it->second;
            } else if (font.ucs2) {
                // Unicode 直接编码：处理代理对
                if (code >= 0xD800 && code <= 0xDBFF && i + 1 < raw.size()) {
                    uint32_t lo = (static_cast<uint32_t>(static_cast<unsigned char>(raw[i])) << 8) |
                                  static_cast<unsigned char>(raw[i + 1]);
                    if (lo >= 0xDC00 && lo <= 0xDFFF) {
                        i += 2;
                        code = 0x10000 + ((code - 0xD800) << 10) + (lo - 0xDC00);
                    }
                }
                utf8Append(out, code);
            } else {
                // 没有 ToUnicode 的 CID 字体：无法可靠映射，输出占位符
                out += "\xEF\xBF\xBD";
            }
        }
    } else {
        for (unsigned char c : raw) {
            auto it = font.toUnicode.find(c);
            if (it != font.toUnicode.end()) {
                out += it->second;
            } else {
                utf8Append(out, winAnsiChar(c));
            }
        }
    }
    return out;
}

// 计算一段字符串显示后的水平前进量（单位：1/1000 em）。
// 优先使用字体自带的宽度表；没有宽度表时按字符类型粗略估算。
double advanceWidthEm(const std::string& raw, const FontInfo& font) {
    double em = 0.0;
    if (font.hasWidths) {
        if (font.twoByte) {
            for (size_t i = 0; i + 1 < raw.size(); i += 2) {
                uint32_t code = (static_cast<uint32_t>(static_cast<unsigned char>(raw[i])) << 8) |
                                static_cast<unsigned char>(raw[i + 1]);
                auto it = font.widths.find(code);
                em += (it != font.widths.end()) ? it->second : font.defaultWidth;
            }
        } else {
            for (unsigned char c : raw) {
                auto it = font.widths.find(c);
                em += (it != font.widths.end()) ? it->second : font.defaultWidth;
            }
        }
        return em;
    }
    // 无宽度表：中日韩字符按全角、ASCII 按半角估算
    std::string u = decodeFontString(raw, font);
    size_t pos = 0;
    while (pos < u.size()) {
        uint32_t cp = utf8Next(u, pos);
        if (cp == ' ') em += 250;
        else if (cp >= 0x2E80) em += 1000;
        else em += 500;
    }
    return em;
}

// 解释内容流（runContent 的实现体）
class ContentInterpreter {
public:
    ContentInterpreter(PdfDocument& doc, std::string& out) : doc_(doc), out_(out) {}

    void setResources(const PdfPtr& resources) { resources_ = resources; }

    void run(const std::string& content, int depth) {
        if (depth > 6) return;
        ContentLexer lexer(content);
        std::vector<ContentToken> operands;
        ContentToken tok;
        while (lexer.next(tok)) {
            if (tok.kind == ContentToken::End) break;
            if (tok.kind != ContentToken::Operator) {
                operands.push_back(tok);
                if (operands.size() > 4096) operands.erase(operands.begin());
                continue;
            }
            if (tok.text == "BI") {
                lexer.skipInlineImage();
                operands.clear();
                continue;
            }
            handleOperator(tok.text, operands, depth);
            operands.clear();
        }
    }

private:
    // 取倒数第 n 个数值型操作数
    bool lastNumber(const std::vector<ContentToken>& ops, size_t back, double& v) {
        if (ops.size() < back) return false;
        const ContentToken& t = ops[ops.size() - back];
        if (t.kind != ContentToken::Number) return false;
        v = t.num;
        return true;
    }

    // 取最后一个字符串操作数
    bool lastString(const std::vector<ContentToken>& ops, std::string& s) {
        for (size_t i = ops.size(); i-- > 0;) {
            if (ops[i].kind == ContentToken::String) {
                s = ops[i].text;
                return true;
            }
        }
        return false;
    }

    // 取最后一个名字操作数
    bool lastName(const std::vector<ContentToken>& ops, std::string& s) {
        for (size_t i = ops.size(); i-- > 0;) {
            if (ops[i].kind == ContentToken::Name) {
                s = ops[i].text;
                return true;
            }
        }
        return false;
    }

    // 追加一个换行（避免连续多个换行）
    void appendNewline() {
        if (out_.empty() || out_.back() != '\n') out_.push_back('\n');
        lastCp_ = '\n';
    }

    // 文本空间 -> 页面空间的统一缩放因子（取 Tm 线性部分的模）
    double textScale() const {
        double det = tmA_ * tmD_ - tmB_ * tmC_;
        double s = std::sqrt(std::fabs(det));
        return (s > 1e-6) ? s : 1.0;
    }

    // 当前笔位在页面空间中的横坐标
    double pageX() const {
        double tx = lineX_ + x_, ty = lineY_ + y_;
        return tmA_ * tx + tmC_ * ty + tmE_;
    }

    // 当前笔位在页面空间中的纵坐标
    double pageY() const {
        double tx = lineX_ + x_, ty = lineY_ + y_;
        return tmB_ * tx + tmD_ * ty + tmF_;
    }

    // 显示一段文本：根据坐标变化推断换行与空格
    void show(const std::string& raw) {
        std::string u = decodeFontString(raw, font_);
        if (u.empty()) return;

        double scale = textScale();
        double effSize = fontSize_ * scale;   // 页面空间中的有效字号
        if (effSize <= 0.01) effSize = 12.0;
        double px = pageX();
        double py = pageY();

        if (hasLast_) {
            double dx = px - lastPageX_;
            double dy = std::fabs(py - lastPageY_);
            if (dy > effSize * 0.55) {
                appendNewline();              // 纵向变化：换行
            } else if (dx < -effSize * 0.5) {
                appendNewline();              // 回到行首：新的一行
            } else if (dx > effSize * 2.5) {
                appendNewline();              // 水平大跳跃：多栏或表格
            } else if (dx > effSize * 0.25 || pendingSpace_) {
                // 只在西文之间补空格，中文不需要
                size_t q2 = 0;
                uint32_t nextCp = utf8Next(u, q2);
                if (isAsciiAlnum(lastCp_) && isAsciiAlnum(nextCp) && !out_.empty() &&
                    out_.back() != ' ') {
                    out_.push_back(' ');
                }
            }
            pendingSpace_ = false;
        } else if (!out_.empty() && out_.back() != '\n') {
            appendNewline();                  // 新的一段
        }

        out_ += u;
        size_t q = 0;
        uint32_t c = 0;
        while (q < u.size()) c = utf8Next(u, q);
        lastCp_ = c;

        // 笔位按字宽前进（文本空间单位）
        double adv = advanceWidthEm(raw, font_) / 1000.0 * fontSize_;
        x_ += adv;
        lastPageX_ = pageX();
        lastPageY_ = pageY();
        hasLast_ = true;
    }

    void handleOperator(const std::string& op, const std::vector<ContentToken>& ops, int depth) {
        if (op == "BT") {
            tmA_ = 1; tmB_ = 0; tmC_ = 0; tmD_ = 1; tmE_ = 0; tmF_ = 0;
            lineX_ = lineY_ = 0;
            x_ = y_ = 0;
            leading_ = 0;
        } else if (op == "Tf") {
            std::string name;
            double size = 12;
            if (lastName(ops, name)) {
                auto it = fonts_.find(name);
                if (it != fonts_.end()) font_ = it->second;
                else font_ = FontInfo();
            }
            if (lastNumber(ops, 1, size) && size > 0 && size < 1000) fontSize_ = size;
        } else if (op == "Tm") {
            double a = 0, b = 0, c = 0, d = 0, e = 0, f = 0;
            if (ops.size() >= 6) {
                double vals[6];
                bool ok = true;
                for (int i = 0; i < 6; ++i) {
                    if (ops[ops.size() - 6 + i].kind != ContentToken::Number) { ok = false; break; }
                    vals[i] = ops[ops.size() - 6 + i].num;
                }
                if (ok) {
                    a = vals[0]; b = vals[1]; c = vals[2]; d = vals[3]; e = vals[4]; f = vals[5];
                    tmA_ = a; tmB_ = b; tmC_ = c; tmD_ = d; tmE_ = e; tmF_ = f;
                    lineX_ = lineY_ = 0;
                    x_ = y_ = 0;
                }
            }
        } else if (op == "Td" || op == "TD") {
            double tx = 0, ty = 0;
            if (lastNumber(ops, 2, tx) && lastNumber(ops, 1, ty)) {
                // Td/TD 相对行原点移动，并重置笔位
                lineX_ += tx;
                lineY_ += ty;
                x_ = y_ = 0;
                if (op == "TD") leading_ = -ty;
            }
        } else if (op == "T*") {
            lineY_ -= leading_;
            x_ = y_ = 0;
            hasLast_ = false;
        } else if (op == "TL") {
            double v = 0;
            if (lastNumber(ops, 1, v)) leading_ = v;
        } else if (op == "Tj") {
            std::string s;
            if (lastString(ops, s)) show(s);
        } else if (op == "TJ") {
            if (!ops.empty() && ops.back().kind == ContentToken::ArrayStart) {
                // 空数组
                return;
            }
            // 找到数组操作数（词法器把数组拆成 ArrayStart ... ArrayEnd 之间的记号）
            size_t startIdx = ops.size();
            int level = 0;
            for (size_t i = ops.size(); i-- > 0;) {
                if (ops[i].kind == ContentToken::ArrayEnd) ++level;
                else if (ops[i].kind == ContentToken::ArrayStart) {
                    if (--level <= 0) { startIdx = i; break; }
                }
            }
            if (startIdx >= ops.size()) return;
            for (size_t i = startIdx + 1; i < ops.size(); ++i) {
                if (ops[i].kind == ContentToken::String) {
                    show(ops[i].text);
                } else if (ops[i].kind == ContentToken::Number) {
                    double adj = -ops[i].num / 1000.0 * fontSize_;
                    x_ += adj;
                    lastPageX_ = pageX();
                    // 较大的负调整值表示词间空白
                    if (ops[i].num < -250) pendingSpace_ = true;
                }
            }
        } else if (op == "'") {
            lineY_ -= leading_;
            x_ = y_ = 0;
            hasLast_ = false;
            std::string s;
            if (lastString(ops, s)) show(s);
        } else if (op == "\"") {
            lineY_ -= leading_;
            x_ = y_ = 0;
            hasLast_ = false;
            std::string s;
            if (lastString(ops, s)) show(s);
        } else if (op == "Do") {
            std::string name;
            if (!lastName(ops, name) || !xobjects_) return;
            auto it = xobjects_->find(name);
            if (it == xobjects_->end()) return;
            PdfPtr xobj = doc_.resolvePublic(it->second);
            if (!xobj || xobj->type != ObjType::Dict) return;
            auto st = xobj->dict.find("Subtype");
            if (st == xobj->dict.end() || !st->second || st->second->type != ObjType::Name ||
                st->second->text != "Form") {
                return;
            }
            std::vector<uint8_t> data;
            if (!doc_.decodeStreamPublic(xobj, data)) return;
            std::string content(reinterpret_cast<const char*>(data.data()), data.size());
            ContentInterpreter sub(doc_, out_);
            sub.fontSize_ = fontSize_;
            auto resIt = xobj->dict.find("Resources");
            if (resIt != xobj->dict.end()) sub.setResources(doc_.resolvePublic(resIt->second));
            else sub.setResources(resources_);
            sub.prepare();
            sub.run(content, depth + 1);
            // 表单元内容结束后另起一行
            if (!out_.empty() && out_.back() != '\n') out_.push_back('\n');
            hasLast_ = false;
        }
    }

public:
    // 读取资源字典中的字体表与 XObject 表
    void prepare() {
        fonts_.clear();
        xobjects_ = nullptr;
        xobjectsHolder_.reset();
        if (!resources_) return;
        PdfPtr res = doc_.resolvePublic(resources_);
        if (!res || res->type != ObjType::Dict) return;

        auto fontIt = res->dict.find("Font");
        if (fontIt != res->dict.end()) {
            PdfPtr fonts = doc_.resolvePublic(fontIt->second);
            if (fonts && fonts->type == ObjType::Dict) {
                for (auto& kv : fonts->dict) {
                    PdfPtr fdict = doc_.resolvePublic(kv.second);
                    FontInfo info;
                    doc_.loadFontPublic(fdict, info);
                    fonts_[kv.first] = info;
                }
            }
        }
        auto xoIt = res->dict.find("XObject");
        if (xoIt != res->dict.end()) {
            PdfPtr xo = doc_.resolvePublic(xoIt->second);
            if (xo && xo->type == ObjType::Dict) {
                // 持有该对象，避免 xobjects_ 指向已释放的临时对象
                xobjectsHolder_ = xo;
                xobjects_ = &xobjectsHolder_->dict;
            }
        }
    }

private:
    PdfDocument& doc_;
    std::string& out_;
    PdfPtr resources_;
    PdfPtr xobjectsHolder_;                       // 持有 XObject 字典
    std::map<std::string, FontInfo> fonts_;
    const std::map<std::string, PdfPtr>* xobjects_ = nullptr;

    FontInfo font_;
    double fontSize_ = 12.0;
    // 文本矩阵 Tm 的六个分量；Td/TD 在文本空间中相对“行原点”移动
    double tmA_ = 1, tmB_ = 0, tmC_ = 0, tmD_ = 1, tmE_ = 0, tmF_ = 0;
    double lineX_ = 0, lineY_ = 0;    // 行原点相对 Tm 原点的偏移
    double x_ = 0, y_ = 0;            // 当前笔位相对行原点的偏移
    double lastPageX_ = 0, lastPageY_ = 0;  // 上一个字符结束处的页面坐标
    double leading_ = 0;              // 行距（TL）
    bool hasLast_ = false;            // 是否已有上一个字符的坐标
    bool pendingSpace_ = false;       // TJ 中出现了较大的字距间隙
    uint32_t lastCp_ = 0;             // 最近输出的码点（用于判断空格）

    friend class PdfDocument;
};

// =============================================================================
//  七、PdfDocument 对外接口
// =============================================================================
bool PdfDocument::load(std::string bytes, std::string& err) {
    buf_ = std::move(bytes);
    if (buf_.size() < 8) {
        err = "PDF 文件过短";
        return false;
    }
    if (buf_.compare(0, 5, "%PDF-") != 0) {
        size_t pos = buf_.find("%PDF-");
        if (pos == std::string::npos || pos > 1024) {
            err = "不是有效的 PDF 文件（缺少 %PDF- 文件头）";
            return false;
        }
    }
    if (!parseObjects()) {
        err = "PDF 对象解析失败";
        return false;
    }
    expandObjectStreams();

    // 检测加密 PDF
    for (const auto& kv : objects_) {
        const PdfPtr& o = kv.second;
        if (o && o->type == ObjType::Dict && o->dict.count("Encrypt")) {
            err = "该 PDF 已加密，暂不支持解析";
            return false;
        }
    }
    return true;
}

// 供内部的 ContentInterpreter 调用（避免嵌套类访问私有成员时的样板代码）
bool PdfDocument::extractText(std::string& text, std::string& err) {
    text.clear();

    // 找到文档目录（Catalog）
    PdfPtr catalog;
    for (auto& kv : objects_) {
        PdfPtr o = kv.second;
        if (!o || o->type != ObjType::Dict) continue;
        auto t = o->dict.find("Type");
        if (t != o->dict.end() && t->second && t->second->type == ObjType::Name &&
            t->second->text == "Catalog") {
            catalog = o;
            break;
        }
    }
    if (!catalog) {
        err = "PDF 中找不到文档目录（Catalog）";
        return false;
    }
    auto pagesIt = catalog->dict.find("Pages");
    if (pagesIt == catalog->dict.end()) {
        err = "PDF 中找不到页面树（Pages）";
        return false;
    }

    std::vector<std::pair<PdfPtr, PdfPtr>> pages;
    std::set<int> visited;
    collectPages(pagesIt->second, nullptr, pages, visited, 0);
    if (pages.empty()) {
        err = "PDF 中未找到页面";
        return false;
    }

    std::string all;
    for (size_t pi = 0; pi < pages.size(); ++pi) {
        PdfPtr page = pages[pi].first;
        PdfPtr resources = pages[pi].second;
        if (!page || page->type != ObjType::Dict) continue;

        // 拼接页面内容（可能是单个流或流数组）
        std::vector<uint8_t> content;
        auto cIt = page->dict.find("Contents");
        if (cIt != page->dict.end()) {
            PdfPtr contents = resolve(cIt->second);
            std::vector<PdfPtr> streams;
            if (contents && contents->type == ObjType::Array) {
                for (auto& item : contents->items) streams.push_back(resolve(item));
            } else if (contents) {
                streams.push_back(contents);
            }
            for (auto& s : streams) {
                std::vector<uint8_t> part;
                if (s && decodeStream(s, part)) {
                    content.insert(content.end(), part.begin(), part.end());
                    content.push_back('\n');
                }
            }
        }
        if (content.empty()) {
            continue;
        }

        std::string pageText;
        ContentInterpreter interp(*this, pageText);
        interp.setResources(resources);
        interp.prepare();
        std::string contentStr(reinterpret_cast<const char*>(content.data()), content.size());
        interp.run(contentStr, 0);
        pageText += "\n";
        all += pageText;
    }

    // 规整输出：压缩空行、去掉行尾空格
    std::string tidy;
    tidy.reserve(all.size());
    int blankRun = 0;
    for (size_t i = 0; i < all.size(); ++i) {
        char c = all[i];
        if (c == ' ' || c == '\t') {
            if (tidy.empty() || tidy.back() == '\n') continue;
            tidy.push_back(c);
            continue;
        }
        if (c == '\n') {
            if (!tidy.empty() && tidy.back() == ' ') tidy.pop_back();
            ++blankRun;
            continue;
        }
        if (blankRun > 0) {
            if (!tidy.empty()) tidy.push_back('\n');
            blankRun = 0;
        }
        tidy.push_back(c);
    }
    while (!tidy.empty() && (tidy.back() == '\n' || tidy.back() == ' ')) tidy.pop_back();

    text = tidy;
    if (text.empty()) {
        err = "未能从 PDF 中提取到文本（可能是扫描件或图片型 PDF）";
        return false;
    }
    return true;
}

} // namespace

// =============================================================================
//  公共入口
// =============================================================================
bool extractPdfText(const std::string& bytes, std::string& text, std::string& err) {
    PdfDocument doc;
    if (!doc.load(bytes, err)) return false;
    return doc.extractText(text, err);
}

} // namespace pc

