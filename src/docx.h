// =============================================================================
//  docx.h —— Word (.docx) 文本提取
// =============================================================================
#pragma once

#include <string>

namespace pc {

// 从 DOCX 字节流中提取纯文本。
// 规则：段落之间以 '\n' 分隔，制表符保留为 '\t'，删除修订中的文本不提取。
bool extractDocxText(const std::string& bytes, std::string& text, std::string& err);

} // namespace pc
