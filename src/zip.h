// =============================================================================
//  zip.h —— 极简 ZIP 读取器
//  DOCX 本质是一个 ZIP 容器，本模块只实现“按文件名取出并解压”这一必要功能。
// =============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pc {

// ZIP 中央目录中的一条记录。
struct ZipEntry {
    std::string name;        // 文件名（UTF-8）
    uint16_t method = 0;     // 0=存储, 8=deflate
    uint32_t compSize = 0;   // 压缩后大小
    uint32_t uncompSize = 0; // 原始大小
    uint32_t localOffset = 0;// 本地文件头在文件中的偏移
};

// ZIP 归档读取器：构造函数不做事，open() 解析中央目录。
class ZipArchive {
public:
    // 解析内存中的 ZIP 数据，建立条目索引。
    bool open(const std::string& data, std::string& err);

    // 判断是否存在指定名字的条目。
    bool hasFile(const std::string& name) const;

    // 取出并解压指定条目；out 为原始字节。返回是否成功。
    bool extract(const std::string& name, std::string& out, std::string& err) const;

    // 访问全部条目（用于调试与扩展）。
    const std::vector<ZipEntry>& entries() const { return entries_; }

private:
    // 在条目表中线性查找，返回下标，找不到返回 -1。
    int findEntry(const std::string& name) const;

    std::string data_;               // 归档的原始字节
    std::vector<ZipEntry> entries_;  // 中央目录条目
};

} // namespace pc
