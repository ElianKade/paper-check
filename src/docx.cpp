// =============================================================================
//  docx.cpp —— DOCX 正文抽取
//
//  DOCX = ZIP 包，正文位于 word/document.xml（WordprocessingML）。
//  这里不引入完整 XML 解析器，而是用一个针对 WordprocessingML 的轻量状态机：
//    <w:t>  文本节点      -> 输出文本（并解码 XML 实体）
//    <w:p>  段落          -> 输出换行
//    <w:br/> <w:cr/>      -> 输出换行
//    <w:tab/>             -> 输出制表符
//    <w:delText> <w:instrText> -> 跳过（删除修订、域代码）
//  这样可以避免把样式名、属性等内容误当成正文。
// =============================================================================
#include "docx.h"

#include "zip.h"

#include <string>

namespace pc {
namespace {

// 判断 tag 是否以 prefix 开头且后面紧跟 '>'、'/' 或空白（即标签名完全匹配）。
bool tagIs(const std::string& tag, const char* prefix) {
    size_t n = std::char_traits<char>::length(prefix);
    if (tag.size() < n || tag.compare(0, n, prefix) != 0) return false;
    if (tag.size() == n) return true;
    char c = tag[n];
    return c == '>' || c == '/' || c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// 解码 XML 实体：&amp; &lt; &gt; &quot; &apos; &#十进制; &#x十六进制;
std::string decodeXmlEntities(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') {
            out.push_back(s[i]);
            continue;
        }
        size_t semi = s.find(';', i + 1);
        if (semi == std::string::npos || semi - i > 12) {
            out.push_back(s[i]);
            continue;
        }
        std::string ent = s.substr(i + 1, semi - i - 1);
        if (ent == "amp") out.push_back('&');
        else if (ent == "lt") out.push_back('<');
        else if (ent == "gt") out.push_back('>');
        else if (ent == "quot") out.push_back('"');
        else if (ent == "apos") out.push_back('\'');
        else if (ent == "nbsp") out += "\xC2\xA0";
        else if (ent.size() > 1 && ent[0] == '#') {
            uint32_t cp = 0;
            bool ok = true;
            if (ent[1] == 'x' || ent[1] == 'X') {
                for (size_t k = 2; k < ent.size(); ++k) {
                    char c = ent[k];
                    int v = -1;
                    if (c >= '0' && c <= '9') v = c - '0';
                    else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
                    else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
                    if (v < 0) { ok = false; break; }
                    cp = cp * 16 + static_cast<uint32_t>(v);
                }
            } else {
                for (size_t k = 1; k < ent.size(); ++k) {
                    char c = ent[k];
                    if (c < '0' || c > '9') { ok = false; break; }
                    cp = cp * 10 + static_cast<uint32_t>(c - '0');
                }
            }
            if (ok && cp > 0 && cp <= 0x10FFFF) {
                // 复用公共的 UTF-8 编码器（通过局部实现避免引入额外依赖）
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
                i = semi;
                continue;
            }
            out.push_back(s[i]);
        } else {
            out.push_back(s[i]);
            continue;
        }
        i = semi;  // 跳过整个实体
    }
    return out;
}

// 把连续空行压缩为一个换行，并去掉行首尾空白，保证输出干净。
std::string tidyText(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    int newlineRun = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (c == '\n' || c == '\r') {
            ++newlineRun;
            continue;
        }
        if (c == ' ' || c == '\t') {
            // 行首空白直接丢弃；行内空白保留
            if (out.empty() || out.back() == '\n') continue;
            out.push_back(c);
            continue;
        }
        if (newlineRun > 0) {
            if (!out.empty()) out.push_back('\n');
            newlineRun = 0;
        }
        out.push_back(c);
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
    return out;
}

} // namespace

bool extractDocxText(const std::string& bytes, std::string& text, std::string& err) {
    text.clear();
    ZipArchive zip;
    if (!zip.open(bytes, err)) return false;

    // 正文 XML 的标准位置；个别生成器会在前面加 "/"。
    std::string xml;
    if (zip.hasFile("word/document.xml")) {
        if (!zip.extract("word/document.xml", xml, err)) return false;
    } else if (zip.hasFile("/word/document.xml")) {
        if (!zip.extract("/word/document.xml", xml, err)) return false;
    } else {
        err = "DOCX 中找不到 word/document.xml，可能不是有效的 Word 文档";
        return false;
    }

    std::string out;
    out.reserve(xml.size() / 2);

    size_t pos = 0;
    bool inText = false;      // 位于 <w:t> 内
    bool skipping = false;    // 位于需要跳过的标签内（w:delText / w:instrText）

    while (pos < xml.size()) {
        size_t lt = xml.find('<', pos);
        if (lt == std::string::npos) break;

        // 标签之前的裸文本（正常情况下 WordprocessingML 不会有）
        if (inText && lt > pos) {
            out += decodeXmlEntities(xml.substr(pos, lt - pos));
        }
        pos = lt;

        // 注释 <!-- --> 与处理指令 <? ?>
        if (xml.compare(pos, 4, "<!--") == 0) {
            size_t end = xml.find("-->", pos + 4);
            pos = (end == std::string::npos) ? xml.size() : end + 3;
            continue;
        }
        if (xml.compare(pos, 2, "<?") == 0) {
            size_t end = xml.find("?>", pos + 2);
            pos = (end == std::string::npos) ? xml.size() : end + 2;
            continue;
        }
        if (xml.compare(pos, 9, "<![CDATA[") == 0) {
            size_t end = xml.find("]]>", pos + 9);
            if (end == std::string::npos) break;
            if (inText) out.append(xml, pos + 9, end - pos - 9);
            pos = end + 3;
            continue;
        }

        size_t gt = xml.find('>', pos);
        if (gt == std::string::npos) break;
        std::string tag = xml.substr(pos + 1, gt - pos - 1);
        pos = gt + 1;

        bool closing = !tag.empty() && tag[0] == '/';
        bool selfClosing = !tag.empty() && tag.back() == '/';
        std::string name = closing ? tag.substr(1) : tag;

        if (closing) {
            if (tagIs(name, "w:t")) inText = false;
            if (tagIs(name, "w:delText")) skipping = false;
            if (tagIs(name, "w:instrText")) skipping = false;
            if (tagIs(name, "w:p")) out.push_back('\n');  // 段落结束
            continue;
        }

        if (tagIs(name, "w:p")) {
            // 段落开始：保证与上一段之间有换行（tidyText 会压缩多余换行）
            out.push_back('\n');
        } else if (tagIs(name, "w:t")) {
            if (!selfClosing) {
                inText = true;
                skipping = false;
            }
        } else if (tagIs(name, "w:delText") || tagIs(name, "w:instrText")) {
            if (!selfClosing) skipping = true;
        } else if (tagIs(name, "w:br") || tagIs(name, "w:cr")) {
            out.push_back('\n');
        } else if (tagIs(name, "w:tab")) {
            out.push_back('\t');
        }
        (void)skipping;
    }

    text = tidyText(out);
    if (text.empty()) {
        err = "DOCX 中未提取到文本内容";
        return false;
    }
    return true;
}

} // namespace pc
