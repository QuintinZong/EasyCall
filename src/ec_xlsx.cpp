// EasyCall - .xlsx (zip+deflate+xml) 与 .csv 导入实现
// 自带极简 ZIP 读取器 + INFLATE 解压器, 无第三方依赖
#include "ec_xlsx.h"
#include "ec_common.h"
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <map>

// ==================== DEFLATE (INFLATE) ====================
// ------------------------------------------------------------------
// INFLATE 位流读取器 + 规范 Huffman 解码说明:
// DEFLATE 的码流按"位"读取, 且每个码按 LSB-first(低位在前)拼装;
// Huffman 码表是"规范码": 同长度码字连续排列且按符号序递增, 因此只需
// 存 "每个码长的符号个数 cnt[len]" 即可重建整棵树:
//   offs[len]  = 长度为 len 的码字区间起点;
//   syms[index] = 该区间内按符号序号排列的符号;
// 解码时按位累积 code, 落入 [first, first+count) 即命中对应符号。
// ------------------------------------------------------------------
// 位流读取器: 逐位吐出码流(LSB 优先), 支持按字节对齐
struct Bits {
    const uint8_t* p; size_t n, pos;   // 输入缓冲区/总长/当前字节位置
    uint32_t buf; int cnt; bool fail;  // 当前字节缓冲/剩余位数/出错标志
    // 功能: 读 1 个位(最低位先出); 数据耗尽时置 fail 并返回 0
    int bit() {
        if (cnt == 0) {
            if (pos >= n) { fail = true; return 0; }
            buf = p[pos++]; cnt = 8;
        }
        int b = buf & 1; buf >>= 1; cnt--;
        return b;
    }
    // 功能: 连续读 k 个位, 拼成整数(先读出的位为低位)
    uint32_t bits(int k) {
        uint32_t v = 0;
        for (int i = 0; i < k; i++) v |= (uint32_t)bit() << i;
        return v;
    }
    void alignByte() { cnt = 0; }  // 丢弃当前字节剩余位, 对齐下一字节
};

// 功能: 由符号码长数组构建规范 Huffman 查询表
// 参数: lens 各符号码长(0 表示未出现, 合法范围 1..15); n 符号总数;
//       offs [出参] 长度 17 的偏移表, offs[len] 为长度为 len 的码区间起点;
//       syms [出参] 按规范码顺序排好的符号表
// 返回: true=成功; false=码长非法(>15)
static bool BuildTable(const uint8_t* lens, int n, uint16_t* offs, uint16_t* syms) {
    int cnt[16] = {0};
    for (int i = 0; i < n; i++) {
        if (lens[i] > 15) return false;
        if (lens[i]) cnt[lens[i]]++;
    }
    offs[0] = 0; offs[1] = 0;
    for (int l = 1; l <= 15; l++) offs[l + 1] = offs[l] + cnt[l];   // 前缀和: 各区段起点
    uint16_t next[16];
    memcpy(next, offs, sizeof next);
    for (int sym = 0; sym < n; sym++)
        if (lens[sym]) syms[next[lens[sym]]++] = (uint16_t)sym;     // 同码长内按符号序填入
    return true;
}

// 功能: 从位流解码一个 Huffman 符号(规范码, 逐位累积匹配)
// 参数: br 位流; offs/syms 由 BuildTable 生成的查询表
// 返回: 解码出的符号; 码流耗尽或超出 15 位时返回 -1
static int DecodeSym(Bits& br, const uint16_t* offs, const uint16_t* syms) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; len++) {
        code |= br.bit();
        int count = offs[len + 1] - offs[len];   // 当前码长对应的符号个数
        if (code - first < count) return syms[index + (code - first)];
        index += count;
        first = (first + count) << 1;            // 进入下一码长区间的起始码
        code <<= 1;
    }
    return -1;
}

