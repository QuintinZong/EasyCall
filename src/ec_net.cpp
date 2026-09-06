// EasyCall - 网络/HTTP 实现
#include "ec_common.h"
#include <winhttp.h>
#include <algorithm>
#include <cstring>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winhttp.lib")

// ================= 网络初始化 =================
bool EcNetStart() { WSADATA d; return WSAStartup(MAKEWORD(2, 2), &d) == 0; }
void EcNetStop()  { WSACleanup(); }

// ================= 编码 =================
std::wstring U8W(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
std::string WU8(const std::wstring& s) {
    if (s.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string a(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), &a[0], n, nullptr, nullptr);
    return a;
}

// ================= 路径 / 配置 =================
std::wstring ExeDirW() {
    wchar_t buf[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p(buf);
    size_t k = p.find_last_of(L"\\/");
    if (k != std::wstring::npos) p = p.substr(0, k + 1);
    return p;
}
std::wstring IniFileW() { return ExeDirW() + L"easycall.ini"; }

std::wstring IniGet(const wchar_t* sec, const wchar_t* key, const std::wstring& def) {
    wchar_t buf[2048] = {0};
    DWORD n = GetPrivateProfileStringW(sec, key, def.c_str(), buf, 2048, IniFileW().c_str());
    return std::wstring(buf, n);
}
void IniSet(const wchar_t* sec, const wchar_t* key, const std::wstring& val) {
    WritePrivateProfileStringW(sec, key, val.c_str(), IniFileW().c_str());
}

std::wstring NowTimeW() {
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t buf[32];
    swprintf(buf, 32, L"%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);
    return buf;
}
std::string NowStampMs() {
    ULONGLONG t = GetTickCount64();
    return std::to_string(t);
}

// ================= TCP 帧 =================
static bool SendAll(SOCKET s, const char* buf, int n) {
    while (n > 0) {
        int r = send(s, buf, n, 0);
        if (r == SOCKET_ERROR || r == 0) return false;
        buf += r; n -= r;
    }
    return true;
}
static bool RecvAll(SOCKET s, char* buf, int n, int timeoutMs) {
    int done = 0;
    while (done < n) {
        fd_set fds; FD_ZERO(&fds); FD_SET(s, &fds);
        timeval tv; tv.tv_sec = timeoutMs / 1000; tv.tv_usec = (timeoutMs % 1000) * 1000;
        int r = select(0, &fds, nullptr, nullptr, &tv);
        if (r <= 0) return false;
        int got = recv(s, buf + done, n - done, 0);
        if (got <= 0) return false;
        done += got;
    }
    return true;
}
bool TcpSendFrame(SOCKET s, const std::string& p) {
    if (p.empty() || p.size() > EC_FRAME_MAX) return false;
    uint32_t len = (uint32_t)p.size();
    uint32_t net = htonl(len);
    if (!SendAll(s, (const char*)&net, 4)) return false;
    return SendAll(s, p.data(), (int)p.size());
}
std::string TcpRecvFrame(SOCKET s, int timeoutMs, bool* ok) {
    *ok = false;
    uint32_t net = 0;
    if (!RecvAll(s, (char*)&net, 4, timeoutMs)) return std::string();
    uint32_t len = ntohl(net);
    if (len == 0 || len > EC_FRAME_MAX) return std::string();
    std::string p(len, 0);
    if (!RecvAll(s, &p[0], (int)len, timeoutMs)) return std::string();
    *ok = true;
    return p;
}

std::vector<std::string> SplitLines(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == '\n') { if (!cur.empty() || !out.empty()) out.push_back(cur); cur.clear(); }
        else if (c != '\r') cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}
std::vector<std::string> SplitTabs(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == '\t') { out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    out.push_back(cur);
    return out;
}

std::string BuildCallPayload(const std::wstring& place, const std::wstring& teacher,
                             const std::vector<std::wstring>& items, std::string* callIdOut) {
    std::string callId = NowStampMs();
    std::string out = "CALL\n" + callId + "\n" + WU8(place) + "\n" + WU8(teacher) + "\n";
    for (auto& it : items) out += WU8(it) + "\n";
    if (callIdOut) *callIdOut = callId;
    return out;
}

int ParseFrames(const std::string& body, std::vector<std::string>& out) {
    out.clear();
    size_t pos = 0;
    while (pos + 4 <= body.size()) {
        // 服务器(pack('N') / writeUInt32BE)使用大端长度前缀
        uint32_t len = ((uint32_t)(uint8_t)body[pos] << 24) |
                       ((uint32_t)(uint8_t)body[pos + 1] << 16) |
                       ((uint32_t)(uint8_t)body[pos + 2] << 8) |
                       (uint32_t)(uint8_t)body[pos + 3];
        pos += 4;
        if (len == 0 || len > EC_FRAME_MAX || pos + len > body.size()) break;   // 尾部异常则停止
        out.push_back(body.substr(pos, len));
        pos += len;
    }
    return (int)out.size();
}

long long HttpSeqFromHeader(const std::string& hdr) {
    std::string h = hdr;
    for (auto& c : h) c = (char)tolower((unsigned char)c);
    size_t p = h.find("x-easycall-seq:");
    if (p == std::string::npos) return 0;
    p += 15;
    while (p < h.size() && h[p] == ' ') p++;
    long long v = 0;
    while (p < h.size() && isdigit((unsigned char)h[p])) { v = v * 10 + (h[p] - '0'); p++; }
    return v;
}

// ================= UDP 发现 =================
int DiscoverBoards(std::vector<BoardInfo>& out, int waitMs) {
    SOCKET sd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sd == INVALID_SOCKET) return 0;
    BOOL b = TRUE;
    setsockopt(sd, SOL_SOCKET, SO_REUSEADDR, (const char*)&b, sizeof b);
    sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(EC_UDP_DISC_PORT);
    sa.sin_addr.s_addr = INADDR_ANY;
    if (bind(sd, (sockaddr*)&sa, sizeof sa) != 0) { closesocket(sd); return 0; }
    ULONGLONG deadline = GetTickCount64() + (ULONGLONG)waitMs;
    char buf[2048];
    while ((LONGLONG)(GetTickCount64() - deadline) < 0) {
        fd_set fds; FD_ZERO(&fds); FD_SET(sd, &fds);
        timeval tv; tv.tv_sec = 0; tv.tv_usec = 200000;
        int r = select(0, &fds, nullptr, nullptr, &tv);
        if (r <= 0) continue;
        sockaddr_in from; int fl = sizeof from;
        int n = recvfrom(sd, buf, (int)sizeof buf - 1, 0, (sockaddr*)&from, &fl);
        if (n <= 0) continue;
        buf[n] = 0;
        std::vector<std::string> toks = SplitTabs(std::string(buf, n));
        if (toks.size() < 3 || toks[0] != "EASYCALL_DISC") continue;
        int port = atoi(toks[1].c_str());
        if (port <= 0 || port > 65535) continue;
        std::wstring name = U8W(toks[2]);
        std::wstring ip = U8W(inet_ntoa(from.sin_addr));
        bool dup = false;
        for (auto& e : out) if (e.ip == ip && e.port == (unsigned short)port) { dup = true; break; }
        if (!dup) out.push_back({ name, ip, (unsigned short)port });
    }
    closesocket(sd);
    return (int)out.size();
}

void UdpBroadcastPresence(const std::wstring& name, unsigned short port) {
    SOCKET sd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sd == INVALID_SOCKET) return;
    BOOL b = TRUE;
    setsockopt(sd, SOL_SOCKET, SO_BROADCAST, (const char*)&b, sizeof b);
    std::string msg = "EASYCALL_DISC\t" + std::to_string(port) + "\t" + WU8(name);
    sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(EC_UDP_DISC_PORT);
    sa.sin_addr.s_addr = INADDR_BROADCAST;
    sendto(sd, msg.data(), (int)msg.size(), 0, (sockaddr*)&sa, sizeof sa);
    closesocket(sd);
}

