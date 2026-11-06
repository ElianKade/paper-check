// =============================================================================
//  inflate.cpp —— DEFLATE 解压算法实现
//
//  实现要点（对应 RFC 1951）：
//    1. 位流读取：DEFLATE 按“最低位优先”打包，因此用位缓冲区读取。
//    2. 三种块类型：00 存储块、01 固定 Huffman、10 动态 Huffman。
//    3. Huffman 解码：由码长构造“各长度码字数量 + 符号表”，
//       再按规范 Huffman 的计数法逐位解码（与 zlib/puff 的经典做法一致）。
//    4. LZ77 反引用：按长度/距离基表还原，重叠复制时逐字节拷贝。
// =============================================================================
#include "inflate.h"

#include <cstring>

namespace pc {
namespace {

constexpr int kMaxBits = 15;      // DEFLATE 最长码字位数
constexpr int kMaxSymbols = 320;  // 字面量/长度 288 + 距离 30 的余量

// -----------------------------------------------------------------------------
// 位流读取器：内部维护一个 32 位缓冲区，按需从输入字节流补充。
// -----------------------------------------------------------------------------
struct BitReader {
    const uint8_t* data;
    size_t size;
    size_t pos;
    uint32_t bitBuf;
    int bitCnt;
    bool error;

    BitReader(const uint8_t* d, size_t n)
        : data(d), size(n), pos(0), bitBuf(0), bitCnt(0), error(false) {}

    // 读取 n 个二进制位（n <= 16），低位在前。
    int bits(int need) {
        while (bitCnt < need) {
            if (pos >= size) {
                error = true;
                return 0;
            }
            bitBuf |= static_cast<uint32_t>(data[pos++]) << bitCnt;
            bitCnt += 8;
        }
        int value = static_cast<int>(bitBuf & ((1u << need) - 1));
        bitBuf >>= need;
        bitCnt -= need;
        return value;
    }

    // 丢弃当前未用完的位，对齐到字节边界（存储块使用）。
    void alignToByte() {
        int drop = bitCnt & 7;
        bitBuf >>= drop;
        bitCnt -= drop;
        // 缓冲区里剩余的是整字节，回退输入指针即可完全对齐。
        pos -= static_cast<size_t>(bitCnt / 8);
        bitBuf = 0;
        bitCnt = 0;
    }
};

// -----------------------------------------------------------------------------
// 规范 Huffman 表：count[len] 表示码长为 len 的符号个数，
// symbol[] 按码长递增排列，供解码时按“码字区间”直接定位。
// -----------------------------------------------------------------------------
struct Huffman {
    uint16_t count[kMaxBits + 1];
    uint16_t symbol[kMaxSymbols];

    // 依据码长数组构造解码表，返回 false 表示码长集合不合法（过度订阅）。
    bool build(const uint8_t* lengths, int n) {
        std::memset(count, 0, sizeof(count));
        for (int i = 0; i < n; ++i) {
            if (lengths[i] > kMaxBits) return false;
            ++count[lengths[i]];
        }
        if (count[0] == n) return false;  // 没有任何有效符号

        // 校验 Kraft 不等式：left 表示当前层还可容纳的码字数量。
        int left = 1;
        for (int len = 1; len <= kMaxBits; ++len) {
            left <<= 1;
            left -= count[len];
            if (left < 0) return false;  // 码长集合过度订阅
        }

        // 计算每种码长的起始下标。
        int offs[kMaxBits + 2];
        offs[1] = 0;
        for (int len = 1; len < kMaxBits; ++len) {
            offs[len + 1] = offs[len] + count[len];
        }
        for (int i = 0; i < n; ++i) {
            if (lengths[i] != 0) {
                symbol[offs[lengths[i]]++] = static_cast<uint16_t>(i);
            }
        }
        return true;
    }