// 功能: 解码"码长码长"序列(DEFLATE 动态块头部), 还原 字面量/距离 两棵树的码长数组
// 符号含义: 0-15 直接为码长; 16=重复上一码长(额外2位, 重复3..6次);
//           17=连续0(额外3位, 3..10个); 18=连续0(额外7位, 11..138个)
// 参数: br 位流; lens [出参] 码长数组; n 数组长度;
//       clOffs/clSyms 码长码(code-length code)查询表
// 返回: true=成功; false=符号非法或位流出错
static bool DecodeLengths(Bits& br, uint8_t* lens, int n,
                          const uint16_t* clOffs, const uint16_t* clSyms) {
    int i = 0;
    while (i < n) {
        int sym = DecodeSym(br, clOffs, clSyms);
        if (sym < 0 || br.fail) return false;
        if (sym < 16) { lens[i++] = (uint8_t)sym; }
        else if (sym == 16) {
            if (i == 0) return false;                       // 开头不能是"重复上一位"
            int rep = 3 + (int)br.bits(2);
            uint8_t prev = lens[i - 1];
            while (rep-- && i < n) lens[i++] = prev;
        } else if (sym == 17) {
            int rep = 3 + (int)br.bits(3);
            while (rep-- && i < n) lens[i++] = 0;
        } else {  // 18
            int rep = 11 + (int)br.bits(7);
            while (rep-- && i < n) lens[i++] = 0;
        }
    }
    return true;
}

