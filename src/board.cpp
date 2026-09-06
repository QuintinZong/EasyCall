// EasyCall 班级叫号系统 - 班级大屏端
#define _WIN32_IE 0x0600
#include "ec_common.h"
#include <windows.h>
#include <commctrl.h>
#include <winsock2.h>
// MinGW 旧版 gdiplus 头文件缺少 PROPID 定义, 先补上
typedef ULONG PROPID;
#include <gdiplus.h>
#include <vector>
#include <string>
#include <deque>
#include <thread>
#include <atomic>
#include <memory>
#include <algorithm>

using namespace Gdiplus;
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "gdiplus.lib")

#define WM_APP_NEWCALL (WM_APP + 1)
#define WM_APP_CLEAR   (WM_APP + 2)
#define WM_APP_STATUS  (WM_APP + 3)
#define WM_APP_BLACK   (WM_APP + 4)
#define WM_APP_CHAT    (WM_APP + 5)

struct CallItem { std::wstring id, name, cls; };
struct QCall { std::wstring place, teacher; std::vector<CallItem> items; };

enum : INT_PTR { IDC_BTN_SETTINGS = 100, IDC_BTN_BLACK, IDC_BTN_CHAT, IDC_BTN_CLEAR,
       IDC_ED_MODE, IDC_ED_BASE, IDC_ED_ROOM, IDC_ED_PORT, IDC_ED_TITLE,
       IDC_BTN_OK, IDC_BTN_CANCEL,
       IDC_CHAT_LOG = 200, IDC_CHAT_INPUT, IDC_CHAT_SEND };

static HINSTANCE g_hInst;
static HWND g_hwnd, g_btnSettings, g_btnBlack, g_btnChat, g_btnClear;
static HWND g_dlg = nullptr, g_dlgMode, g_dlgBase, g_dlgRoom, g_dlgPort, g_dlgTitle;
static HWND g_chatWnd = nullptr, g_chatLog = nullptr, g_chatInput = nullptr;
static std::deque<QCall> g_queue;                 // 叫号队列(按入队顺序依次显示)
static int g_advanceLeft = 20;                    // 当前叫号剩余显示秒数
static std::vector<std::wstring> g_history;
static std::vector<std::wstring> g_chatMsgs;
static std::vector<std::string> g_sentChatIds;
static std::wstring g_lastCallId;
static std::wstring g_statusText = L"正在启动…";
static std::wstring g_title = L"叫号";
static std::wstring g_base = EC_DEFAULT_RELAY;
static std::wstring g_room = L"101";
static std::wstring g_mode = L"lan";
static int g_port = EC_TCP_PORT;
static int g_dpi = 96;
static bool g_online = false;
static bool g_black = false;
static bool g_callHidden = false;   // 本机一键清屏: 仅隐藏当前叫号显示, 不清除队列
static int g_flash = 0;
static std::atomic<bool> g_stop{false};
static SOCKET g_listen = INVALID_SOCKET;
static SOCKET g_client = INVALID_SOCKET;
static std::thread g_threadNet, g_threadBroad, g_threadPresence;
static HWND g_hoverBtn = nullptr;
static std::wstring g_uiSnapPath;
static int g_forceDpi = 0;
static int g_uiDelay = 0;   // --ui 截图延迟(毫秒), 0=默认1500
static bool (*g_chatSender)(const std::wstring&) = nullptr;
static void DoUiSnap();
static void HandlePayload(const std::string& payload, bool fromTcp, SOCKET replySock);
static void EnsureChatWindow(bool focus = true);

static int S(int px) { return MulDiv(px, g_dpi, 96); }

