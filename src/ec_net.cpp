// EasyCall - 网络/HTTP 实现
#include "ec_common.h"
#include <winhttp.h>
#include <algorithm>
#include <cstring>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winhttp.lib")

// ================= 网络初始化 =================
// 功能: 初始化 WinSock 2.2, 任何网络操作前必须调用一次
// 参数: 无
// 返回: true=初始化成功; false=WSAStartup 失败
bool EcNetStart() { WSADATA d; return WSAStartup(MAKEWORD(2, 2), &d) == 0; }
// 功能: 清理 WinSock 资源, 程序退出前调用(与 EcNetStart 配对)
// 参数: 无
// 返回: 无
void EcNetStop()  { WSACleanup(); }

// ================= 编码 =================
// 功能: UTF-8 窄字符串 -> UTF-16 宽字符串
// 参数: s 待转换的 UTF-8 字符串
// 返回: 转换后的宽字符串; 输入为空或转换失败时返回空串
std::wstring U8W(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
// 功能: UTF-16 宽字符串 -> UTF-8 窄字符串(网络传输统一用 UTF-8)
// 参数: s 待转换的宽字符串
// 返回: 转换后的 UTF-8 字符串; 输入为空或转换失败时返回空串
std::string WU8(const std::wstring& s) {
    if (s.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string a(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), &a[0], n, nullptr, nullptr);
    return a;
}

// ================= 路径 / 配置 =================
// 功能: 取当前 EXE 所在目录(结尾带反斜杠), 学生名单/对话记录/INI 都放在该目录
// 参数: 无
// 返回: EXE 所在目录的绝对路径, 如 "C:\Tools\EasyCall\"
std::wstring ExeDirW() {
    wchar_t buf[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p(buf);
    size_t k = p.find_last_of(L"\\/");
    if (k != std::wstring::npos) p = p.substr(0, k + 1);
    return p;
}
// 功能: 得到 INI 配置文件完整路径 (EXE目录\easycall.ini)
// 参数: 无
// 返回: INI 文件的完整路径字符串
std::wstring IniFileW() { return ExeDirW() + L"easycall.ini"; }

// 功能: 从 INI 文件读取一个字符串配置项
// 参数: sec 节名(如 L"net"); key 键名; def 读取失败/不存在时使用的默认值
// 返回: 配置值; 未找到时返回 def
std::wstring IniGet(const wchar_t* sec, const wchar_t* key, const std::wstring& def) {
    wchar_t buf[2048] = {0};
    DWORD n = GetPrivateProfileStringW(sec, key, def.c_str(), buf, 2048, IniFileW().c_str());
    return std::wstring(buf, n);
}
// 功能: 向 INI 文件写入一个字符串配置项
// 参数: sec 节名; key 键名; val 要保存的值
// 返回: 无
void IniSet(const wchar_t* sec, const wchar_t* key, const std::wstring& val) {
    WritePrivateProfileStringW(sec, key, val.c_str(), IniFileW().c_str());
}

// 功能: 取当前本地时间, 格式化为 "HH:MM:SS"(用于历史记录/对话前缀)
// 参数: 无
// 返回: 形如 "14:03:27" 的宽字符串
std::wstring NowTimeW() {
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t buf[32];
    swprintf(buf, 32, L"%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);
    return buf;
}
// 功能: 取系统启动以来的毫秒数(GetTickCount64), 转为十进制字符串
// 参数: 无
// 返回: 毫秒时间戳字符串; 用作叫号 ID / 聊天消息 ID(足够唯一且可用作排序序号)
std::string NowStampMs() {
    ULONGLONG t = GetTickCount64();
    return std::to_string(t);
}

// ================= TCP 帧 =================
// ------------------------------------------------------------------
// 帧协议说明(教师端 <-> 大屏端 局域网直连时使用):
//   每个帧 = [4字节 长度前缀][UTF-8 负载]
//   长度前缀为网络字节序(大端, htonl 转换), 值 = 负载字节数, 不含前缀本身;
//   负载最大 EC_FRAME_MAX 字节。收发两端都按此格式封帧/拆帧。
// ------------------------------------------------------------------
// 功能: 循环发送直到 n 字节全部发出(处理 send 返回部分发送的情况)
// 参数: s 已连接套接字; buf 待发送数据指针; n 待发送字节数
// 返回: true=全部发出; false=发送出错或连接断开
static bool SendAll(SOCKET s, const char* buf, int n) {
    while (n > 0) {
        int r = send(s, buf, n, 0);
        if (r == SOCKET_ERROR || r == 0) return false;
        buf += r; n -= r;
    }
    return true;
}
// 功能: 循环接收直到凑满 n 字节(处理 recv 返回部分数据的情况), 带超时
// 参数: s 套接字; buf 接收缓冲区; n 需要接收的字节数; timeoutMs 每次 select 等待超时(毫秒)
// 返回: true=收满 n 字节; false=超时/出错/连接断开
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
// 功能: 按帧协议发送一帧: 先发4字节大端长度前缀, 再发 UTF-8 负载
// 参数: s 套接字; p 负载内容(UTF-8)
// 返回: true=发送成功; false=负载为空/超长或发送失败
bool TcpSendFrame(SOCKET s, const std::string& p) {
    if (p.empty() || p.size() > EC_FRAME_MAX) return false;
    uint32_t len = (uint32_t)p.size();
    uint32_t net = htonl(len);                    // 转成网络字节序(大端)
    if (!SendAll(s, (const char*)&net, 4)) return false;
    return SendAll(s, p.data(), (int)p.size());
}
// 功能: 按帧协议接收一帧: 先收4字节长度前缀, 校验后收负载
// 参数: s 套接字; timeoutMs 接收超时(毫秒); ok [出参] true=成功收到完整帧, false=失败/超时/长度非法
// 返回: 收到的负载(UTF-8); 失败时返回空串
std::string TcpRecvFrame(SOCKET s, int timeoutMs, bool* ok) {
    *ok = false;
    uint32_t net = 0;
    if (!RecvAll(s, (char*)&net, 4, timeoutMs)) return std::string();
    uint32_t len = ntohl(net);                    // 大端 -> 本机字节序
    if (len == 0 || len > EC_FRAME_MAX) return std::string();
    std::string p(len, 0);
    if (!RecvAll(s, &p[0], (int)len, timeoutMs)) return std::string();
    *ok = true;
    return p;
}

// 功能: 按 '\n' 拆分文本行, 忽略 '\r'; 保留空行(除尾部)
// 参数: s 待拆分的文本(通常是一个报文负载)
// 返回: 拆分后的行数组
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
// 功能: 按 '\t' 拆分字段(学生行 "学号\t姓名\t班级" 的解析)
// 参数: s 待拆分字符串
// 返回: 拆分后的字段数组(至少1个元素)
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

// ------------------------------------------------------------------
// CALL 报文格式(叫号, 由教师端构造、大屏端解析):
//   CALL\n
//   <callId>\n          -- 叫号ID(毫秒时间戳字符串), 大屏端用它去重
//   <place>\n           -- 集合地点(如 "台前")
//   <teacher>\n         -- 教师姓名
//   <学生行>\n          -- 每个学生占一行, 行内为 "学号\t姓名\t班级"
//   <学生行>\n          -- 可多行(同时叫多人)
// 其他简短指令(负载即整个报文): CLEAR(清屏) / BLACK(黑屏) / PING / PONG
// ------------------------------------------------------------------
// 功能: 构造 CALL 叫号报文
// 参数: place 集合地点; teacher 教师姓名; items 学生行数组(每项为 "学号\t姓名\t班级" 的宽字符串);
//       callIdOut [出参, 可空] 生成的叫号ID(毫秒时间戳)
// 返回: 完整 CALL 报文(UTF-8)
std::string BuildCallPayload(const std::wstring& place, const std::wstring& teacher,
                             const std::vector<std::wstring>& items, std::string* callIdOut) {
    std::string callId = NowStampMs();
    std::string out = "CALL\n" + callId + "\n" + WU8(place) + "\n" + WU8(teacher) + "\n";
    for (auto& it : items) out += WU8(it) + "\n";
    if (callIdOut) *callIdOut = callId;
    return out;
}

// 功能: 从服务器响应体中解析出多个帧(服务器 fetch.php 返回的批量数据)
// 帧格式: [4字节大端长度前缀][负载] 依次拼接, 与 TcpSendFrame 一致
// 参数: body 响应体; out [出参] 解析出的帧负载数组(先清空)
// 返回: 解析出的帧个数; 遇到非法长度前缀即停止
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

// 功能: 从 HTTP 响应头中解析 "X-EasyCall-Seq: <数字>" 序号
// 参数: hdr 原始响应头文本
// 返回: 解析出的序号(服务器已写入的消息总数); 头不存在或非数字时返回 0
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
// 功能: 在局域网内发现班级大屏端(UDP 广播发现协议)
// 协议: 大屏端每隔2秒向 255.255.255.255:EC_UDP_DISC_PORT 广播
//       "EASYCALL_DISC\t<监听端口>\t<大屏名称>"; 本函数监听同一端口收集
// 参数: out [出参] 发现的大屏列表(去重); waitMs 收集窗口时长(毫秒)
// 返回: 发现的大屏台数
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
    ULONGLONG deadline = GetTickCount64() + (ULONGLONG)waitMs;   // 收集截止时刻
    char buf[2048];
    while ((LONGLONG)(GetTickCount64() - deadline) < 0) {
        fd_set fds; FD_ZERO(&fds); FD_SET(sd, &fds);
        timeval tv; tv.tv_sec = 0; tv.tv_usec = 200000;           // 每 200ms 轮询一次
        int r = select(0, &fds, nullptr, nullptr, &tv);
        if (r <= 0) continue;
        sockaddr_in from; int fl = sizeof from;
        int n = recvfrom(sd, buf, (int)sizeof buf - 1, 0, (sockaddr*)&from, &fl);
        if (n <= 0) continue;
        buf[n] = 0;
        std::vector<std::string> toks = SplitTabs(std::string(buf, n));
        if (toks.size() < 3 || toks[0] != "EASYCALL_DISC") continue;   // 非本协议报文, 忽略
        int port = atoi(toks[1].c_str());
        if (port <= 0 || port > 65535) continue;
        std::wstring name = U8W(toks[2]);
        std::wstring ip = U8W(inet_ntoa(from.sin_addr));
        bool dup = false;
        for (auto& e : out) if (e.ip == ip && e.port == (unsigned short)port) { dup = true; break; }
        if (!dup) out.push_back({ name, ip, (unsigned short)port });   // 同一台去重
    }
    closesocket(sd);
    return (int)out.size();
}

// 功能: 向局域网广播一次大屏在线宣告(大屏端每2秒调用一次)
// 参数: name 大屏显示名称; port 大屏 TCP 监听端口(教师端收到后据此直连)
// 返回: 无
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
    sa.sin_addr.s_addr = INADDR_BROADCAST;   // 255.255.255.255 全局广播
    sendto(sd, msg.data(), (int)msg.size(), 0, (sockaddr*)&sa, sizeof sa);
    closesocket(sd);
}

// 功能: 枚举本机所有 IPv4 地址(大屏端用于提示教师"本机IP: x.x.x.x")
// 参数: 无
// 返回: 本机 IPv4 地址列表(过滤 127.x 回环地址, 去重)
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
                if (s.rfind("127.", 0) == 0) continue;   // 跳过回环地址
                bool dup = false;
                for (auto& e : out) if (e == U8W(s)) { dup = true; break; }
                if (!dup) out.push_back(U8W(s));
            }
            freeaddrinfo(res);
        }
    }
    if (out.empty()) {
        // 兜底: 用一个不实际发包的 UDP "连接" 让系统选出本机出网地址
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
// 全局 WinHTTP 会话句柄(复用, 进程内只建一次)
static HINTERNET g_httpSession = nullptr;
static HINTERNET g_curRequest = nullptr;   // 当前进行中的请求句柄(供中断)

// 功能: 立即中止当前进行中的 HTTP 请求(程序退出时用, 让阻塞的线程尽快返回)
// 参数: 无
// 返回: 无
void HttpAbortCurrent() {
    HINTERNET h = (HINTERNET)InterlockedExchangePointer((PVOID volatile*)&g_curRequest, nullptr);
    if (h) WinHttpCloseHandle(h);
}
// 功能: 惰性创建全局 WinHTTP 会话
// 参数: 无
// 返回: true=会话可用; false=创建失败
static bool EnsureSession() {
    if (!g_httpSession)
        g_httpSession = WinHttpOpen(L"EasyCall/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    return g_httpSession != nullptr;
}

// URL 各组成部分(解析结果)
struct UrlParts { std::wstring host, path; INTERNET_PORT port; bool secure; };
// 功能: 解析 URL, 拆分出 主机/路径/端口/是否HTTPS
// 参数: url 形如 "http://IP:端口/xxx" 或 "https://域名/xxx" 的完整地址; p [出参] 解析结果
// 返回: true=解析成功; false=缺少 "://" 或主机为空
static bool ParseUrl(const std::wstring& url, UrlParts& p) {
    size_t pos = url.find(L"://");
    if (pos == std::wstring::npos) return false;
    std::wstring scheme = url.substr(0, pos);
    p.secure = (scheme == L"https");
    std::wstring rest = url.substr(pos + 3);
    size_t slash = rest.find(L'/');
    std::wstring hostport = (slash == std::wstring::npos) ? rest : rest.substr(0, slash);
    p.path = (slash == std::wstring::npos) ? L"/" : rest.substr(slash);
    p.port = p.secure ? 443 : 80;                       // 默认端口
    size_t colon = hostport.rfind(L':');
    if (colon != std::wstring::npos && hostport.find(L']') == std::wstring::npos) {
        p.host = hostport.substr(0, colon);
        int prt = _wtoi(hostport.substr(colon + 1).c_str());
        if (prt > 0 && prt < 65536) p.port = (INTERNET_PORT)prt;
        else p.host = hostport;                          // 冒号后不是合法端口, 视为主机名一部分
    } else {
        p.host = hostport;
    }
    if (p.host.empty()) return false;
    return true;
}

// 功能: 执行一次 HTTP 请求(内部核心; 对外由 HttpGet/HttpPostForm 包装)
// 参数: verb HTTP 方法(L"GET"/L"POST"); url 完整地址; bodyUtf8 请求体(UTF-8, GET 时为空);
//       respUtf8 [出参] 响应体(UTF-8); err [出参] 失败原因(中文, 可直接显示);
//       timeoutSec 收发总超时(秒); respHdr [出参, 可空] 原始响应头
// 返回: true=请求成功(无论HTTP状态码); false=失败, 原因见 err
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
    InterlockedExchangePointer((PVOID volatile*)&g_curRequest, r);   // 登记当前请求, 供 HttpAbortCurrent 中断
    WinHttpSetTimeouts(r, 8000, 8000, timeoutSec * 1000, timeoutSec * 1000);
    const wchar_t* hdrs = WINHTTP_NO_ADDITIONAL_HEADERS;
    std::wstring h;
    if (!bodyUtf8.empty()) { h = L"Content-Type: application/x-www-form-urlencoded\r\n"; hdrs = h.c_str(); }
    BOOL ok = WinHttpSendRequest(r, hdrs, (DWORD)-1, (LPVOID)bodyUtf8.data(), (DWORD)bodyUtf8.size(),
                                 (DWORD)bodyUtf8.size(), 0);
    if (ok) ok = WinHttpReceiveResponse(r, nullptr);
    if (ok && respHdr) {
        // 先查询原始响应头所需缓冲区大小, 再实际读取
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
        // 循环读取响应体, 直到没有更多数据
        DWORD avail = 0;
        std::string body;
        while (WinHttpQueryDataAvailable(r, &avail) && avail > 0) {
            std::vector<char> chunk(avail + 1);
            DWORD read = 0;
            if (!WinHttpReadData(r, chunk.data(), avail, &read) || read == 0) break;
            body.append(chunk.data(), read);
            if (body.size() > EC_FRAME_MAX) { ok = FALSE; break; }   // 响应过大, 视为异常
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

// 功能: 发送 HTTP POST 请求(表单格式), 用于向中转服务器 push.php 推数据
// 参数: url 完整地址; bodyUtf8 表单体(UTF-8, 如 "room=101&data=CALL...");
//       respUtf8 [出参] 响应体; err [出参] 失败原因; timeoutSec 超时(秒);
//       respHdr [出参, 可空] 原始响应头
// 返回: true=成功; false=失败
bool HttpPostForm(const std::wstring& url, const std::string& bodyUtf8,
                  std::string& respUtf8, std::wstring& err, int timeoutSec, std::string* respHdr) {
    return HttpRequest(L"POST", url, bodyUtf8, respUtf8, err, timeoutSec, respHdr);
}
// 功能: 发送 HTTP GET 请求, 用于 fetch.php 轮询消息 / presence.php 查在线状态
// 参数: url 完整地址(含查询串); respUtf8 [出参] 响应体; err [出参] 失败原因;
//       timeoutSec 超时(秒); respHdr [出参, 可空] 原始响应头(取 X-EasyCall-Seq 用)
// 返回: true=成功; false=失败
bool HttpGet(const std::wstring& url, std::string& respUtf8, std::wstring& err,
             int timeoutSec, std::string* respHdr) {
    return HttpRequest(L"GET", url, std::string(), respUtf8, err, timeoutSec, respHdr);
}

// 功能: URL 百分号编码(RFC 3986 保留字符 A-Za-z0-9-_.~ 原样保留, 其余转 %XX)
// 参数: s 原始字符串(UTF-8 字节序列)
// 返回: 编码后的字符串(可直接拼接到 URL 查询串或表单值)
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
// 功能: JSON 字符串转义(反斜杠/引号/换行/回车/制表符), 供拼接 JSON 文件用
// 参数: s 原始字符串(UTF-8)
// 返回: 转义后的字符串(不含首尾引号)
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
// 功能: 迷你 JSON 取值: 从形如 {"k":"v",...} 的对象串中取字符串字段的值
//       并完成反转义(支持 \\ \" \/ \n \r \t 与 \uXXXX)
// 参数: obj JSON 对象串; key 字段名
// 返回: 字段值(UTF-8); 未找到或为空时返回空串
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
                // \uXXXX: 解析4位十六进制 -> UTF-16 码元 -> 转回 UTF-8
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
                else { out += n; }   // 转义不合法则原样保留
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
// 功能: 计算 PNG 使用的 CRC-32(多项式 0xEDB88320), 初始值 0xFFFFFFFF
// 参数: crc 当前校验值; p 数据指针; n 数据字节数
// 返回: 更新后的校验值(结果需再与 0xFFFFFFFF 异或才是最终 CRC)
static uint32_t PngCrcUpdate(uint32_t crc, const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & -(int32_t)(crc & 1));
    }
    return crc;
}
// 功能: 向输出串追加一个 4 字节大端整数(PNG 各字段均为大端)
// 参数: out [出参] 输出串; v 要写入的32位无符号整数
// 返回: 无
static void WrBE(std::string& out, uint32_t v) {
    out += (char)(v >> 24);
    out += (char)(v >> 16);
    out += (char)(v >> 8);
    out += (char)v;
}
// 功能: 写一个 PNG 数据块(chunk): [4字节长度][4字节类型][数据][4字节CRC32]
//       CRC 覆盖 "类型+数据" 两部分
// 参数: out [出参] 输出串; type 4字节块类型(如 "IHDR"); data 块数据
// 返回: 无
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
// 功能: 把内存中的 32 位 BGRA 像素保存为 PNG 文件(--ui 自截图调试用)
//       输出结构: 8字节签名 + IHDR + IDAT + IEND;
//       IDAT 内为 zlib 流(0x78 0x01), 数据用"无压缩存储块"分块存放
//       (每块 <= 65535 字节), 块头含 LEN 与 ~LEN 校验, 末尾附 Adler-32
// 参数: path 输出文件路径; bgra 像素数据(BGRA 顺序, 每像素4字节, 按自上而下的行序);
//       w 宽度(像素); h 高度(像素)
// 返回: true=写入成功; false=参数非法或写文件失败
bool SavePng32(const std::wstring& path, const uint8_t* bgra, int w, int h) {
    if (!bgra || w <= 0 || h <= 0) return false;
    std::string png;
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };   // PNG 魔数签名
    png.append((const char*)sig, 8);
    std::string ihdr;
    WrBE(ihdr, (uint32_t)w);
    WrBE(ihdr, (uint32_t)h);
    ihdr += (char)8;   // bit depth   每个通道 8 位
    ihdr += (char)6;   // RGBA        颜色类型 6 = 真彩+Alpha
    ihdr += (char)0;   // compression 压缩方法 0 (deflate)
    ihdr += (char)0;   // filter      滤波方法 0
    ihdr += (char)0;   // interlace   无隔行
    PngChunk(png, "IHDR", ihdr);

    // 原始数据: 每行 = 1 字节 filter(0) + 像素(BGRA, alpha 强制 255)
    std::string raw;
    raw.reserve((size_t)h * ((size_t)w * 4 + 1));
    std::string row((size_t)w * 4 + 1, 0);
    for (int y = 0; y < h; y++) {
        row[0] = 0;                                   // 每行滤波类型 = 0(无滤波)
        const uint8_t* src = bgra + (size_t)y * w * 4;
        for (int x = 0; x < w; x++) {
            // DIB 内存为 BGRA, PNG 需要 RGBA
            row[1 + x * 4 + 0] = src[x * 4 + 2];      // B -> R
            row[1 + x * 4 + 1] = src[x * 4 + 1];      // G -> G
            row[1 + x * 4 + 2] = src[x * 4 + 0];      // R -> B
            row[1 + x * 4 + 3] = 255;                 // Alpha 恒为不透明
        }
        raw.append(row.data(), row.size());
    }
    // zlib 流: 无压缩存储块 + Adler32
    std::string idat;
    idat += (char)0x78;                               // zlib 头: CMF (deflate, 32K 窗口)
    idat += (char)0x01;                               // zlib 头: FLG (与 CMF 组成校验通过的值)
    size_t pos = 0;
    while (pos < raw.size()) {
        size_t n = raw.size() - pos;
        if (n > 65535) n = 65535;                     // deflate 存储块单块最大 65535 字节
        bool last = (pos + n >= raw.size());          // 最后一块置 BFINAL=1
        idat += (char)(last ? 1 : 0);                 // 块头: BFINAL + BTYPE=00(存储块)
        idat += (char)(n & 0xFF);                     // LEN 低字节
        idat += (char)((n >> 8) & 0xFF);              // LEN 高字节
        idat += (char)((~n) & 0xFF);                  // NLEN(~LEN) 低字节(校验用)
        idat += (char)(((~n) >> 8) & 0xFF);           // NLEN 高字节
        idat.append(raw.data() + pos, n);             // 原始数据本体
        pos += n;
    }
    uint32_t a = 1, b = 0;                            // Adler-32 校验(标准初值 a=1,b=0)
    for (char c : raw) {
        a = (a + (uint8_t)c) % 65521u;
        b = (b + a) % 65521u;
    }
    WrBE(idat, (b << 16) | a);                        // Adler-32 以 (b<<16)|a 大端写入
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