// 功能: INFLATE 解压入口: 把 DEFLATE 压缩数据解到 out
// 处理流程: 跳过 zlib 头 -> 逐块解码(BFINAL/BTYPE) ->
//           存储块(直接拷贝 LEN) 或 Huffman 块(固定/动态码表) ->
//           LZ77 回退拷贝(len/dist 距离-长度对) -> 直至 BFINAL=1
// 参数: src 压缩数据; srcLen 压缩数据长度; out [出参] 解压结果;
//       cap 输出上限(字节, 超过即失败, 防 zip 炸弹)
// 返回: true=解压成功; false=数据损坏/超上限
static bool Inflate(const uint8_t* src, size_t srcLen, std::string& out, size_t cap) {
    // DEFLATE 规范固定的 长度码(29个)基础值表
    static const uint16_t lenBase[29] = {
        3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
    // 对应的额外位数表(长度 = 基础值 + 额外位拼出的偏移)
    static const uint8_t  lenExtra[29] = {
        0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
    // 距离码(30个)基础值表
    static const uint16_t distBase[30] = {
        1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
    // 距离码对应的额外位数表
    static const uint8_t  distExtra[30] = {
        0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };

    Bits br;
    br.p = src; br.n = srcLen; br.pos = 0; br.buf = 0; br.cnt = 0; br.fail = false;
    // 兼容 zlib 头 (CM=8 且校验通过才跳过)
    // zlib 头2字节: 高字节=CMF, 低字节=FLG; (CMF<<8|FLG) 必须能被31整除
    if (srcLen >= 2 && src[0] == 0x78 && (((uint32_t)src[0] << 8 | src[1]) % 31 == 0))
        br.pos = 2;

    out.clear();
    if (cap > 0) out.reserve(cap < (size_t)1 << 22 ? cap : (size_t)1 << 22);   // 预留容量上限4MB

    // 构造固定 Huffman 码表(规范规定的固定码长分布):
    // 字面量/长度码: 0..143 码长8, 144..255 码长9, 256..279 码长7, 280..287 码长8
    uint8_t fixLitLens[288], fixDistLens[32];
    uint16_t fixLitOffs[17], fixLitSyms[288], fixDistOffs[17], fixDistSyms[32];
    for (int i = 0; i < 144; i++) fixLitLens[i] = 8;
    for (int i = 144; i < 256; i++) fixLitLens[i] = 9;
    for (int i = 256; i < 280; i++) fixLitLens[i] = 7;
    for (int i = 280; i < 288; i++) fixLitLens[i] = 8;
    BuildTable(fixLitLens, 288, fixLitOffs, fixLitSyms);
    for (int i = 0; i < 32; i++) fixDistLens[i] = 5;   // 距离码 0..31 全部码长5
    BuildTable(fixDistLens, 32, fixDistOffs, fixDistSyms);

    bool final = false;   // BFINAL: 1=最后一块, 解完即止
    while (!final) {
        if (br.fail) return false;
        final = br.bit() != 0;
        int btype = (int)br.bits(2);   // BTYPE: 0=存储 1=固定Huffman 2=动态Huffman
        if (btype == 0) {
            // ---- 无压缩存储块: 字节对齐后读 LEN/NLEN(~LEN校验), 原样拷贝 ----
            br.alignByte();
            if (br.pos + 4 > srcLen) return false;
            uint32_t len = (uint32_t)src[br.pos] | ((uint32_t)src[br.pos + 1] << 8);
            uint32_t nlen = (uint32_t)src[br.pos + 2] | ((uint32_t)src[br.pos + 3] << 8);
            br.pos += 4;
            if ((len ^ 0xFFFF) != nlen) return false;   // NLEN 必须是 LEN 按位取反
            if (br.pos + len > srcLen) return false;
            out.append((const char*)src + br.pos, len);
            br.pos += len;
            if (out.size() > cap) return false;
        } else {
            // ---- Huffman 编码块 ----
            const uint16_t* lo; const uint16_t* ls;   // 字面量/长度树查询表
            const uint16_t* d_o; const uint16_t* d_s; // 距离树查询表
            uint8_t dynLit[288], dynDist[32];
            uint16_t dynLitOffs[17], dynLitSyms[288], dynDistOffs[17], dynDistSyms[32];
            if (btype == 1) {
                lo = fixLitOffs; ls = fixLitSyms; d_o = fixDistOffs; d_s = fixDistSyms;
            } else if (btype == 2) {
                // ---- 动态块: 先读各树节点数与"码长码"表, 再解出两棵树的码长 ----
                int hlit = (int)br.bits(5) + 257;   // 字面量/长度码个数(257..286)
                int hdist = (int)br.bits(5) + 1;    // 距离码个数(1..32)
                int hclen = (int)br.bits(4) + 4;    // 码长码个数(4..19)
                // 码长码按规范规定的乱序排列读取
                static const int clOrder[19] = { 16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };
                uint8_t clLens[19] = {0};
                for (int i = 0; i < hclen; i++) clLens[clOrder[i]] = (uint8_t)br.bits(3);
                uint16_t clOffs[17], clSyms[19];
                BuildTable(clLens, 19, clOffs, clSyms);
                if (!DecodeLengths(br, dynLit, hlit, clOffs, clSyms)) return false;
                if (!DecodeLengths(br, dynDist, hdist, clOffs, clSyms)) return false;
                BuildTable(dynLit, hlit, dynLitOffs, dynLitSyms);
                BuildTable(dynDist, hdist, dynDistOffs, dynDistSyms);
                lo = dynLitOffs; ls = dynLitSyms; d_o = dynDistOffs; d_s = dynDistSyms;
            } else return false;   // BTYPE=3 保留值, 非法

            for (;;) {
                int sym = DecodeSym(br, lo, ls);
                if (br.fail) return false;
                if (sym < 0) return false;
                if (sym < 256) {
                    out.push_back((char)sym);                     // 字面量: 直接输出该字节
                } else if (sym == 256) {
                    break;                                        // 块结束标记
                } else {
                    // ---- 长度/距离对(LZ77 回退拷贝) ----
                    if (sym > 285) return false;
                    int idx = sym - 257;                          // 长度码序号 0..28
                    uint32_t len = lenBase[idx] + br.bits(lenExtra[idx]);
                    int dsym = DecodeSym(br, d_o, d_s);           // 距离码
                    if (dsym < 0 || dsym > 29 || br.fail) return false;
                    uint32_t dist = distBase[dsym] + br.bits(distExtra[dsym]);
                    if (dist > out.size()) return false;          // 回退距离不能超过已输出数据
                    size_t sp = out.size() - dist;
                    // 逐字节从已输出区复制(允许 dist<len 时的重叠自复制)
                    for (uint32_t k = 0; k < len; k++) out.push_back(out[sp + k]);
                }
                if (out.size() > cap) return false;
            }
        }
    }
    return true;
}

// ==================== ZIP ====================
// ------------------------------------------------------------------
// ZIP 文件结构说明(本实现按此解析 .xlsx):
//   [本地文件头1][数据1][本地文件头2][数据2]...[中央目录][EOCD]
//   EOCD(End Of Central Directory): 签名 PK\x05\x06, 位于文件末尾,
//     内含 条目总数(偏移10, 2字节) 与 中央目录偏移(偏移16, 4字节);
//     因 EOCD 前可能有注释, 需从文件尾往前最多 65557 字节内搜索签名。
//   中央目录条目: 签名 PK\x01\x02, 含 压缩方法(偏移10)、压缩大小(20)、
//     解压大小(24)、文件名长度(28)、扩展区长度(30)、注释长度(32)、
//     本地头偏移(42), 后跟变长文件名/扩展区/注释。
//   本地文件头: 签名 PK\x03\x04, 含 文件名长度(26)、扩展区长度(28),
//     其后紧接压缩数据。
// ------------------------------------------------------------------
// 功能: 从字节流读取小端 uint16 (ZIP 全部字段为小端)
static inline uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
// 功能: 从字节流读取小端 uint32
static inline uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// ZIP 内单个条目(取自中央目录)的描述
struct ZipEntry {
    std::string name;                 // 条目名(内部路径, 统一 '/' 分隔)
    uint16_t method;                  // 压缩方法: 0=存储, 8=deflate
    uint32_t compSize, uncompSize, lhOff;   // 压缩大小 / 解压大小 / 本地文件头偏移
};

// 功能: 解析 ZIP 中央目录, 得到全部条目列表
// 步骤: 1) 从末尾前向搜索 EOCD 签名 "PK\x05\x06"
//       2) 读出 条目总数 与 中央目录偏移
//       3) 顺序遍历中央目录条目(PK\x01\x02), 记录 name/method/大小/本地头偏移
// 参数: data 整个 zip 文件内容; out [出参] 条目列表
// 返回: true=成功; false=签名缺失/结构损坏/ZIP64(不支持)
static bool ReadZipEntries(const std::vector<uint8_t>& data, std::vector<ZipEntry>& out) {
    size_t n = data.size();
    if (n < 22) return false;
    size_t eocd = SIZE_MAX;
    size_t minI = n > 65557 ? n - 65557 : 0;   // EOCD+注释最大 65557 字节, 只需搜这一段
    for (size_t i = n - 22 + 1; i-- > minI; ) {
        if (i + 4 <= n && memcmp(data.data() + i, "PK\x05\x06", 4) == 0) { eocd = i; break; }
    }
    if (eocd == SIZE_MAX) return false;
    uint16_t count = rd16(data.data() + eocd + 10);   // 条目总数
    uint32_t cdOff = rd32(data.data() + eocd + 16);   // 中央目录在文件中的偏移
    if (count == 0xFFFF || cdOff >= n) return false;   // ZIP64 不支持
    size_t pos = cdOff;
    for (uint16_t k = 0; k < count; k++) {
        if (pos + 46 > n || rd32(data.data() + pos) != 0x02014b50) return false;   // 校验中央目录签名
        ZipEntry e;
        e.method = rd16(data.data() + pos + 10);
        e.compSize = rd32(data.data() + pos + 20);
        e.uncompSize = rd32(data.data() + pos + 24);
        uint16_t nameLen = rd16(data.data() + pos + 28);
        uint16_t extraLen = rd16(data.data() + pos + 30);
        uint16_t cmtLen = rd16(data.data() + pos + 32);
        e.lhOff = rd32(data.data() + pos + 42);
        if (pos + 46 + nameLen > n) return false;
        e.name.assign((const char*)data.data() + pos + 46, nameLen);
        std::replace(e.name.begin(), e.name.end(), '\\', '/');   // 统一分隔符方便比较
        out.push_back(std::move(e));
        pos += 46 + nameLen + extraLen + cmtLen;   // 跳到下一条目
    }
    return true;
}

// 功能: 解压单个 ZIP 条目: 定位本地文件头, 取出压缩数据, 按方法解压
// 参数: data 整个 zip 文件内容; e 中央目录里记录的条目; out [出参] 解压后的原始内容
// 返回: true=成功; false=本地头损坏/方法不支持/解压失败
static bool ExtractEntry(const std::vector<uint8_t>& data, const ZipEntry& e, std::string& out) {
    size_t p = e.lhOff;
    if (p + 30 > data.size() || rd32(data.data() + p) != 0x04034b50) return false;   // 本地头签名
    uint16_t nameLen = rd16(data.data() + p + 26);
    uint16_t extraLen = rd16(data.data() + p + 28);
    size_t d = p + 30 + nameLen + extraLen;   // 压缩数据起点
    if (d + e.compSize > data.size()) return false;
    const uint8_t* raw = data.data() + d;
    if (e.method == 0) { out.assign((const char*)raw, e.compSize); return true; }   // 存储: 直接拷贝
    if (e.method != 8) return false;   // 只支持 deflate
    return Inflate(raw, e.compSize, out, e.uncompSize);
}

// ==================== XML 轻量解析 ====================
// 功能: 解码 XML 实体引用: &lt; &gt; &amp; &quot; &apos; &#xHH; &#DD;
// 参数: s 原始 XML 文本片段
// 返回: 实体解码后的字符串
static std::string XmlDecode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '&') {
            if (s.compare(i, 4, "&lt;") == 0) { out += '<'; i += 3; }
            else if (s.compare(i, 4, "&gt;") == 0) { out += '>'; i += 3; }
            else if (s.compare(i, 5, "&amp;") == 0) { out += '&'; i += 4; }
            else if (s.compare(i, 6, "&quot;") == 0) { out += '"'; i += 5; }
            else if (s.compare(i, 6, "&apos;") == 0) { out += '\''; i += 5; }
            else if (s.compare(i, 3, "&#x") == 0) {
                // 十六进制数字字符引用 &#xHH;
                size_t e = s.find(';', i + 3);
                if (e != std::string::npos) {
                    unsigned v = 0;
                    for (size_t q = i + 3; q < e; q++) {
                        char c = s[q]; v <<= 4;
                        if (c >= '0' && c <= '9') v |= (c - '0');
                        else if (c >= 'a' && c <= 'f') v |= (c - 'a' + 10);
                        else if (c >= 'A' && c <= 'F') v |= (c - 'A' + 10);
                        else { v = 0; break; }
                    }
                    std::string u = WU8(std::wstring(1, (wchar_t)v));   // 码点 -> UTF-8
                    out += u; i = e;
                } else out += s[i];
            }
            else if (s.compare(i, 2, "&#") == 0) {
                // 十进制数字字符引用 &#DD;
                size_t e = s.find(';', i + 2);
                if (e != std::string::npos) {
                    unsigned v = 0;
                    for (size_t q = i + 2; q < e; q++) {
                        char c = s[q];
                        if (c < '0' || c > '9') { v = 0; break; }
                        v = v * 10 + (c - '0');
                    }
                    std::string u = WU8(std::wstring(1, (wchar_t)v));
                    out += u; i = e;
                } else out += s[i];
            }
            else out += s[i];
        } else out += s[i];
    }
    return out;
}