std::vector<std::wstring> LocalIPv4s() {
    std::vector<std::wstring> out;
    char host[256] = {0};
    if (gethostname(host, sizeof host) == 0) {
        addrinfo hints; memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_INET;
        addrinfo* res = nullptr;
        if (getaddrinfo(host, nullptr, &hints, &res) == 0) {
            for (addrinfo* p = res; p; p = p->ai_next) {
                char ip[64] = {0};
                if (!inet_ntop(AF_INET, &((sockaddr_in*)p->ai_addr)->sin_addr, ip, sizeof ip)) continue;
                std::string s(ip);
                if (s.rfind("127.", 0) == 0) continue;
                bool dup = false;
                for (auto& e : out) if (e == U8W(s)) { dup = true; break; }
                if (!dup) out.push_back(U8W(s));
            }
            freeaddrinfo(res);
        }
    }
    if (out.empty()) {
        SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s != INVALID_SOCKET) {
            sockaddr_in sa; memset(&sa, 0, sizeof sa);
            sa.sin_family = AF_INET; sa.sin_port = htons(53);
            inet_pton(AF_INET, "8.8.8.8", &sa.sin_addr);
            if (connect(s, (sockaddr*)&sa, sizeof sa) == 0) {
                sockaddr_in local; int l = sizeof local;
                if (getsockname(s, (sockaddr*)&local, &l) == 0) {
                    char ip[64] = {0};
                    inet_ntop(AF_INET, &local.sin_addr, ip, sizeof ip);
                    if (std::string(ip).rfind("127.", 0) != 0) out.push_back(U8W(ip));
                }
            }
            closesocket(s);
        }
    }
    return out;
}

