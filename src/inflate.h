// =============================================================================
//  inflate.h —— 纯 C++ 实现的 DEFLATE / zlib 解压器
//  不依赖 zlib，用于解析 DOCX（ZIP 容器）与 PDF（FlateDecode 流）。
// =============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pc {

// 解压 RFC1951 原始的 DEFLATE 数据。
bool inflateRaw(const uint8_t* in, size_t inSize, std::vector<uint8_t>& out, std::string& err);

// 解压 RFC1950 的 zlib 数据（含 2 字节头与 Adler-32 校验）。
bool inflateZlib(const uint8_t* in, size_t inSize, std::vector<uint8_t>& out, std::string& err);

} // namespace pc