// 功能: 从 from 位置起查找标签文本 tag, 且其后字符必须是 空格/'/'/'>'
//       (防止 "<t" 误匹配 "<table" 之类的前缀标签)
// 参数: xml 文本; tag 要查找的标签文本(含 '<', 如 "<si"); from 起始搜索位置
// 返回: 匹配位置; 找不到返回 npos
static size_t FindTag(const std::string& xml, const char* tag, size_t from) {
    size_t tl = strlen(tag);
    for (;;) {
        size_t p = xml.find(tag, from);
        if (p == std::string::npos) return p;
        char c = (p + tl < xml.size()) ? xml[p + tl] : '>';
        if (c == ' ' || c == '>' || c == '/') return p;
        from = p + tl;
    }
}

// 功能: 解析 sharedStrings.xml: 依次取出每个 <si>...</si> 内的所有
//       <t> 文本片段(富文本 run 会被拼接), 实体解码后存入数组
//       下标即为单元格中 t="s" 所引用的索引
// 参数: xml sharedStrings.xml 全文; out [出参] 共享字符串表
// 返回: 无
static void ParseSharedStrings(const std::string& xml, std::vector<std::string>& out) {
    size_t pos = 0;
    for (;;) {
        size_t si = FindTag(xml, "<si", pos);          // 找下一个共享字符串条目
        if (si == std::string::npos) break;
        size_t siEnd = xml.find("</si>", si);
        if (siEnd == std::string::npos) break;
        std::string value;
        size_t q = si;
        for (;;) {
            size_t t = FindTag(xml, "<t", q);          // 条目内的每个文本节点
            if (t == std::string::npos || t > siEnd) break;
            size_t tEnd = xml.find('>', t);
            if (tEnd == std::string::npos || tEnd > siEnd) break;
            if (tEnd > t && xml[tEnd - 1] == '/') { q = tEnd + 1; continue; }   // <t .../>
            size_t close = xml.find("</t>", tEnd);
            if (close == std::string::npos || close > siEnd) break;
            value += xml.substr(tEnd + 1, close - tEnd - 1);   // 拼接片段
            q = close + 4;
        }
        out.push_back(XmlDecode(value));               // 实体解码后入表
        pos = siEnd + 5;
    }
}

