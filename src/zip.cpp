// =============================================================================
//  zip.cpp —— ZIP 中央目录解析与条目解压
//
//  ZIP 结构速览：
//    [本地文件头 + 数据] ... [中央目录] [中央目录结束记录 EOCD]
//  中央目录中的名字/大小/偏移是可靠来源；本地文件头中的大小字段在有
//  “数据描述符”时可能为 0，因此这里只用本地文件头定位数据起点。
// =============================================================================
#include "zip.h"

#include "inflate.h"

#include <cstring>

namespace pc {
namespace {

// 小端读取工具函数
uint16_t rd16(const std::string& d, size_t p) {
    return static_cast<uint16_t>(static_cast<uint8_t>(d[p]) |
                                 (static_cast<uint16_t>(static_cast<uint8_t>(d[p + 1])) << 8));
}

uint32_t rd32(const std::string& d, size_t p) {
    return static_cast<uint32_t>(static_cast<uint8_t>(d[p])) |
           (static_cast<uint32_t>(static_cast<uint8_t>(d[p + 1])) << 8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(d[p + 2])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(d[p + 3])) << 24);
}

constexpr uint32_t kEocdSig = 0x06054b50;      // 中央目录结束记录
constexpr uint32_t kCentralSig = 0x02014b50;   // 中央目录条目
constexpr uint32_t kLocalSig = 0x04034b50;     // 本地文件头

} // namespace

bool ZipArchive::open(const std::string& data, std::string& err) {
    data_ = data;
    entries_.clear();

    if (data_.size() < 22) {
        err = "ZIP 数据过短";
        return false;
    }

    // 从尾部向前搜索 EOCD（其注释区最长 65535 字节）。
    size_t eocd = std::string::npos;
    size_t minPos = data_.size() > 65557 ? data_.size() - 65557 : 0;
    for (size_t i = data_.size() - 22 + 1; i-- > minPos;) {
        if (rd32(data_, i) == kEocdSig) {
            eocd = i;
            break;
        }
    }
    if (eocd == std::string::npos) {
        err = "找不到 ZIP 中央目录结束记录";
        return false;
    }

    uint16_t count = rd16(data_, eocd + 10);
    uint32_t cdOffset = rd32(data_, eocd + 16);
    if (cdOffset >= data_.size()) {
        err = "ZIP 中央目录偏移非法";
        return false;
    }

    // 顺序读取中央目录中的每个条目。
    size_t p = cdOffset;
    for (uint16_t i = 0; i < count; ++i) {
        if (p + 46 > data_.size() || rd32(data_, p) != kCentralSig) {
            err = "ZIP 中央目录条目损坏";
            return false;
        }
        ZipEntry e;
        e.method = rd16(data_, p + 10);
        e.compSize = rd32(data_, p + 20);
        e.uncompSize = rd32(data_, p + 24);
        uint16_t nameLen = rd16(data_, p + 28);
        uint16_t extraLen = rd16(data_, p + 30);
        uint16_t commentLen = rd16(data_, p + 32);
        e.localOffset = rd32(data_, p + 42);
        if (p + 46 + nameLen > data_.size()) {
            err = "ZIP 条目文件名越界";
            return false;
        }
        e.name = data_.substr(p + 46, nameLen);
        entries_.push_back(e);
        p += 46u + nameLen + extraLen + commentLen;
    }
    return true;
}

int ZipArchive::findEntry(const std::string& name) const {
    for (size_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].name == name) return static_cast<int>(i);
    }
    return -1;
}

bool ZipArchive::hasFile(const std::string& name) const {
    return findEntry(name) >= 0;
}

bool ZipArchive::extract(const std::string& name, std::string& out, std::string& err) const {
    out.clear();
    int idx = findEntry(name);
    if (idx < 0) {
        err = "ZIP 中不存在文件：" + name;
        return false;
    }
    const ZipEntry& e = entries_[static_cast<size_t>(idx)];

    // 读取本地文件头以获得数据区起点。
    size_t lp = e.localOffset;
    if (lp + 30 > data_.size() || rd32(data_, lp) != kLocalSig) {
        err = "ZIP 本地文件头损坏：" + name;
        return false;
    }
    uint16_t nameLen = rd16(data_, lp + 26);
    uint16_t extraLen = rd16(data_, lp + 28);
    size_t dataStart = lp + 30u + nameLen + extraLen;
    if (dataStart + e.compSize > data_.size()) {
        err = "ZIP 条目数据越界：" + name;
        return false;
    }
    const uint8_t* src = reinterpret_cast<const uint8_t*>(data_.data() + dataStart);

    if (e.method == 0) {  // 存储：直接拷贝
        out.assign(data_, dataStart, e.compSize);
        return true;
    }
    if (e.method == 8) {  // deflate：解压
        std::vector<uint8_t> raw;
        if (!inflateRaw(src, e.compSize, raw, err)) {
            err = "解压 ZIP 条目失败：" + name;
            return false;
        }
        out.assign(reinterpret_cast<const char*>(raw.data()), raw.size());
        return true;
    }
    err = "不支持的 ZIP 压缩方法：" + std::to_string(e.method);
    return false;
}

} // namespace pc
