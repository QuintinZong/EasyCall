// EasyCall 班级叫号系统 - 公共头文件
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>

// ---------------- 编码转换 ----------------
std::wstring U8W(const std::string& s);     // UTF-8 -> wide
std::string  WU8(const std::wstring& s);    // wide -> UTF-8

// ---------------- 路径 / 配置 ----------------
std::wstring ExeDirW();                     // exe 所在目录(带尾斜杠)
std::wstring IniFileW();                    // exe 目录下 easycall.ini
std::wstring IniGet(const wchar_t* sec, const wchar_t* key, const std::wstring& def);
void        IniSet(const wchar_t* sec, const wchar_t* key, const std::wstring& val);
std::wstring NowTimeW();                    // HH:MM:SS
std::string  NowStampMs();                  // 毫秒时间戳(用作叫号ID)

// ---------------- 协议常量 ----------------
constexpr unsigned short EC_TCP_PORT    = 25800;   // 局域网 TCP 端口
constexpr unsigned short EC_UDP_DISC_PORT = 25801; // 局域网 UDP 发现端口
constexpr uint32_t EC_FRAME_MAX = 1048576;          // 单帧 1MB 上限
constexpr const wchar_t* EC_DEFAULT_RELAY = L"";   // 出厂默认不内置任何服务器地址, 由使用者手动填写自己的服务器

bool EcNetStart();
void EcNetStop();

// TCP 帧: 4字节小端长度 + UTF-8 负载
bool        TcpSendFrame(SOCKET s, const std::string& payload);
std::string TcpRecvFrame(SOCKET s, int timeoutMs, bool* ok);  // ok=false: 超时或断开

// items: 每项 "学号\t姓名\t班级"; 生成 "CALL\n<callId>\n<item>\n..."
std::string BuildCallPayload(const std::vector<std::wstring>& items, std::string* callIdOut);
std::vector<std::string> SplitLines(const std::string& s);
std::vector<std::string> SplitTabs(const std::string& s);

// ---------------- UDP 发现 ----------------
struct BoardInfo { std::wstring name, ip; unsigned short port; };
int  DiscoverBoards(std::vector<BoardInfo>& out, int waitMs);   // 返回发现数量
void UdpBroadcastPresence(const std::wstring& name, unsigned short port);
std::vector<std::wstring> LocalIPv4s();

// ---------------- HTTP (WinHTTP) ----------------
bool HttpPostForm(const std::wstring& url, const std::string& bodyUtf8,
                  std::string& respUtf8, std::wstring& err, int timeoutSec,
                  std::string* respHdr = nullptr);
bool HttpGet(const std::wstring& url, std::string& respUtf8, std::wstring& err,
             int timeoutSec, std::string* respHdr = nullptr);
std::string UrlEncode(const std::string& s);
void HttpAbortCurrent();   // 中断另一个线程正在进行的 HTTP 请求(用于退出时快速停止)

// ---------------- 迷你 JSON (仅用于 students.json) ----------------
std::string JsonEscape(const std::string& s);
std::string JsonStrVal(const std::string& obj, const char* key);

// ---------------- 自截图辅助(开发用): 32bpp BGRA 像素 -> PNG ----------------
bool SavePng32(const std::wstring& path, const uint8_t* bgra, int w, int h);