// 功能: 去掉字符串首尾空白(空格/Tab/回车/换行)
// 参数: s 输入字符串
// 返回: 修剪后的字符串(全空白则返回空串)
static std::wstring TrimW(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return L"";
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}

// 功能: 规范化数字文本: "123.0" -> "123", "1E3" -> "1000",
//       非纯数字串(如学号 "2023001" 已在Excel里是文本时)原样返回
// 参数: val 单元格原始值
// 返回: 规范化后的字符串
static std::string CleanNumber(const std::string& val) {
    if (val.empty()) return val;
    if (val.find_first_not_of("0123456789.-+eE") != std::string::npos) return val;   // 含其他字符不动
    const char* s = val.c_str();
    char* end = nullptr;
    double d = strtod(s, &end);
    if (end == s || *end != 0) return val;   // 不能完整解析为数字则原样返回
    if (d == (long long)d) {
        char buf[64];
        snprintf(buf, sizeof buf, "%lld", (long long)d);   // 整数值: 去掉小数点/指数
        return buf;
    }
    char buf[64];
    snprintf(buf, sizeof buf, "%g", d);
    return buf;
}

// 功能: 解析工作表 sheet1.xml: 逐 <row> 收集各列单元格值, 每行取前3列
//       作为 学号(id)/姓名(name)/班级(cls)
// 单元格解析: 列号由 r="A1" 形式的引用算出(A=1, B=2, ... 26进制);
//       t="s" 表示共享字符串(取 <v> 索引查表); t="inlineStr" 表示
//       内联文本; 其他为数值(经 CleanNumber 规范化)
// 参数: xml 工作表全文; sst 共享字符串表; out [出参] 解析出的行
// 返回: 无
static void ParseSheet(const std::string& xml, const std::vector<std::string>& sst,
                       std::vector<XRow>& out) {
    size_t pos = 0;
    for (;;) {
        size_t r1 = FindTag(xml, "<row", pos);         // 下一行
        if (r1 == std::string::npos) break;
        size_t r2 = xml.find('>', r1);
        if (r2 == std::string::npos) break;
        size_t rEnd = xml.find("</row>", r2);
        if (rEnd == std::string::npos) break;
        std::string rowXml = xml.substr(r1, rEnd - r1);   // 本行原始片段
        std::map<int, std::string> colVals;               // 列号 -> 值
        size_t q = 0;
        for (;;) {
            size_t c1 = rowXml.find("<c ", q);            // 下一单元格标签
            if (c1 == std::string::npos) break;
            size_t c2 = rowXml.find('>', c1);
            if (c2 == std::string::npos) break;
            std::string ctag = rowXml.substr(c1, c2 - c1);   // <c ...> 属性部分
            int col = -1;
            size_t ra = ctag.find("r=\"");
            if (ra != std::string::npos) {
                std::string ref = ctag.substr(ra + 3);
                size_t rb = ref.find('"');
                if (rb != std::string::npos) {
                    std::string letters;
                    for (char ch : ref.substr(0, rb))
                        if (isalpha((unsigned char)ch)) letters += (char)toupper((unsigned char)ch);
                    col = 0;
                    for (char ch : letters) col = col * 26 + (ch - 'A' + 1);   // 列字母 -> 数字(A=1)
                    col--;   // 转为0基
                }
            }
            if (col < 0) { q = c2 + 1; continue; }     // 取不到列号(异常)则跳过
            bool isShared = ctag.find("t=\"s\"") != std::string::npos;               // 共享字符串类型
            bool isInline = ctag.find("t=\"inlineStr\"") != std::string::npos;       // 内联文本类型
            std::string val;
            if (isInline) {
                // 内联文本: 值在 <t>...</t> 中
                size_t t1 = FindTag(rowXml, "<t", c1);
                if (t1 != std::string::npos && t1 < c2) {
                    size_t t2 = rowXml.find('>', t1);
                    if (t2 != std::string::npos) {
                        size_t t3 = rowXml.find("</t>", t2);
                        if (t3 != std::string::npos) val = rowXml.substr(t2 + 1, t3 - t2 - 1);
                    }
                }
            } else {
                // 普通类型: 值在 <v>...</v> 中(限定在下一个单元格标签之前)
                size_t v1 = rowXml.find("<v>", c1);
                size_t nextC = rowXml.find("<c ", c2);
                if (v1 != std::string::npos && (nextC == std::string::npos || v1 < nextC)) {
                    size_t v2 = rowXml.find("</v>", v1);
                    if (v2 != std::string::npos) {
                        val = rowXml.substr(v1 + 3, v2 - v1 - 3);
                        if (isShared) {
                            int idx = atoi(val.c_str());   // 共享字符串: 值是该表的索引
                            if (idx >= 0 && idx < (int)sst.size()) val = sst[idx];
                        } else if (!val.empty()) {
                            val = CleanNumber(val);
                        }
                    }
                }
            }
            colVals[col] = XmlDecode(val);   // 实体解码后按列号存
            q = c2 + 1;
        }
        XRow row;
        auto g = [&](int c) -> std::string {   // 取列 c 的值, 缺列给空串
            auto it = colVals.find(c);
            return it == colVals.end() ? std::string() : it->second;
        };
        row.id = TrimW(U8W(g(0)));   // 第1列: 学号
        row.name = TrimW(U8W(g(1))); // 第2列: 姓名
        row.cls = TrimW(U8W(g(2)));  // 第3列: 班级/备注
        out.push_back(row);
        pos = rEnd + 6;
    }
}

