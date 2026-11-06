// =============================================================================
//  pdf.h —— PDF 文本提取
//  支持：经典对象/对象流、FlateDecode/ASCIIHexDecode、ToUnicode CMap、
//        Identity-H / UniGB-UCS2-H 等常见中文编码、页面树遍历。
// =============================================================================
#pragma once

#include <string>

namespace pc {

// 从 PDF 字节流中提取纯文本；扫描版 PDF（无文本层）会返回 false。
bool extractPdfText(const std::string& bytes, std::string& text, std::string& err);

} // namespace pc