// ================= HTTP (WinHTTP) =================
static HINTERNET g_httpSession = nullptr;
static HINTERNET g_curRequest = nullptr;   // 当前进行中的请求句柄(供中断)

void HttpAbortCurrent() {
    HINTERNET h = (HINTERNET)InterlockedExchangePointer((PVOID volatile*)&g_curRequest, nullptr);
    if (h) WinHttpCloseHandle(h);
}
static bool EnsureSession() {
    if (!g_httpSession)
        g_httpSession = WinHttpOpen(L"EasyCall/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    return g_httpSession != nullptr;
}

struct UrlParts { std::wstring host, path; INTERNET_PORT port; bool secure; };
static bool ParseUrl(const std::wstring& url, UrlParts& p) {
    size_t pos = url.find(L"://");
    if (pos == std::wstring::npos) return false;
    std::wstring scheme = url.substr(0, pos);
    p.secure = (scheme == L"https");
    std::wstring rest = url.substr(pos + 3);
    size_t slash = rest.find(L'/');
    std::wstring hostport = (slash == std::wstring::npos) ? rest : rest.substr(0, slash);
    p.path = (slash == std::wstring::npos) ? L"/" : rest.substr(slash);
    p.port = p.secure ? 443 : 80;
    size_t colon = hostport.rfind(L':');
    if (colon != std::wstring::npos && hostport.find(L']') == std::wstring::npos) {
        p.host = hostport.substr(0, colon);
        int prt = _wtoi(hostport.substr(colon + 1).c_str());
        if (prt > 0 && prt < 65536) p.port = (INTERNET_PORT)prt;
        else p.host = hostport;
    } else {
        p.host = hostport;
    }
    if (p.host.empty()) return false;
    return true;
}

static bool HttpRequest(const wchar_t* verb, const std::wstring& url, const std::string& bodyUtf8,
                        std::string& respUtf8, std::wstring& err, int timeoutSec, std::string* respHdr) {
    respUtf8.clear(); err.clear();
    if (!EnsureSession()) { err = L"WinHTTP 初始化失败"; return false; }
    UrlParts up;
    if (!ParseUrl(url, up)) { err = L"地址格式错误(需以 http:// 或 https:// 开头)"; return false; }
    HINTERNET c = WinHttpConnect(g_httpSession, up.host.c_str(), up.port, 0);
    if (!c) { err = L"无法连接服务器 " + up.host; return false; }
    HINTERNET r = WinHttpOpenRequest(c, verb, up.path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES, up.secure ? WINHTTP_FLAG_SECURE : 0);
    if (!r) { WinHttpCloseHandle(c); err = L"创建请求失败"; return false; }
    InterlockedExchangePointer((PVOID volatile*)&g_curRequest, r);
    WinHttpSetTimeouts(r, 8000, 8000, timeoutSec * 1000, timeoutSec * 1000);
    const wchar_t* hdrs = WINHTTP_NO_ADDITIONAL_HEADERS;
    std::wstring h;
    if (!bodyUtf8.empty()) { h = L"Content-Type: application/x-www-form-urlencoded\r\n"; hdrs = h.c_str(); }
    BOOL ok = WinHttpSendRequest(r, hdrs, (DWORD)-1, (LPVOID)bodyUtf8.data(), (DWORD)bodyUtf8.size(),
                                 (DWORD)bodyUtf8.size(), 0);
    if (ok) ok = WinHttpReceiveResponse(r, nullptr);
    if (ok && respHdr) {
        DWORD sz = 0;
        WinHttpQueryHeaders(r, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
                            nullptr, &sz, WINHTTP_NO_HEADER_INDEX);
        if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && sz > 0) {
            std::vector<wchar_t> buf(sz / 2 + 2);
            if (WinHttpQueryHeaders(r, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
                                    buf.data(), &sz, WINHTTP_NO_HEADER_INDEX))
                *respHdr = WU8(buf.data());
        }
    }
    if (ok) {
        DWORD avail = 0;
        std::string body;
        while (WinHttpQueryDataAvailable(r, &avail) && avail > 0) {
            std::vector<char> chunk(avail + 1);
            DWORD read = 0;
            if (!WinHttpReadData(r, chunk.data(), avail, &read) || read == 0) break;
            body.append(chunk.data(), read);
            if (body.size() > EC_FRAME_MAX) { ok = FALSE; break; }
        }
        respUtf8 = std::move(body);
    }
    if (!ok) {
        DWORD e = GetLastError();
        wchar_t msg[256];
        swprintf(msg, 256, L"网络请求失败(错误 %lu)", e);
        err = msg;
    }
    InterlockedCompareExchangePointer((PVOID volatile*)&g_curRequest, nullptr, r);
    WinHttpCloseHandle(r);
    WinHttpCloseHandle(c);
    return ok != FALSE;
}