static HFONT MakeFont(int pt, int weight) {
    return CreateFontW(-MulDiv(pt, g_dpi, 72), 0, 0, 0, weight, 0, 0, 0,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
}
static void SetStatus(const std::wstring& t, bool online) {
    g_statusText = t;
    g_online = online;
}
static std::wstring MakeLine(const CallItem& it) {
    std::wstring s;
    if (!it.id.empty()) s += it.id + L"   ";
    s += it.name;
    if (!it.cls.empty()) s += L"　(" + it.cls + L")";
    return s;
}

// ---------------- 对话 ----------------
static std::wstring ChatFile() { return ExeDirW() + L"chat_board.json"; }
static void ChatSave() {
    std::string json = "[";
    for (size_t i = 0; i < g_chatMsgs.size(); i++) {
        if (i) json += ",";
        json += "{\"t\":\"" + JsonEscape(WU8(g_chatMsgs[i])) + "\"}";
    }
    json += "]";
    HANDLE h = CreateFileW(ChatFile().c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        WriteFile(h, json.data(), (DWORD)json.size(), &w, nullptr);
        CloseHandle(h);
    }
}
static void ChatLoad() {
    g_chatMsgs.clear();
    HANDLE h = CreateFileW(ChatFile().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD hi = 0;
    DWORD size = GetFileSize(h, &hi);
    if (hi || size == 0 || size > 4 * 1024 * 1024) { CloseHandle(h); return; }
    std::string json(size, 0);
    DWORD got = 0;
    if (!ReadFile(h, &json[0], size, &got, nullptr) || got != size) { CloseHandle(h); return; }
    CloseHandle(h);
    size_t pos = 0;
    for (;;) {
        size_t a = json.find('{', pos);
        if (a == std::string::npos) break;
        size_t b = json.find('}', a);
        if (b == std::string::npos) break;
        std::wstring t = U8W(JsonStrVal(json.substr(a, b - a + 1), "t"));
        if (!t.empty()) g_chatMsgs.push_back(t);
        pos = b + 1;
    }
}
static void ChatWipe() {
    DeleteFileW(ChatFile().c_str());
    g_chatMsgs.clear();
}
static void ChatLogAppend(const std::wstring& line) {
    if (!g_chatLog) return;
    int len = GetWindowTextLengthW(g_chatLog);
    SendMessageW(g_chatLog, EM_SETSEL, len, len);
    std::wstring out = line + L"\r\n";
    SendMessageW(g_chatLog, EM_REPLACESEL, 0, (LPARAM)out.c_str());
}
static void ChatAppend(const std::wstring& sender, const std::wstring& text) {
    g_chatMsgs.push_back(NowTimeW() + L" " + sender + L": " + text);
    while (g_chatMsgs.size() > 500) g_chatMsgs.erase(g_chatMsgs.begin());
    ChatLogAppend(g_chatMsgs.back());
    ChatSave();
}
static void OnChatFrame(const std::string& frame) {
    std::vector<std::string> lines = SplitLines(frame);
    if (lines.size() < 3 || lines[0] != "CHAT") return;
    for (auto& s : g_sentChatIds) if (s == lines[1]) return;   // 自己发的回显, 跳过
    std::wstring sender = U8W(lines[2]);
    std::wstring text;
    for (size_t i = 3; i < lines.size(); i++) {
        if (i > 3) text += L"\n";
        text += U8W(lines[i]);
    }
    ChatAppend(sender, text);
    // 收到消息自动弹出对话窗口(不抢焦点)
    if (g_chatWnd && IsWindow(g_chatWnd)) ShowWindow(g_chatWnd, SW_SHOW);
    else EnsureChatWindow(false);
}
static bool BoardSendChat(const std::wstring& text) {
    std::wstring t = text;
    {
        size_t a = t.find_first_not_of(L" \t\r\n");
        if (a == std::wstring::npos) return false;
        size_t b = t.find_last_not_of(L" \t\r\n");
        t = t.substr(a, b - a + 1);
    }
    if (t.empty()) return false;
    std::string msgId = NowStampMs();
    g_sentChatIds.push_back(msgId);
    while (g_sentChatIds.size() > 64) g_sentChatIds.erase(g_sentChatIds.begin());
    std::string payload = "CHAT\n" + msgId + "\n" + WU8(g_title) + "\n" + WU8(t);
    bool ok = false;
    if (g_mode == L"relay") {
        std::string room = WU8(g_room);
        if (!g_base.empty() && !room.empty()) {
            std::string body = "room=" + UrlEncode(room) + "&data=" + UrlEncode(payload);
            std::string resp;
            std::wstring err;
            ok = HttpPostForm(g_base + L"push.php", body, resp, err, 12) &&
                 resp.find("OK") != std::string::npos;
            if (!ok) ChatAppend(L"系统", L"发送失败: " + err);
        } else {
            ChatAppend(L"系统", L"发送失败: 未配置服务器地址/房间号");
        }
    } else {
        if (g_client != INVALID_SOCKET) {
            ok = TcpSendFrame(g_client, payload);
            if (!ok) ChatAppend(L"系统", L"发送失败: 教师端已断开");
        } else {
            ChatAppend(L"系统", L"发送失败: 教师端未连接");
        }
    }
    if (ok) ChatAppend(g_title, t);
    return ok;
}

// ---------------- 对话窗口 ----------------
static LRESULT CALLBACK ChatInputProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_KEYDOWN && w == VK_RETURN) {
        PostMessageW(GetParent(h), WM_COMMAND, MAKEWPARAM(IDC_CHAT_SEND, BN_CLICKED), (LPARAM)h);
        return 0;
    }
    return CallWindowProcW((WNDPROC)GetPropW(h, L"EcOrigProc"), h, m, w, l);
}
static LRESULT CALLBACK ChatProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_chatLog = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                    WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE |
                                        ES_READONLY | ES_AUTOVSCROLL,
                                    0, 0, 10, 10, hwnd, (HMENU)IDC_CHAT_LOG, g_hInst, nullptr);
        g_chatInput = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                      WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                      0, 0, 10, 10, hwnd, (HMENU)IDC_CHAT_INPUT, g_hInst, nullptr);
        WNDPROC orig = (WNDPROC)GetWindowLongPtrW(g_chatInput, GWLP_WNDPROC);
        SetPropW(g_chatInput, L"EcOrigProc", (HANDLE)orig);
        SetWindowLongPtrW(g_chatInput, GWLP_WNDPROC, (LONG_PTR)ChatInputProc);
        CreateWindowExW(0, L"BUTTON", L"发送",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                        0, 0, 10, 10, hwnd, (HMENU)IDC_CHAT_SEND, g_hInst, nullptr);
        {
            HFONT f = MakeFont(11, FW_NORMAL);
            SendMessageW(g_chatLog, WM_SETFONT, (WPARAM)f, TRUE);
            SendMessageW(g_chatInput, WM_SETFONT, (WPARAM)f, TRUE);
            SendMessageW(GetDlgItem(hwnd, IDC_CHAT_SEND), WM_SETFONT, (WPARAM)f, TRUE);
        }
        for (auto& line : g_chatMsgs) ChatLogAppend(line);
        return 0;
    }
    case WM_SIZE: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        int hh = S(30);
        MoveWindow(g_chatLog, S(8), S(8), rc.right - S(16), rc.bottom - S(52) - hh, TRUE);
        MoveWindow(g_chatInput, S(8), rc.bottom - hh - S(12), rc.right - S(96), hh, TRUE);
        MoveWindow(GetDlgItem(hwnd, IDC_CHAT_SEND), rc.right - S(80), rc.bottom - hh - S(13), S(72), hh + 2, TRUE);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_CHAT_SEND) {
            if (g_chatSender) {
                std::wstring t;
                int n = GetWindowTextLengthW(g_chatInput);
                t.resize(n);
                if (n) GetWindowTextW(g_chatInput, &t[0], n + 1);
                if (!t.empty()) {
                    g_chatSender(t);
                    SetWindowTextW(g_chatInput, L"");
                    SetFocus(g_chatInput);
                }
            }
            return 0;
        }
        break;
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
static void EnsureChatWindow(bool focus) {
    if (g_chatWnd && IsWindow(g_chatWnd)) {
        ShowWindow(g_chatWnd, SW_SHOW);
        if (focus) SetForegroundWindow(g_chatWnd);
        return;
    }
    RECT mrc;
    GetWindowRect(g_hwnd, &mrc);
    g_chatWnd = CreateWindowExW(0, L"EasyCallChatWnd", L"对话 - 班级大屏",
                                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                mrc.left + S(60), mrc.top + S(80), S(540), S(540),
                                g_hwnd, nullptr, g_hInst, nullptr);
    ShowWindow(g_chatWnd, SW_SHOW);
}