    // 从位流解码一个符号；成功返回 true。
    bool decode(BitReader& br, int& out) const {
        int code = 0;
        int first = 0;
        int index = 0;
        for (int len = 1; len <= kMaxBits; ++len) {
            int b = br.bits(1);
            if (br.error) return false;
            code |= b;
            int cnt = count[len];
            if (code - cnt < first) {   // 命中该长度区间
                out = symbol[index + (code - first)];
                return true;
            }
            index += cnt;
            first += cnt;
            first <<= 1;
            code <<= 1;
        }
        return false;
    }
};

// 长度码（257~285）对应的基础长度与额外位数。
const int kLenBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                          31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const int kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                           2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};

// 距离码（0~29）对应的基础距离与额外位数。
const int kDistBase[30] = {1,    2,    3,    4,    5,    7,     9,     13,    17,   25,
                           33,   49,   65,   97,   129,  193,   257,   385,   513,  769,
                           1025, 1537, 2049, 3073, 4097, 6145,  8193,  12289, 16385, 24577};
const int kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3,  3,  4,  4,  5,  5,  6,
                            6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

// 构造固定 Huffman 表（块类型 01）。
void buildFixedTables(Huffman& lit, Huffman& dist) {
    uint8_t litLen[288];
    for (int i = 0; i < 144; ++i) litLen[i] = 8;
    for (int i = 144; i < 256; ++i) litLen[i] = 9;
    for (int i = 256; i < 280; ++i) litLen[i] = 7;
    for (int i = 280; i < 288; ++i) litLen[i] = 8;
    lit.build(litLen, 288);

    uint8_t distLen[30];
    for (int i = 0; i < 30; ++i) distLen[i] = 5;
    dist.build(distLen, 30);
}

// 解压一个 Huffman 编码块（类型 01 或 10），outSize 为输出上限。
bool inflateHuffmanBlock(BitReader& br, const Huffman& lit, const Huffman& dist,
                         std::vector<uint8_t>& out, std::string& err) {
    for (;;) {
        int sym = 0;
        if (!lit.decode(br, sym)) {
            err = "Huffman 解码失败（字面量/长度表）";
            return false;
        }
        if (sym < 256) {
            out.push_back(static_cast<uint8_t>(sym));
        } else if (sym == 256) {
            return true;  // 块结束
        } else {
            sym -= 257;
            if (sym >= 29) {
                err = "非法的长度码";
                return false;
            }
            int length = kLenBase[sym] + br.bits(kLenExtra[sym]);
            if (br.error) {
                err = "位流提前结束（长度）";
                return false;
            }
            int dsym = 0;
            if (!dist.decode(br, dsym) || dsym >= 30) {
                err = "非法的距离码";
                return false;
            }
            int distance = kDistBase[dsym] + br.bits(kDistExtra[dsym]);
            if (br.error) {
                err = "位流提前结束（距离）";
                return false;
            }
            if (distance <= 0 || static_cast<size_t>(distance) > out.size()) {
                err = "距离超出已输出数据范围";
                return false;
            }
            // LZ77 复制：允许重叠（distance 小于 length 时逐字节拷贝即可）。
            size_t src = out.size() - static_cast<size_t>(distance);
            for (int i = 0; i < length; ++i) {
                out.push_back(out[src + static_cast<size_t>(i)]);
            }
        }
        if (out.size() > (256u << 20)) {   // 单项输出上限 256MB，防止畸形数据耗尽内存
            err = "解压结果过大";
            return false;
        }
    }
}

} // namespace

