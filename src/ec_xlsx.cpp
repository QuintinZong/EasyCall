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
struct Bits {
    const uint8_t* p; size_t n, pos;
    uint32_t buf; int cnt; bool fail;
    int bit() {
        if (cnt == 0) {
            if (pos >= n) { fail = true; return 0; }
            buf = p[pos++]; cnt = 8;
        }
        int b = buf & 1; buf >>= 1; cnt--;
        return b;
    }
    uint32_t bits(int k) {
        uint32_t v = 0;
        for (int i = 0; i < k; i++) v |= (uint32_t)bit() << i;
        return v;
    }
    void alignByte() { cnt = 0; }  // 丢弃当前字节剩余位, 对齐下一字节
};

static bool BuildTable(const uint8_t* lens, int n, uint16_t* offs, uint16_t* syms) {
    int cnt[16] = {0};
    for (int i = 0; i < n; i++) {
        if (lens[i] > 15) return false;
        if (lens[i]) cnt[lens[i]]++;
    }
    offs[0] = 0; offs[1] = 0;
    for (int l = 1; l <= 15; l++) offs[l + 1] = offs[l] + cnt[l];
    uint16_t next[16];
    memcpy(next, offs, sizeof next);
    for (int sym = 0; sym < n; sym++)
        if (lens[sym]) syms[next[lens[sym]]++] = (uint16_t)sym;
    return true;
}

static int DecodeSym(Bits& br, const uint16_t* offs, const uint16_t* syms) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; len++) {
        code |= br.bit();
        int count = offs[len + 1] - offs[len];
        if (code - first < count) return syms[index + (code - first)];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return -1;
}