// ---------------- 协议处理 ----------------
static void HandlePayload(const std::string& payload, bool fromTcp, SOCKET replySock) {
    std::vector<std::string> lines = SplitLines(payload);
    if (lines.empty()) return;
    if (lines[0] == "CALL") {
        if (lines.size() < 4) return;
        if (lines[1] == WU8(g_lastCallId)) return;   // 去重
        g_lastCallId = U8W(lines[1]);
        QCall q;
        q.place = lines[2].empty() ? L"台前" : U8W(lines[2]);
        q.teacher = lines[3].empty() ? L"教师" : U8W(lines[3]);
        for (size_t i = 4; i < lines.size(); i++) {
            std::vector<std::string> f = SplitTabs(lines[i]);
            CallItem it;
            if (f.size() > 0) it.id = U8W(f[0]);
            if (f.size() > 1) it.name = U8W(f[1]);
            if (f.size() > 2) it.cls = U8W(f[2]);
            if (!it.id.empty() || !it.name.empty()) q.items.push_back(it);
        }
        if (!q.items.empty()) {
            QCall* m = new QCall(q);
            PostMessageW(g_hwnd, WM_APP_NEWCALL, 0, (LPARAM)m);
        }
    } else if (lines[0] == "CHAT") {
        PostMessageW(g_hwnd, WM_APP_CHAT, 0, (LPARAM)new std::string(payload));
    } else if (lines[0] == "CLEAR") {
        PostMessageW(g_hwnd, WM_APP_CLEAR, 0, 0);
    } else if (lines[0] == "BLACK") {
        PostMessageW(g_hwnd, WM_APP_BLACK, 0, 0);
    } else if (lines[0] == "PING" && fromTcp && replySock != INVALID_SOCKET) {
        TcpSendFrame(replySock, "PONG");
    }
}

// ---------------- 局域网模式 ----------------
static void LanThreadProc() {
    for (;;) {
        g_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (g_listen == INVALID_SOCKET) break;
        BOOL b = TRUE;
        setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, (const char*)&b, sizeof b);
        sockaddr_in sa;
        memset(&sa, 0, sizeof sa);
        sa.sin_family = AF_INET;
        sa.sin_port = htons((u_short)g_port);
        sa.sin_addr.s_addr = INADDR_ANY;
        if (bind(g_listen, (sockaddr*)&sa, sizeof sa) != 0 || listen(g_listen, 4) != 0) {
            PostMessageW(g_hwnd, WM_APP_STATUS, 0,
                         (LPARAM)new std::wstring(L"端口 " + std::to_wstring(g_port) +
                                                  L" 被占用, 请在设置中更换端口"));
            closesocket(g_listen);
            g_listen = INVALID_SOCKET;
            break;
        }
        std::wstring ips;
        for (auto& ip : LocalIPv4s()) {
            if (!ips.empty()) ips += L" / ";
            ips += ip;
        }
        if (ips.empty()) ips = L"127.0.0.1";
        PostMessageW(g_hwnd, WM_APP_STATUS, 1,
                     (LPARAM)new std::wstring(L"监听中, 本机IP: " + ips + L" 端口 " +
                                              std::to_wstring(g_port)));
        while (!g_stop.load()) {
            sockaddr_in from;
            int fl = sizeof from;
            g_client = accept(g_listen, (sockaddr*)&from, &fl);
            if (g_client == INVALID_SOCKET) {
                if (g_stop.load()) break;
                continue;
            }
            char ip[64] = {0};
            inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
            PostMessageW(g_hwnd, WM_APP_STATUS, 1,
                         (LPARAM)new std::wstring(L"教师端已连接: " + U8W(ip)));
            for (;;) {
                bool ok = false;
                std::string f = TcpRecvFrame(g_client, 40000, &ok);
                if (!ok) break;
                HandlePayload(f, true, g_client);
            }
            closesocket(g_client);
            g_client = INVALID_SOCKET;
            PostMessageW(g_hwnd, WM_APP_STATUS, 1,
                         (LPARAM)new std::wstring(L"教师端已断开, 继续监听端口 " +
                                                  std::to_wstring(g_port)));
        }
        closesocket(g_listen);
        g_listen = INVALID_SOCKET;
        break;
    }
}
static void BroadThreadProc() {
    while (!g_stop.load()) {
        UdpBroadcastPresence(g_title + L" · 教室大屏", (unsigned short)g_port);
        Sleep(2000);
    }
}