// -----------------------------------------------------------------------------
// 原始 DEFLATE 解压主循环
// -----------------------------------------------------------------------------
bool inflateRaw(const uint8_t* in, size_t inSize, std::vector<uint8_t>& out, std::string& err) {
    out.clear();
    if (in == nullptr || inSize == 0) {
        err = "输入为空";
        return false;
    }

    BitReader br(in, inSize);
    bool last = false;
    while (!last) {
        last = br.bits(1) != 0;
        int type = br.bits(2);
        if (br.error) {
            err = "位流提前结束（块头）";
            return false;
        }

        if (type == 0) {
            // ---- 存储块：直接拷贝 ----
            br.alignToByte();
            if (br.pos + 4 > br.size) {
                err = "存储块头不完整";
                return false;
            }
            unsigned len = static_cast<unsigned>(in[br.pos]) |
                           (static_cast<unsigned>(in[br.pos + 1]) << 8);
            unsigned nlen = static_cast<unsigned>(in[br.pos + 2]) |
                            (static_cast<unsigned>(in[br.pos + 3]) << 8);
            br.pos += 4;
            if ((len ^ 0xFFFFu) != nlen) {
                err = "存储块长度校验失败";
                return false;
            }
            if (br.pos + len > br.size) {
                err = "存储块数据不完整";
                return false;
            }
            out.insert(out.end(), in + br.pos, in + br.pos + len);
            br.pos += len;
        } else if (type == 1) {
            Huffman lit, dist;
            buildFixedTables(lit, dist);
            if (!inflateHuffmanBlock(br, lit, dist, out, err)) return false;
        } else if (type == 2) {
            // ---- 动态 Huffman：先读码长表，再读字面量/距离码长 ----
            int hlit = br.bits(5) + 257;
            int hdist = br.bits(5) + 1;
            int hclen = br.bits(4) + 4;
            if (br.error || hlit > 286 || hdist > 30) {
                err = "动态 Huffman 头部非法";
                return false;
            }
            static const int kOrder[19] = {16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                           11, 4,  12, 3, 13, 2, 14, 1, 15};
            uint8_t clLen[19] = {0};
            for (int i = 0; i < hclen; ++i) {
                clLen[kOrder[i]] = static_cast<uint8_t>(br.bits(3));
            }
            Huffman clTable;
            if (!clTable.build(clLen, 19)) {
                err = "码长表 Huffman 非法";
                return false;
            }

            uint8_t lengths[288 + 32] = {0};
            int idx = 0;
            const int total = hlit + hdist;
            while (idx < total) {
                int sym = 0;
                if (!clTable.decode(br, sym)) {
                    err = "码长解码失败";
                    return false;
                }
                if (sym < 16) {
                    lengths[idx++] = static_cast<uint8_t>(sym);
                } else if (sym == 16) {
                    if (idx == 0) {
                        err = "码长重复引用越界";
                        return false;
                    }
                    int rep = 3 + br.bits(2);
                    uint8_t prev = lengths[idx - 1];
                    while (rep-- > 0 && idx < total) lengths[idx++] = prev;
                } else if (sym == 17) {
                    int rep = 3 + br.bits(3);
                    while (rep-- > 0 && idx < total) lengths[idx++] = 0;
                } else {
                    int rep = 11 + br.bits(7);
                    while (rep-- > 0 && idx < total) lengths[idx++] = 0;
                }
                if (br.error) {
                    err = "位流提前结束（码长）";
                    return false;
                }
            }

            Huffman lit, dist;
            if (!lit.build(lengths, hlit) || !dist.build(lengths + hlit, hdist)) {
                err = "动态 Huffman 表非法";
                return false;
            }
            if (!inflateHuffmanBlock(br, lit, dist, out, err)) return false;
        } else {
            err = "非法的块类型（3）";
            return false;
        }
    }
    return true;
}

// -----------------------------------------------------------------------------
// zlib 包装：2 字节头 + DEFLATE 数据 + 4 字节 Adler-32
// -----------------------------------------------------------------------------
bool inflateZlib(const uint8_t* in, size_t inSize, std::vector<uint8_t>& out, std::string& err) {
    out.clear();
    if (in == nullptr || inSize < 6) {
        err = "zlib 数据过短";
        return false;
    }
    uint8_t cmf = in[0];
    uint8_t flg = in[1];
    if ((cmf & 0x0F) != 8) {
        err = "不支持的压缩方法（非 DEFLATE）";
        return false;
    }
    if ((static_cast<int>(cmf) * 256 + flg) % 31 != 0) {
        err = "zlib 头部校验失败";
        return false;
    }
    size_t offset = 2;
    if (flg & 0x20) offset += 4;  // 预置字典（本项目不支持）
    if (offset >= inSize) {
        err = "zlib 头部不完整";
        return false;
    }
    return inflateRaw(in + offset, inSize - offset, out, err);
}

} // namespace pc