static bool DecodeLengths(Bits& br, uint8_t* lens, int n,
                          const uint16_t* clOffs, const uint16_t* clSyms) {
    int i = 0;
    while (i < n) {
        int sym = DecodeSym(br, clOffs, clSyms);
        if (sym < 0 || br.fail) return false;
        if (sym < 16) { lens[i++] = (uint8_t)sym; }
        else if (sym == 16) {
            if (i == 0) return false;
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

static bool Inflate(const uint8_t* src, size_t srcLen, std::string& out, size_t cap) {
    static const uint16_t lenBase[29] = {
        3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
    static const uint8_t  lenExtra[29] = {
        0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
    static const uint16_t distBase[30] = {
        1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
    static const uint8_t  distExtra[30] = {
        0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };

    Bits br;
    br.p = src; br.n = srcLen; br.pos = 0; br.buf = 0; br.cnt = 0; br.fail = false;
    // 兼容 zlib 头 (CM=8 且校验通过才跳过)
    if (srcLen >= 2 && src[0] == 0x78 && (((uint32_t)src[0] << 8 | src[1]) % 31 == 0))
        br.pos = 2;

    out.clear();
    if (cap > 0) out.reserve(cap < (size_t)1 << 22 ? cap : (size_t)1 << 22);

    uint8_t fixLitLens[288], fixDistLens[32];
    uint16_t fixLitOffs[17], fixLitSyms[288], fixDistOffs[17], fixDistSyms[32];
    for (int i = 0; i < 144; i++) fixLitLens[i] = 8;
    for (int i = 144; i < 256; i++) fixLitLens[i] = 9;
    for (int i = 256; i < 280; i++) fixLitLens[i] = 7;
    for (int i = 280; i < 288; i++) fixLitLens[i] = 8;
    BuildTable(fixLitLens, 288, fixLitOffs, fixLitSyms);
    for (int i = 0; i < 32; i++) fixDistLens[i] = 5;
    BuildTable(fixDistLens, 32, fixDistOffs, fixDistSyms);

    bool final = false;
    while (!final) {
        if (br.fail) return false;
        final = br.bit() != 0;
        int btype = (int)br.bits(2);
        if (btype == 0) {
            br.alignByte();
            if (br.pos + 4 > srcLen) return false;
            uint32_t len = (uint32_t)src[br.pos] | ((uint32_t)src[br.pos + 1] << 8);
            uint32_t nlen = (uint32_t)src[br.pos + 2] | ((uint32_t)src[br.pos + 3] << 8);
            br.pos += 4;
            if ((len ^ 0xFFFF) != nlen) return false;
            if (br.pos + len > srcLen) return false;
            out.append((const char*)src + br.pos, len);
            br.pos += len;
            if (out.size() > cap) return false;
        } else {
            const uint16_t* lo; const uint16_t* ls;
            const uint16_t* d_o; const uint16_t* d_s;
            uint8_t dynLit[288], dynDist[32];
            uint16_t dynLitOffs[17], dynLitSyms[288], dynDistOffs[17], dynDistSyms[32];
            if (btype == 1) {
                lo = fixLitOffs; ls = fixLitSyms; d_o = fixDistOffs; d_s = fixDistSyms;
            } else if (btype == 2) {
                int hlit = (int)br.bits(5) + 257;
                int hdist = (int)br.bits(5) + 1;
                int hclen = (int)br.bits(4) + 4;
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
            } else return false;

            for (;;) {
                int sym = DecodeSym(br, lo, ls);
                if (br.fail) return false;
                if (sym < 0) return false;
                if (sym < 256) {
                    out.push_back((char)sym);
                } else if (sym == 256) {
                    break;
                } else {
                    if (sym > 285) return false;
                    int idx = sym - 257;
                    uint32_t len = lenBase[idx] + br.bits(lenExtra[idx]);
                    int dsym = DecodeSym(br, d_o, d_s);
                    if (dsym < 0 || dsym > 29 || br.fail) return false;
                    uint32_t dist = distBase[dsym] + br.bits(distExtra[dsym]);
                    if (dist > out.size()) return false;
                    size_t sp = out.size() - dist;
                    for (uint32_t k = 0; k < len; k++) out.push_back(out[sp + k]);
                }
                if (out.size() > cap) return false;
            }
        }
    }
    return true;
}

// ==================== ZIP ====================
static inline uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

struct ZipEntry {
    std::string name;
    uint16_t method;
    uint32_t compSize, uncompSize, lhOff;
};

static bool ReadZipEntries(const std::vector<uint8_t>& data, std::vector<ZipEntry>& out) {
    size_t n = data.size();
    if (n < 22) return false;
    size_t eocd = SIZE_MAX;
    size_t minI = n > 65557 ? n - 65557 : 0;
    for (size_t i = n - 22 + 1; i-- > minI; ) {
        if (i + 4 <= n && memcmp(data.data() + i, "PK\x05\x06", 4) == 0) { eocd = i; break; }
    }
    if (eocd == SIZE_MAX) return false;
    uint16_t count = rd16(data.data() + eocd + 10);
    uint32_t cdOff = rd32(data.data() + eocd + 16);
    if (count == 0xFFFF || cdOff >= n) return false;   // ZIP64 不支持
    size_t pos = cdOff;
    for (uint16_t k = 0; k < count; k++) {
        if (pos + 46 > n || rd32(data.data() + pos) != 0x02014b50) return false;
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
        std::replace(e.name.begin(), e.name.end(), '\\', '/');
        out.push_back(std::move(e));
        pos += 46 + nameLen + extraLen + cmtLen;
    }
    return true;
}

static bool ExtractEntry(const std::vector<uint8_t>& data, const ZipEntry& e, std::string& out) {
    size_t p = e.lhOff;
    if (p + 30 > data.size() || rd32(data.data() + p) != 0x04034b50) return false;
    uint16_t nameLen = rd16(data.data() + p + 26);
    uint16_t extraLen = rd16(data.data() + p + 28);
    size_t d = p + 30 + nameLen + extraLen;
    if (d + e.compSize > data.size()) return false;
    const uint8_t* raw = data.data() + d;
    if (e.method == 0) { out.assign((const char*)raw, e.compSize); return true; }
    if (e.method != 8) return false;
    return Inflate(raw, e.compSize, out, e.uncompSize);
}

// ==================== XML 轻量解析 ====================
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
                    std::string u = WU8(std::wstring(1, (wchar_t)v));
                    out += u; i = e;
                } else out += s[i];
            }
            else if (s.compare(i, 2, "&#") == 0) {
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

static void ParseSharedStrings(const std::string& xml, std::vector<std::string>& out) {
    size_t pos = 0;
    for (;;) {
        size_t si = FindTag(xml, "<si", pos);
        if (si == std::string::npos) break;
        size_t siEnd = xml.find("</si>", si);
        if (siEnd == std::string::npos) break;
        std::string value;
        size_t q = si;
        for (;;) {
            size_t t = FindTag(xml, "<t", q);
            if (t == std::string::npos || t > siEnd) break;
            size_t tEnd = xml.find('>', t);
            if (tEnd == std::string::npos || tEnd > siEnd) break;
            if (tEnd > t && xml[tEnd - 1] == '/') { q = tEnd + 1; continue; }   // <t .../>
            size_t close = xml.find("</t>", tEnd);
            if (close == std::string::npos || close > siEnd) break;
            value += xml.substr(tEnd + 1, close - tEnd - 1);
            q = close + 4;
        }
        out.push_back(XmlDecode(value));
        pos = siEnd + 5;
    }
}

static std::wstring TrimW(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return L"";
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}

static std::string CleanNumber(const std::string& val) {
    if (val.empty()) return val;
    if (val.find_first_not_of("0123456789.-+eE") != std::string::npos) return val;
    const char* s = val.c_str();
    char* end = nullptr;
    double d = strtod(s, &end);
    if (end == s || *end != 0) return val;
    if (d == (long long)d) {
        char buf[64];
        snprintf(buf, sizeof buf, "%lld", (long long)d);
        return buf;
    }
    char buf[64];
    snprintf(buf, sizeof buf, "%g", d);
    return buf;
}

static void ParseSheet(const std::string& xml, const std::vector<std::string>& sst,
                       std::vector<XRow>& out) {
    size_t pos = 0;
    for (;;) {
        size_t r1 = FindTag(xml, "<row", pos);
        if (r1 == std::string::npos) break;
        size_t r2 = xml.find('>', r1);
        if (r2 == std::string::npos) break;
        size_t rEnd = xml.find("</row>", r2);
        if (rEnd == std::string::npos) break;
        std::string rowXml = xml.substr(r1, rEnd - r1);
        std::map<int, std::string> colVals;
        size_t q = 0;
        for (;;) {
            size_t c1 = rowXml.find("<c ", q);
            if (c1 == std::string::npos) break;
            size_t c2 = rowXml.find('>', c1);
            if (c2 == std::string::npos) break;
            std::string ctag = rowXml.substr(c1, c2 - c1);
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
                    for (char ch : letters) col = col * 26 + (ch - 'A' + 1);
                    col--;
                }
            }
            if (col < 0) { q = c2 + 1; continue; }
            bool isShared = ctag.find("t=\"s\"") != std::string::npos;
            bool isInline = ctag.find("t=\"inlineStr\"") != std::string::npos;
            std::string val;
            if (isInline) {
                size_t t1 = FindTag(rowXml, "<t", c1);
                if (t1 != std::string::npos && t1 < c2) {
                    size_t t2 = rowXml.find('>', t1);
                    if (t2 != std::string::npos) {
                        size_t t3 = rowXml.find("</t>", t2);
                        if (t3 != std::string::npos) val = rowXml.substr(t2 + 1, t3 - t2 - 1);
                    }
                }
            } else {
                size_t v1 = rowXml.find("<v>", c1);
                size_t nextC = rowXml.find("<c ", c2);
                if (v1 != std::string::npos && (nextC == std::string::npos || v1 < nextC)) {
                    size_t v2 = rowXml.find("</v>", v1);
                    if (v2 != std::string::npos) {
                        val = rowXml.substr(v1 + 3, v2 - v1 - 3);
                        if (isShared) {
                            int idx = atoi(val.c_str());
                            if (idx >= 0 && idx < (int)sst.size()) val = sst[idx];
                        } else if (!val.empty()) {
                            val = CleanNumber(val);
                        }
                    }
                }
            }
            colVals[col] = XmlDecode(val);
            q = c2 + 1;
        }
        XRow row;
        auto g = [&](int c) -> std::string {
            auto it = colVals.find(c);
            return it == colVals.end() ? std::string() : it->second;
        };
        row.id = TrimW(U8W(g(0)));
        row.name = TrimW(U8W(g(1)));
        row.cls = TrimW(U8W(g(2)));
        out.push_back(row);
        pos = rEnd + 6;
    }
}

// ==================== CSV ====================
static void SplitCsvLine(const std::string& line, std::vector<std::string>& out) {
    out.clear();
    std::string cur;
    bool inQ = false;
    for (size_t i = 0; i < line.size(); i++) {
        char ch = line[i];
        if (inQ) {
            if (ch == '"') {
                if (i + 1 < line.size() && line[i + 1] == '"') { cur += '"'; i++; }
                else inQ = false;
            } else cur += ch;
        } else {
            if (ch == '"') inQ = true;
            else if (ch == ',') { out.push_back(cur); cur.clear(); }
            else cur += ch;
        }
    }
    out.push_back(cur);
}

static std::wstring CsvDecode(const std::string& s) {
    if (!s.empty() && (uint8_t)s[0] == 0xEF && s.size() > 2 &&
        (uint8_t)s[1] == 0xBB && (uint8_t)s[2] == 0xBF)
        return U8W(s.substr(3));
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), nullptr, 0) > 0)
        return U8W(s);
    int n = MultiByteToWideChar(936, 0, s.data(), (int)s.size(), nullptr, 0);   // GBK 回退
    if (n <= 0) return U8W(s);
    std::wstring w(n, 0);
    MultiByteToWideChar(936, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

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
static std::wstring LowerW(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

bool ImportSpreadsheet(const std::wstring& path, std::vector<XRow>& out, std::wstring& err) {
    out.clear();
    std::wstring lower = LowerW(path);
    size_t dot = lower.find_last_of(L'.');
    std::wstring ext = (dot == std::wstring::npos) ? L"" : lower.substr(dot);

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
        std::string content(data.begin(), data.end());
        ParseCsv(content, rows);
    } else {
        std::vector<ZipEntry> entries;
        if (!ReadZipEntries(data, entries)) { err = L"不是有效的 .xlsx 文件(旧版 .xls 请先用 Excel 另存为 .xlsx 或 .csv)"; return false; }
        std::string sstXml, sheetXml;
        for (auto& e : entries)
            if (e.name == "xl/sharedStrings.xml") ExtractEntry(data, e, sstXml);
        const ZipEntry* sheet = nullptr;
        for (auto& e : entries)
            if (e.name == "xl/worksheets/sheet1.xml") { sheet = &e; break; }
        if (!sheet)
            for (auto& e : entries) {
                if (e.name.compare(0, 15, "xl/worksheets/") == 0 &&
                    e.name.size() > 15 && e.name.substr(e.name.size() - 4) == ".xml") {
                    sheet = &e; break;
                }
            }
        if (!sheet) { err = L"未找到工作表"; return false; }
        if (!ExtractEntry(data, *sheet, sheetXml)) { err = L"解压工作表失败"; return false; }
        std::vector<std::string> sst;
        ParseSharedStrings(sstXml, sst);
        ParseSheet(sheetXml, sst, rows);
    }

    // 跳过表头行 / 空白行
    std::vector<XRow> cleaned;
    bool first = true;
    auto isHeader = [](const std::wstring& v) {
        std::wstring s = LowerW(v);
        return s == L"学号" || s == L"序号" || s == L"编号" || s == L"id" || s == L"no" ||
               s == L"no." || s == L"number" || s == L"姓名" || s == L"名字" || s == L"name";
    };
    for (auto& r : rows) {
        if (first) {
            first = false;
            if (isHeader(r.id) || isHeader(r.name)) continue;
        }
        if (r.id.empty() && r.name.empty()) continue;
        cleaned.push_back(r);
    }
    if (cleaned.empty()) { err = L"未解析到学生数据(请确认前两列为: 学号 / 姓名)"; return false; }
    out = std::move(cleaned);
    return true;
}