// ---------------- 服务器中转模式 ----------------
static void RelayThreadProc() {
    std::wstring base = g_base;
    std::string room = WU8(g_room);
    long long after = 0;
    while (!g_stop.load()) {
        if (base.empty()) {
            PostMessageW(g_hwnd, WM_APP_STATUS, 0,
                         (LPARAM)new std::wstring(L"未配置服务器地址: 请点右下角[设置]填写自己的服务器"));
            Sleep(2000);
            continue;
        }
        std::string resp, hdr;
        std::wstring err;
        std::wstring url = base + L"fetch.php?room=" + U8W(UrlEncode(room)) +
                           L"&after=" + std::to_wstring(after);
        if (HttpGet(url, resp, err, 32, &hdr)) {
            PostMessageW(g_hwnd, WM_APP_STATUS, 1,
                         (LPARAM)new std::wstring(L"已连接服务器, 房间[" + g_room + L"], 等待叫号…"));
            long long seq = HttpSeqFromHeader(hdr);
            if (seq > after) after = seq;
            std::vector<std::string> frames;
            ParseFrames(resp, frames);
            for (auto& fr : frames) HandlePayload(fr, false, INVALID_SOCKET);
        } else {
            PostMessageW(g_hwnd, WM_APP_STATUS, 0,
                         (LPARAM)new std::wstring(L"服务器连接失败, 3秒后重试…"));
            Sleep(3000);
        }
    }
}
static void PresenceThreadProc() {
    while (!g_stop.load()) {
        std::string resp;
        std::wstring err;
        std::string body = "room=" + UrlEncode(WU8(g_room));
        HttpPostForm(g_base + L"presence.php", body, resp, err, 6);
        Sleep(8000);
    }
}
static void StartNet() {
    g_stop.store(false);
    if (g_mode == L"relay") {
        g_threadNet = std::thread(RelayThreadProc);
        g_threadPresence = std::thread(PresenceThreadProc);
    } else {
        g_threadNet = std::thread(LanThreadProc);
        g_threadBroad = std::thread(BroadThreadProc);
    }
}
static void StopNet() {
    g_stop.store(true);
    HttpAbortCurrent();
    if (g_listen != INVALID_SOCKET) closesocket(g_listen);
    if (g_client != INVALID_SOCKET) closesocket(g_client);
    if (g_threadNet.joinable()) g_threadNet.join();
    if (g_threadBroad.joinable()) g_threadBroad.join();
    if (g_threadPresence.joinable()) g_threadPresence.join();
}