bool HttpPostForm(const std::wstring& url, const std::string& bodyUtf8,
                  std::string& respUtf8, std::wstring& err, int timeoutSec, std::string* respHdr) {
    return HttpRequest(L"POST", url, bodyUtf8, respUtf8, err, timeoutSec, respHdr);
}
bool HttpGet(const std::wstring& url, std::string& respUtf8, std::wstring& err,
             int timeoutSec, std::string* respHdr) {
    return HttpRequest(L"GET", url, std::string(), respUtf8, err, timeoutSec, respHdr);
}

std::string UrlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

// ================= 迷你 JSON =================
std::string JsonEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"':  out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:   out += c; break;
        }
    }
    return out;
}
std::string JsonStrVal(const std::string& obj, const char* key) {
    std::string pat = std::string("\"") + key + "\":\"";
    size_t p = obj.find(pat);
    if (p == std::string::npos) return std::string();
    p += pat.size();
    std::string out;
    while (p < obj.size() && obj[p] != '"') {
        if (obj[p] == '\\' && p + 1 < obj.size()) {
            char n = obj[p + 1];
            if (n == '"') out += '"';
            else if (n == '\\') out += '\\';
            else if (n == '/') out += '/';
            else if (n == 'n') out += '\n';
            else if (n == 'r') out += '\r';
            else if (n == 't') out += '\t';
            else if (n == 'u' && p + 5 < obj.size()) {
                unsigned v = 0;
                bool bad = false;
                for (int i = 0; i < 4; i++) {
                    char hc = obj[p + 2 + i];
                    v <<= 4;
                    if (hc >= '0' && hc <= '9') v |= (hc - '0');
                    else if (hc >= 'a' && hc <= 'f') v |= (hc - 'a' + 10);
                    else if (hc >= 'A' && hc <= 'F') v |= (hc - 'A' + 10);
                    else { bad = true; break; }
                }
                if (!bad) { wchar_t wc = (wchar_t)v; out += WU8(std::wstring(1, wc)); p += 4; }
                else { out += n; }
            }
            else { out += n; }
            p += 2;
        } else {
            out += obj[p];
            p++;
        }
    }
    return out;
}