// ==================== CSV ====================
// 功能: 拆分一行 CSV: 支持双引号包裹字段、"" 转义为字面引号,
//       逗号在引号内不算分隔符(本实现不处理跨行字段)
// 参数: line 一行原始文本; out [出参] 拆分出的字段(先清空)
// 返回: 无
static void SplitCsvLine(const std::string& line, std::vector<std::string>& out) {
    out.clear();
    std::string cur;
    bool inQ = false;   // 是否处于引号内
    for (size_t i = 0; i < line.size(); i++) {
        char ch = line[i];
        if (inQ) {
            if (ch == '"') {
                if (i + 1 < line.size() && line[i + 1] == '"') { cur += '"'; i++; }   // "" -> 字面引号
                else inQ = false;   // 引号闭合
            } else cur += ch;
        } else {
            if (ch == '"') inQ = true;
            else if (ch == ',') { out.push_back(cur); cur.clear(); }   // 字段分隔
            else cur += ch;
        }
    }
    out.push_back(cur);
}

// 功能: 把 CSV 字段字节流解码成宽字符串:
//       1) 去 UTF-8 BOM; 2) 按 UTF-8 严格解码; 3) 失败则按 GBK(代码页936)回退
// 参数: s 字段原始字节
// 返回: 解码后的宽字符串
static std::wstring CsvDecode(const std::string& s) {
    if (!s.empty() && (uint8_t)s[0] == 0xEF && s.size() > 2 &&
        (uint8_t)s[1] == 0xBB && (uint8_t)s[2] == 0xBF)
        return U8W(s.substr(3));   // 带 BOM: 去掉 BOM 后按 UTF-8
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), nullptr, 0) > 0)
        return U8W(s);   // 合法 UTF-8
    int n = MultiByteToWideChar(936, 0, s.data(), (int)s.size(), nullptr, 0);   // GBK 回退
    if (n <= 0) return U8W(s);
    std::wstring w(n, 0);
    MultiByteToWideChar(936, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

// 功能: 解析整个 CSV 文本: 先按行拆分, 再每行拆字段, 取前3列
//       作为 学号(id)/姓名(name)/班级(cls)
// 参数: content CSV 全文; out [出参] 解析出的行
// 返回: 无
static void ParseCsv(const std::string& content, std::vector<XRow>& out) {
    std::vector<std::string> lines;
    std::string cur;
    for (char c : content) {
        if (c == '\n') { lines.push_back(cur); cur.clear(); }
        else if (c != '\r') cur += c;
    }
    if (!cur.empty()) lines.push_back(cur);
    for (auto& line : lines) {
        if (line.empty()) continue;
        std::vector<std::string> f;
        SplitCsvLine(line, f);
        XRow r;
        if (f.size() > 0) r.id = TrimW(CsvDecode(f[0]));
        if (f.size() > 1) r.name = TrimW(CsvDecode(f[1]));
        if (f.size() > 2) r.cls = TrimW(CsvDecode(f[2]));
        out.push_back(r);
    }
}

// ==================== 入口 ====================
// 功能: 宽字符串转小写(用于扩展名比较)
// 参数: s 输入字符串
// 返回: 全小写字符串
static std::wstring LowerW(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

// 功能: 表格导入总入口: 根据扩展名调度 xlsx(zip+xml) 或 csv 解析,
//       跳过表头行与空白行, 输出学生行列表
// 参数: path 文件完整路径; out [出参] 解析结果(学号/姓名/班级);
//       err [出参] 失败原因(中文, 可直接弹窗显示)
// 返回: true=成功且至少解析到1行; false=失败(原因见 err)
bool ImportSpreadsheet(const std::wstring& path, std::vector<XRow>& out, std::wstring& err) {
    out.clear();
    std::wstring lower = LowerW(path);
    size_t dot = lower.find_last_of(L'.');
    std::wstring ext = (dot == std::wstring::npos) ? L"" : lower.substr(dot);   // 小写扩展名(含点)

    // 读入整个文件(最大约4GB, 实际受内存限制)
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { err = L"无法打开文件"; return false; }
    DWORD hi = 0;
    DWORD size = GetFileSize(h, &hi);
    if (hi || size == 0) { CloseHandle(h); err = L"文件为空或过大"; return false; }
    std::vector<uint8_t> data(size);
    DWORD got = 0;
    if (!ReadFile(h, data.data(), size, &got, nullptr) || got != size) {
        CloseHandle(h); err = L"读取文件失败"; return false;
    }
    CloseHandle(h);

    std::vector<XRow> rows;
    if (ext == L".csv" || ext == L".txt") {
        // ---- CSV/TXT 路径 ----
        std::string content(data.begin(), data.end());
        ParseCsv(content, rows);
    } else {
        // ---- XLSX 路径(zip + xml) ----
        std::vector<ZipEntry> entries;
        if (!ReadZipEntries(data, entries)) { err = L"不是有效的 .xlsx 文件(旧版 .xls 请先用 Excel 另存为 .xlsx 或 .csv)"; return false; }
        std::string sstXml, sheetXml;
        for (auto& e : entries)
            if (e.name == "xl/sharedStrings.xml") ExtractEntry(data, e, sstXml);   // 解共享字符串表
        const ZipEntry* sheet = nullptr;
        for (auto& e : entries)
            if (e.name == "xl/worksheets/sheet1.xml") { sheet = &e; break; }   // 优先第1张表
        if (!sheet)
            for (auto& e : entries) {
                // 找不到 sheet1 时退而取第一张 .xml 工作表
                if (e.name.compare(0, 15, "xl/worksheets/") == 0 &&
                    e.name.size() > 15 && e.name.substr(e.name.size() - 4) == ".xml") {
                    sheet = &e; break;
                }
            }
        if (!sheet) { err = L"未找到工作表"; return false; }
        if (!ExtractEntry(data, *sheet, sheetXml)) { err = L"解压工作表失败"; return false; }
        std::vector<std::string> sst;
        ParseSharedStrings(sstXml, sst);   // 建立共享字符串索引表
        ParseSheet(sheetXml, sst, rows);   // 解析工作表内容
    }

    // 跳过表头行 / 空白行
    std::vector<XRow> cleaned;
    bool first = true;
    auto isHeader = [](const std::wstring& v) {   // 首行若为常见表头关键词则跳过
        std::wstring s = LowerW(v);
        return s == L"学号" || s == L"序号" || s == L"编号" || s == L"id" || s == L"no" ||
               s == L"no." || s == L"number" || s == L"姓名" || s == L"名字" || s == L"name";
    };
    for (auto& r : rows) {
        if (first) {
            first = false;
            if (isHeader(r.id) || isHeader(r.name)) continue;   // 首行是表头: 丢弃
        }
        if (r.id.empty() && r.name.empty()) continue;   // 空白行: 丢弃
        cleaned.push_back(r);
    }
    if (cleaned.empty()) { err = L"未解析到学生数据(请确认前两列为: 学号 / 姓名)"; return false; }
    out = std::move(cleaned);
    return true;
}