// ---------------- Fluent 暗色按钮 ----------------
static LRESULT CALLBACK FluentBtnProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_MOUSEMOVE:
        if (g_hoverBtn != h) {
            g_hoverBtn = h;
            InvalidateRect(h, nullptr, TRUE);
        }
        {
            TRACKMOUSEEVENT tme;
            memset(&tme, 0, sizeof tme);
            tme.cbSize = sizeof tme;
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = h;
            TrackMouseEvent(&tme);
        }
        break;
    case WM_MOUSELEAVE:
        if (g_hoverBtn == h) {
            g_hoverBtn = nullptr;
            InvalidateRect(h, nullptr, TRUE);
        }
        break;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
        InvalidateRect(h, nullptr, TRUE);
        break;
    }
    return CallWindowProcW((WNDPROC)GetPropW(h, L"EcOrigProc"), h, m, w, l);
}
static void SubmitButton(HWND h) {
    WNDPROC orig = (WNDPROC)GetWindowLongPtrW(h, GWLP_WNDPROC);
    SetPropW(h, L"EcOrigProc", (HANDLE)orig);
    SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)FluentBtnProc);
}
static Gdiplus::Color FluentColor(COLORREF c) {
    return Gdiplus::Color(255, GetRValue(c), GetGValue(c), GetBValue(c));
}
static void DrawDarkButton(HDC dc, const RECT& rc, const wchar_t* text,
                           bool hover, bool pressed, HFONT font) {
    Graphics g(dc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    {
        SolidBrush bg(FluentColor(g_black ? RGB(0, 0, 0) : RGB(13, 27, 48)));
        g.FillRectangle(&bg, (INT)rc.left, (INT)rc.top,
                        (INT)(rc.right - rc.left), (INT)(rc.bottom - rc.top));
    }
    int rad = 4;
    int w = rc.right - rc.left - 1, h = rc.bottom - rc.top - 1;
    int d = rad * 2;
    GraphicsPath path;
    path.AddArc(rc.left, rc.top, d, d, 180, 90);
    path.AddArc(rc.left + w - d, rc.top, d, d, 270, 90);
    path.AddArc(rc.left + w - d, rc.top + h - d, d, d, 0, 90);
    path.AddArc(rc.left, rc.top + h - d, d, d, 90, 90);
    path.CloseFigure();
    SolidBrush fill(FluentColor(pressed ? RGB(22, 22, 22) : hover ? RGB(48, 48, 48) : RGB(36, 36, 36)));
    Pen pen(FluentColor(pressed ? RGB(70, 70, 70) : hover ? RGB(96, 96, 96) : RGB(78, 78, 78)), 1.0f);
    g.FillPath(&fill, &path);
    g.DrawPath(&pen, &path);
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ of = SelectObject(dc, font);
    SetTextColor(dc, RGB(0xEA, 0xEA, 0xEA));
    RECT r = rc;
    DrawTextW(dc, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, of);
}

// ---------------- 绘制 ----------------
static void PaintDraw(HDC dc, const RECT& rc) {
    if (g_black) {
        HBRUSH br = CreateSolidBrush(RGB(0, 0, 0));
        FillRect(dc, &rc, br);
        DeleteObject(br);
        SetBkMode(dc, TRANSPARENT);
        int cy = rc.bottom / 2;
        HFONT fBig = MakeFont(64, FW_BOLD);
        SelectObject(dc, fBig);
        SetTextColor(dc, RGB(0xFF, 0xFF, 0xFF));
        RECT mr = { S(40), cy - S(140), rc.right - S(40), cy - S(10) };
        DrawTextW(dc, L"保持安静", -1, &mr, DT_CENTER | DT_BOTTOM | DT_SINGLELINE);
        DeleteObject(fBig);
        HFONT fSub = MakeFont(14, FW_NORMAL);
        SelectObject(dc, fSub);
        SetTextColor(dc, RGB(0x99, 0x99, 0x99));
        RECT sr = { S(40), cy + S(10), rc.right - S(40), cy + S(70) };
        DrawTextW(dc, L"请安静自习", -1, &sr, DT_CENTER | DT_TOP | DT_SINGLELINE);
        DeleteObject(fSub);
        return;
    }

    bool flashOn = (g_flash > 0) && ((g_flash & 1) == 1);
    COLORREF bg = flashOn ? RGB(58, 88, 132) : RGB(13, 27, 48);
    HBRUSH br = CreateSolidBrush(bg);
    FillRect(dc, &rc, br);
    DeleteObject(br);

    int topH = S(64);
    RECT tr = { 0, 0, rc.right, topH };
    br = CreateSolidBrush(RGB(8, 18, 34));
    FillRect(dc, &tr, br);
    DeleteObject(br);
    SetBkMode(dc, TRANSPARENT);

    HFONT fTitle = MakeFont(26, FW_BOLD);
    SelectObject(dc, fTitle);
    SetTextColor(dc, RGB(255, 255, 255));
    RECT ttl = { S(24), 0, rc.right - S(360), topH };
    DrawTextW(dc, (g_title + L" · EasyCall").c_str(), -1, &ttl, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DeleteObject(fTitle);

    HFONT fSt = MakeFont(11, FW_NORMAL);
    SelectObject(dc, fSt);
    SetTextColor(dc, g_online ? RGB(110, 220, 130) : RGB(235, 120, 110));
    RECT st = { S(24), 0, rc.right - S(24), topH };
    DrawTextW(dc, g_statusText.c_str(), -1, &st, DT_RIGHT | DT_VCENTER | DT_END_ELLIPSIS | DT_SINGLELINE);
    DeleteObject(fSt);

    int mainTop = topH + S(26);
    if (!g_queue.empty() && !g_callHidden) {
        const QCall& q = g_queue.front();
        HFONT fHead = MakeFont(20, FW_BOLD);
        SelectObject(dc, fHead);
        SetTextColor(dc, RGB(255, 214, 90));
        RECT hr = { S(40), mainTop, rc.right - S(40), mainTop + S(42) };
        DrawTextW(dc, (L"请以下同学到" + q.place + L"集合").c_str(), -1, &hr,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        DeleteObject(fHead);

        HFONT fTch = MakeFont(15, FW_NORMAL);
        SelectObject(dc, fTch);
        SetTextColor(dc, RGB(160, 190, 230));
        RECT trc = { S(40), mainTop + S(40), rc.right - S(40), mainTop + S(66) };
        std::wstring tline = L"教师: " + q.teacher;
        if (g_queue.size() > 1) tline += L"  (后续排队 " + std::to_wstring(g_queue.size() - 1) + L" 位)";
        DrawTextW(dc, tline.c_str(), -1, &trc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        DeleteObject(fTch);

        int pts = 46;
        if (q.items.size() > 8) pts = 40;
        if (q.items.size() > 14) pts = 32;
        if (q.items.size() > 20) pts = 26;
        if (q.items.size() > 28) pts = 20;
        int maxW = rc.right - S(80);
        HFONT fBig = nullptr;
        for (;;) {
            if (fBig) DeleteObject(fBig);
            fBig = MakeFont(pts, FW_BOLD);
            HGDIOBJ oldF = SelectObject(dc, fBig);
            int widest = 0;
            for (auto& it : q.items) {
                std::wstring line = MakeLine(it);
                SIZE sz;
                if (GetTextExtentPoint32W(dc, line.c_str(), (int)line.size(), &sz))
                    widest = (std::max)(widest, (int)sz.cx);
            }
            SelectObject(dc, oldF);
            if (widest <= maxW || pts <= 16) break;
            pts -= 2;
        }
        HGDIOBJ oldF = SelectObject(dc, fBig);
        SetTextColor(dc, RGB(245, 245, 245));
        SetTextAlign(dc, TA_CENTER | TA_TOP);
        int cx = rc.right / 2;
        int lineH = MulDiv(pts, g_dpi, 72) + S(18);
        int y = mainTop + S(84);
        for (auto& it : q.items) {
            std::wstring line = MakeLine(it);
            TextOutW(dc, cx, y, line.c_str(), (int)line.size());
            y += lineH;
        }
        SetTextAlign(dc, TA_LEFT | TA_TOP);
        SelectObject(dc, oldF);
        DeleteObject(fBig);
    } else {
        HFONT fIdle = MakeFont(24, FW_NORMAL);
        SelectObject(dc, fIdle);
        SetTextColor(dc, RGB(110, 130, 160));
        RECT ir = { S(40), mainTop + S(60), rc.right - S(40), mainTop + S(160) };
        DrawTextW(dc, L"等待叫号…", -1, &ir, DT_CENTER | DT_VCENTER);
        DeleteObject(fIdle);
    }

    if (!g_history.empty()) {
        HFONT fH = MakeFont(10, FW_NORMAL);
        SelectObject(dc, fH);
        SetTextColor(dc, RGB(130, 150, 175));
        int lineH = S(22);
        int startY = rc.bottom - S(16) - lineH * (int)g_history.size();
        for (size_t i = 0; i < g_history.size(); i++) {
            RECT hr2 = { S(24), startY + lineH * (int)i, rc.right - S(300), startY + lineH * (int)i + lineH };
            DrawTextW(dc, g_history[i].c_str(), -1, &hr2, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS | DT_SINGLELINE);
        }
        DeleteObject(fH);
    }
}
static void Paint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    PaintDraw(dc, rc);
    EndPaint(hwnd, &ps);
}

// ---------------- 设置对话框 ----------------
static void DlgLayout() {
    int x = S(16), w = S(300);
    auto move = [&](int id, int yy, int hh) { MoveWindow(GetDlgItem(g_dlg, id), x, yy, w, hh, TRUE); };
    move(IDC_ED_MODE, S(38), S(120));
    int y = S(66);
    move(IDC_ED_BASE, y + S(22), S(24)); y += S(54);
    move(IDC_ED_ROOM, y + S(22), S(24)); y += S(54);
    move(IDC_ED_PORT, y + S(22), S(24)); y += S(54);
    move(IDC_ED_TITLE, y + S(22), S(24)); y += S(54);
    MoveWindow(GetDlgItem(g_dlg, IDC_BTN_OK), x, y + S(8), S(120), S(32), TRUE);
    MoveWindow(GetDlgItem(g_dlg, IDC_BTN_CANCEL), x + S(160), y + S(8), S(120), S(32), TRUE);
}
static LRESULT CALLBACK DlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_dlg = hwnd;
        auto mkLabel = [&](const wchar_t* t, int yy) {
            CreateWindowExW(0, L"STATIC", t, WS_CHILD | WS_VISIBLE, S(16), yy, S(300), S(20),
                            hwnd, nullptr, g_hInst, nullptr);
        };
        auto mkEdit = [&](const wchar_t* t, INT_PTR id, DWORD style, int yy, int hh) {
            return CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", t,
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | style,
                                   S(16), yy, S(300), hh, hwnd, (HMENU)id, g_hInst, nullptr);
        };
        mkLabel(L"运行模式:", S(18));
        g_dlgMode = CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", L"",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
                                    S(16), S(38), S(300), S(300), hwnd,
                                    (HMENU)IDC_ED_MODE, g_hInst, nullptr);
        SendMessageW(g_dlgMode, CB_ADDSTRING, 0, (LPARAM)L"局域网直连(同一网络)");
        SendMessageW(g_dlgMode, CB_ADDSTRING, 0, (LPARAM)L"服务器中转(跨网络)");
        SendMessageW(g_dlgMode, CB_SETCURSEL, g_mode == L"relay" ? 1 : 0, 0);
        mkLabel(L"中转服务器地址:", S(66));
        g_dlgBase = mkEdit(g_base.c_str(), IDC_ED_BASE, ES_AUTOHSCROLL, S(88), S(24));
        mkLabel(L"房间号(两端一致):", S(120));
        g_dlgRoom = mkEdit(g_room.c_str(), IDC_ED_ROOM, ES_AUTOHSCROLL, S(142), S(24));
        mkLabel(L"局域网监听端口:", S(174));
        g_dlgPort = mkEdit(std::to_wstring(g_port).c_str(), IDC_ED_PORT, ES_AUTOHSCROLL | ES_NUMBER, S(196), S(24));
        mkLabel(L"大屏标题:", S(228));
        g_dlgTitle = mkEdit(g_title.c_str(), IDC_ED_TITLE, ES_AUTOHSCROLL, S(250), S(24));
        CreateWindowExW(0, L"BUTTON", L"保存", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                        S(16), S(292), S(120), S(32), hwnd, (HMENU)IDC_BTN_OK, g_hInst, nullptr);
        CreateWindowExW(0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                        S(176), S(292), S(120), S(32), hwnd, (HMENU)IDC_BTN_CANCEL, g_hInst, nullptr);
        DlgLayout();
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_BTN_OK) {
            int sel = (int)SendMessageW(g_dlgMode, CB_GETCURSEL, 0, 0);
            std::wstring mode = (sel == 1) ? L"relay" : L"lan";
            std::wstring base, room, port, title;
            int nb = GetWindowTextLengthW(g_dlgBase);
            base.resize(nb); if (nb) GetWindowTextW(g_dlgBase, &base[0], nb + 1);
            nb = GetWindowTextLengthW(g_dlgRoom);
            room.resize(nb); if (nb) GetWindowTextW(g_dlgRoom, &room[0], nb + 1);
            nb = GetWindowTextLengthW(g_dlgPort);
            port.resize(nb); if (nb) GetWindowTextW(g_dlgPort, &port[0], nb + 1);
            nb = GetWindowTextLengthW(g_dlgTitle);
            title.resize(nb); if (nb) GetWindowTextW(g_dlgTitle, &title[0], nb + 1);
            int prt = _wtoi(port.c_str());
            if (prt <= 0 || prt > 65535) prt = EC_TCP_PORT;
            IniSet(L"board", L"mode", mode);
            IniSet(L"board", L"base", base);
            IniSet(L"board", L"room", room);
            IniSet(L"board", L"port", std::to_wstring(prt));
            IniSet(L"board", L"title", title);
            MessageBoxW(hwnd, L"设置已保存, 重启程序后生效", L"提示", MB_ICONINFORMATION);
            DestroyWindow(hwnd);
            return 0;
        }
        if (LOWORD(wp) == IDC_BTN_CANCEL) {
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_DESTROY:
        g_dlg = nullptr;
        EnableWindow(g_hwnd, TRUE);
        SetForegroundWindow(g_hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
static void ShowSettings() {
    EnableWindow(g_hwnd, FALSE);
    RECT rc;
    GetWindowRect(g_hwnd, &rc);
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"EasyCallBoardDlg", L"大屏设置",
                               WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                               rc.left + (rc.right - rc.left) / 2 - S(190),
                               rc.top + (rc.bottom - rc.top) / 2 - S(200),
                               S(380), S(420), g_hwnd, nullptr, g_hInst, nullptr);
    if (!dlg) { EnableWindow(g_hwnd, TRUE); return; }
    MSG m;
    while (IsWindow(dlg)) {
        BOOL got = GetMessageW(&m, nullptr, 0, 0);
        if (got <= 0) {
            if (got == 0) PostQuitMessage((int)m.wParam);
            break;
        }
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}

// ---------------- 主窗口 ----------------
static LRESULT CALLBACK BoardProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_hwnd = hwnd;
        HDC dc = GetDC(nullptr);
        g_dpi = g_forceDpi > 0 ? g_forceDpi : GetDeviceCaps(dc, LOGPIXELSY);
        ReleaseDC(nullptr, dc);
        g_btnSettings = CreateWindowExW(0, L"BUTTON", L"设置",
                                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                        0, 0, 10, 10, hwnd, (HMENU)IDC_BTN_SETTINGS, g_hInst, nullptr);
        g_btnBlack = CreateWindowExW(0, L"BUTTON", L"黑屏",
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                     0, 0, 10, 10, hwnd, (HMENU)IDC_BTN_BLACK, g_hInst, nullptr);
        g_btnChat = CreateWindowExW(0, L"BUTTON", L"对话",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                    0, 0, 10, 10, hwnd, (HMENU)IDC_BTN_CHAT, g_hInst, nullptr);
        g_btnClear = CreateWindowExW(0, L"BUTTON", L"一键清空",
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                     0, 0, 10, 10, hwnd, (HMENU)IDC_BTN_CLEAR, g_hInst, nullptr);
        SubmitButton(g_btnSettings);
        SubmitButton(g_btnBlack);
        SubmitButton(g_btnChat);
        SubmitButton(g_btnClear);
        ChatLoad();
        SetTimer(hwnd, 1, 1000, nullptr);
        SetTimer(hwnd, 2, 300, nullptr);
        SetTimer(hwnd, 3, 1000, nullptr);
        if (!g_uiSnapPath.empty())
            SetTimer(hwnd, 77, g_uiDelay > 0 ? g_uiDelay : 1500, nullptr);
        StartNet();
        return 0;
    }
    case WM_SIZE: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        MoveWindow(g_btnSettings, rc.right - S(102), rc.bottom - S(52), S(88), S(36), TRUE);
        MoveWindow(g_btnBlack, rc.right - S(198), rc.bottom - S(52), S(88), S(36), TRUE);
        MoveWindow(g_btnChat, rc.right - S(294), rc.bottom - S(52), S(88), S(36), TRUE);
        MoveWindow(g_btnClear, rc.right - S(390), rc.bottom - S(52), S(88), S(36), TRUE);
        return 0;
    }
    case WM_PAINT:
        Paint(hwnd);
        return 0;
    case WM_TIMER:
        if (wp == 77) {
            DoUiSnap();
            KillTimer(hwnd, 77);
            PostQuitMessage(0);
            return 0;
        }
        if (wp == 1) InvalidateRect(hwnd, nullptr, FALSE);
        else if (wp == 2) {
            if (g_flash > 0) { g_flash--; InvalidateRect(hwnd, nullptr, FALSE); }
            else if (g_black) InvalidateRect(hwnd, nullptr, FALSE);
        } else if (wp == 3) {
            // 叫号队列: 按入队顺序依次叫号, 每条显示约20秒
            if (!g_black && g_queue.size() > 1) {
                g_advanceLeft--;
                if (g_advanceLeft <= 0) {
                    g_queue.pop_front();
                    g_callHidden = false;
                    g_advanceLeft = 20;
                    g_flash = 8;
                    InvalidateRect(hwnd, nullptr, TRUE);
                }
            }
        }
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_BTN_SETTINGS) ShowSettings();
        else if (LOWORD(wp) == IDC_BTN_BLACK) {
            g_black = !g_black;
            SetWindowTextW(g_btnBlack, g_black ? L"恢复显示" : L"黑屏");
            InvalidateRect(hwnd, nullptr, TRUE);
        } else if (LOWORD(wp) == IDC_BTN_CHAT) EnsureChatWindow();
        else if (LOWORD(wp) == IDC_BTN_CLEAR) {
            g_callHidden = true;
            g_history.clear();
            if (g_black) {
                g_black = false;
                SetWindowTextW(g_btnBlack, L"黑屏");
            }
            InvalidateRect(hwnd, nullptr, TRUE);
        }
        return 0;
    case WM_APP_NEWCALL: {
        std::unique_ptr<QCall> m((QCall*)lp);
        g_queue.push_back(*m);
        if (g_queue.size() == 1) g_callHidden = false;
        g_advanceLeft = 20;
        g_flash = 10;
        std::wstring names;
        for (auto& it : m->items) {
            if (!names.empty()) names += L"、";
            names += it.name;
        }
        g_history.push_back(NowTimeW() + L" " + m->teacher + L" 叫号 " +
                            std::to_wstring(m->items.size()) + L" 人 → " + m->place + L": " + names);
        while (g_history.size() > 12) g_history.erase(g_history.begin());
        if (g_black) {
            g_black = false;
            SetWindowTextW(g_btnBlack, L"黑屏");
        }
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    }
    case WM_APP_CLEAR:
        g_queue.clear();
        g_history.clear();
        if (g_black) {
            g_black = false;
            SetWindowTextW(g_btnBlack, L"黑屏");
        }
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    case WM_APP_BLACK:
        g_black = true;
        SetWindowTextW(g_btnBlack, L"恢复显示");
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    case WM_APP_CHAT: {
        std::unique_ptr<std::string> f((std::string*)lp);
        OnChatFrame(*f);
        return 0;
    }
    case WM_APP_STATUS: {
        std::unique_ptr<std::wstring> t((std::wstring*)lp);
        SetStatus(*t, wp != 0);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PRINT: {
        HDC dc = (HDC)wp;
        RECT rc;
        GetClientRect(hwnd, &rc);
        PaintDraw(dc, rc);
        if (lp & PRF_CHILDREN) {
            struct Ctx { HDC dc; POINT org; };
            Ctx ctx;
            ctx.dc = dc;
            POINT cOrg = { 0, 0 };
            MapWindowPoints(hwnd, nullptr, &cOrg, 1);
            ctx.org = cOrg;
            EnumChildWindows(hwnd, [](HWND c, LPARAM l) -> BOOL {
                Ctx* p = (Ctx*)l;
                RECT cr;
                GetWindowRect(c, &cr);
                int ox = cr.left - p->org.x;
                int oy = cr.top - p->org.y;
                int cid = GetDlgCtrlID(c);
                if (cid == IDC_BTN_SETTINGS || cid == IDC_BTN_BLACK || cid == IDC_BTN_CHAT ||
                    cid == IDC_BTN_CLEAR) {
                    wchar_t txt[128];
                    GetWindowTextW(c, txt, 128);
                    RECT r = { ox, oy, ox + (cr.right - cr.left), oy + (cr.bottom - cr.top) };
                    DrawDarkButton(p->dc, r, txt, false, false, MakeFont(11, FW_NORMAL));
                    return TRUE;
                }
                SetViewportOrgEx(p->dc, ox, oy, nullptr);
                SendMessageW(c, WM_PRINT, (WPARAM)p->dc,
                             PRF_CLIENT | PRF_ERASEBKGND | PRF_NONCLIENT | PRF_CHILDREN);
                SetViewportOrgEx(p->dc, 0, 0, nullptr);
                return TRUE;
            }, (LPARAM)&ctx);
        }
        return 0;
    }
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT* d = (DRAWITEMSTRUCT*)lp;
        if (d->CtlType == ODT_BUTTON) {
            wchar_t txt[128];
            GetWindowTextW(d->hwndItem, txt, 128);
            DrawDarkButton(d->hDC, d->rcItem, txt,
                           g_hoverBtn == d->hwndItem,
                           (d->itemState & ODS_SELECTED) != 0,
                           MakeFont(11, FW_NORMAL));
            return TRUE;
        }
        break;
    }
    case WM_DESTROY:
        KillTimer(hwnd, 1);
        KillTimer(hwnd, 2);
        KillTimer(hwnd, 3);
        StopNet();
        ChatWipe();   // 关闭班级端后清空上一次对话
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void EnableRoundedCorners(HWND h) {
    HMODULE d = LoadLibraryW(L"dwmapi.dll");
    if (!d) return;
    typedef HRESULT(WINAPI* Fn)(HWND, DWORD, LPCVOID, DWORD);
    Fn f = (Fn)GetProcAddress(d, "DwmSetWindowAttribute");
    if (f) {
        int pref = 2;
        f(h, 33, &pref, sizeof pref);
    }
}