// ================= 自截图 PNG 编码 (无压缩 deflate 存储块) =================
static uint32_t PngCrcUpdate(uint32_t crc, const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & -(int32_t)(crc & 1));
    }
    return crc;
}
static void WrBE(std::string& out, uint32_t v) {
    out += (char)(v >> 24);
    out += (char)(v >> 16);
    out += (char)(v >> 8);
    out += (char)v;
}
static void PngChunk(std::string& out, const char type[4], const std::string& data) {
    WrBE(out, (uint32_t)data.size());
    uint32_t crc = 0xFFFFFFFFu;
    crc = PngCrcUpdate(crc, (const uint8_t*)type, 4);
    crc = PngCrcUpdate(crc, (const uint8_t*)data.data(), data.size());
    crc ^= 0xFFFFFFFFu;
    out.append(type, 4);
    out.append(data);
    WrBE(out, crc);
}
bool SavePng32(const std::wstring& path, const uint8_t* bgra, int w, int h) {
    if (!bgra || w <= 0 || h <= 0) return false;
    std::string png;
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    png.append((const char*)sig, 8);
    std::string ihdr;
    WrBE(ihdr, (uint32_t)w);
    WrBE(ihdr, (uint32_t)h);
    ihdr += (char)8;   // bit depth
    ihdr += (char)6;   // RGBA
    ihdr += (char)0;   // compression
    ihdr += (char)0;   // filter
    ihdr += (char)0;   // interlace
    PngChunk(png, "IHDR", ihdr);

    // 原始数据: 每行 = 1 字节 filter(0) + 像素(BGRA, alpha 强制 255)
    std::string raw;
    raw.reserve((size_t)h * ((size_t)w * 4 + 1));
    std::string row((size_t)w * 4 + 1, 0);
    for (int y = 0; y < h; y++) {
        row[0] = 0;
        const uint8_t* src = bgra + (size_t)y * w * 4;
        for (int x = 0; x < w; x++) {
            // DIB 内存为 BGRA, PNG 需要 RGBA
            row[1 + x * 4 + 0] = src[x * 4 + 2];
            row[1 + x * 4 + 1] = src[x * 4 + 1];
            row[1 + x * 4 + 2] = src[x * 4 + 0];
            row[1 + x * 4 + 3] = 255;
        }
        raw.append(row.data(), row.size());
    }
    // zlib 流: 无压缩存储块 + Adler32
    std::string idat;
    idat += (char)0x78;
    idat += (char)0x01;
    size_t pos = 0;
    while (pos < raw.size()) {
        size_t n = raw.size() - pos;
        if (n > 65535) n = 65535;
        bool last = (pos + n >= raw.size());
        idat += (char)(last ? 1 : 0);
        idat += (char)(n & 0xFF);
        idat += (char)((n >> 8) & 0xFF);
        idat += (char)((~n) & 0xFF);
        idat += (char)(((~n) >> 8) & 0xFF);
        idat.append(raw.data() + pos, n);
        pos += n;
    }
    uint32_t a = 1, b = 0;
    for (char c : raw) {
        a = (a + (uint8_t)c) % 65521u;
        b = (b + a) % 65521u;
    }
    WrBE(idat, (b << 16) | a);
    PngChunk(png, "IDAT", idat);
    PngChunk(png, "IEND", std::string());

    HANDLE hf = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hf == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0;
    bool ok = WriteFile(hf, png.data(), (DWORD)png.size(), &wr, nullptr) && wr == png.size();
    CloseHandle(hf);
    return ok;
}