static void DoUiSnap() {
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    if (rc.right <= 0 || rc.bottom <= 0 || g_uiSnapPath.empty()) return;
    BITMAPINFO bmi;
    memset(&bmi, 0, sizeof bmi);
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = rc.right;
    bmi.bmiHeader.biHeight = -(int)rc.bottom;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool ok = false;
    if (bmp && bits) {
        HDC memdc = CreateCompatibleDC(nullptr);
        HGDIOBJ old = SelectObject(memdc, bmp);
        SendMessageW(g_hwnd, WM_PRINT, (WPARAM)memdc, PRF_CLIENT | PRF_CHILDREN);
        GdiFlush();
        ok = SavePng32(g_uiSnapPath, (const uint8_t*)bits, rc.right, rc.bottom);
        SelectObject(memdc, old);
        DeleteDC(memdc);
    }
    if (bmp) DeleteObject(bmp);
    HANDLE lg = CreateFileW((ExeDirW() + L"uisnap.log").c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (lg != INVALID_HANDLE_VALUE) {
        std::string msg = "ok=" + std::to_string((int)ok) +
                          " queue=" + std::to_string((int)g_queue.size()) +
                          " head=" + (g_queue.empty() ? "-" : WU8(g_queue.front().teacher)) +
                          " status=" + WU8(g_statusText) + "\n";
        DWORD w = 0;
        WriteFile(lg, msg.data(), (DWORD)msg.size(), &w, nullptr);
        CloseHandle(lg);
    }
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR lpCmdLine, int nShow) {
    g_hInst = hInst;
    std::wstring cmd(lpCmdLine);
    size_t p = cmd.find(L"--ui=");
    if (p != std::wstring::npos) {
        std::wstring val = cmd.substr(p + 5);
        size_t c = val.find(L',');
        if (c != std::wstring::npos) {
            g_uiDelay = _wtoi(val.substr(c + 1).c_str());
            val = val.substr(0, c);
        }
        g_uiSnapPath = val;
        g_forceDpi = 96;
    }
    SetProcessDPIAware();
    {
        HDC ddc = GetDC(nullptr);
        g_dpi = g_forceDpi > 0 ? g_forceDpi : GetDeviceCaps(ddc, LOGPIXELSY);
        ReleaseDC(nullptr, ddc);
    }
    EcNetStart();
    INITCOMMONCONTROLSEX ice;
    ice.dwSize = sizeof ice;
    ice.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&ice);
    GdiplusStartupInput gsi;
    ULONG_PTR gdiToken = 0;
    GdiplusStartup(&gdiToken, &gsi, nullptr);

    g_mode = IniGet(L"board", L"mode", L"lan");
    g_base = IniGet(L"board", L"base", EC_DEFAULT_RELAY);
    if (!g_base.empty() && g_base.back() != L'/') g_base += L'/';
    g_room = IniGet(L"board", L"room", L"101");
    g_port = _wtoi(IniGet(L"board", L"port", L"25800").c_str());
    if (g_port <= 0 || g_port > 65535) g_port = EC_TCP_PORT;
    g_title = IniGet(L"board", L"title", L"叫号");

    WNDCLASSW wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = BoardProc;
    wc.hInstance = hInst;
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"EasyCallBoardWnd";
    RegisterClassW(&wc);

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = DlgProc;
    wc.hInstance = hInst;
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"EasyCallBoardDlg";
    RegisterClassW(&wc);

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = ChatProc;
    wc.hInstance = hInst;
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"EasyCallChatWnd";
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(0, L"EasyCallBoardWnd", L"EasyCall 班级大屏",
                                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                CW_USEDEFAULT, CW_USEDEFAULT, S(1100), S(680),
                                nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;
    EnableRoundedCorners(hwnd);
    ShowWindow(hwnd, SW_MAXIMIZE);
    UpdateWindow(hwnd);

    g_chatSender = BoardSendChat;

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    EcNetStop();
    GdiplusShutdown(gdiToken);
    return 0;
}
