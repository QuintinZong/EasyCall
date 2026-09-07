// EasyCall 班级叫号系统 - 班级大屏端
#define _WIN32_IE 0x0600
#include "ec_common.h"
#include <windows.h>
#include <commctrl.h>
#include <winsock2.h>

// ================= WinToast 集成(可选) =================
// 把 wintoastlib.h / wintoastlib.cpp 放入 src\ 目录后, build.bat 会自动加 -DHAVE_WINTOAST 启用
// 注意: WinRT/WRL 头必须先于 GDI+ 头包含, 避免 Color/byte 等符号冲突
#ifdef HAVE_WINTOAST
#include "wintoastlib.h"
#include <shlobj.h>
#include <propsys.h>
#include <objbase.h>
#endif

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

// 自定义消息: 网络线程把事件投递到主线程处理
#define WM_APP_NEWCALL (WM_APP + 1)   // 收到新的叫号
#define WM_APP_CLEAR   (WM_APP + 2)   // 收到清屏指令
#define WM_APP_STATUS  (WM_APP + 3)   // 网络状态变化(wp=1在线, 0离线; lp=新状态文本)
#define WM_APP_BLACK   (WM_APP + 4)   // 收到黑屏指令
#define WM_APP_CHAT    (WM_APP + 5)   // 收到对话消息
#define WM_APP_TRAY    (WM_APP + 11)  // 托盘图标回调消息(lParam 低字为鼠标事件)

// ---------------- 次级窗口 Fluent 配色(暗色) ----------------
// 设置对话框/对话窗口统一暗色主题, 与大屏深蓝底一致
static const COLORREF C_DBG    = RGB(13, 27, 48);      // 次级窗口底色(与大屏一致)
static const COLORREF C_DEDIT  = RGB(26, 38, 58);      // 编辑框底色
static const COLORREF C_DLIST  = RGB(23, 33, 50);      // 下拉列表底色
static const COLORREF C_DTEXT  = RGB(0xEA, 0xEA, 0xEA);   // 输入文字色
static const COLORREF C_DTEXT2 = RGB(0xC9, 0xD6, 0xE3);   // 标签文字色

// 前置声明: 自绘按钮绘制/钩子函数(定义在文件后部, 对话窗口先引用)
static void SubmitButton(HWND h);
static void DrawDarkButton(HDC dc, const RECT& rc, const wchar_t* text,
                           bool hover, bool pressed, HFONT font);
static HFONT MakeFont(int pt, int weight);

// 单个被叫学生: 学号/姓名/班级
struct CallItem { std::wstring id, name, cls; };
// 一次叫号: 集合地点 + 教师名 + 学生列表(可多人)
struct QCall { std::wstring place, teacher; std::vector<CallItem> items; };

// 全部控件 ID(枚举, 无符号整型)
enum : INT_PTR { IDC_BTN_SETTINGS = 100, IDC_BTN_BLACK, IDC_BTN_CHAT, IDC_BTN_CLEAR,
       IDC_ED_MODE, IDC_ED_BASE, IDC_ED_ROOM, IDC_ED_PORT, IDC_ED_TITLE,   // 设置对话框控件
       IDC_CK_AUTOSTART,                                                    // 开机自启动复选框
       IDC_BTN_OK, IDC_BTN_CANCEL,
       IDC_CHAT_LOG = 200, IDC_CHAT_INPUT, IDC_CHAT_SEND };                // 对话窗口控件

// ---------------- 全局变量 ----------------
static HINSTANCE g_hInst;                                          // 应用实例句柄
static HWND g_hwnd, g_btnSettings, g_btnBlack, g_btnChat, g_btnClear;   // 主窗口及4个按钮
static HWND g_dlg = nullptr, g_dlgMode, g_dlgBase, g_dlgRoom, g_dlgPort, g_dlgTitle;   // 设置对话框控件
static HWND g_chatWnd = nullptr, g_chatLog = nullptr, g_chatInput = nullptr;   // 对话窗口及控件
static std::deque<QCall> g_queue;                 // 叫号队列(按入队顺序依次显示)
static int g_advanceLeft = 20;                    // 当前叫号剩余显示秒数(约20秒轮换)
static std::vector<std::wstring> g_history;       // 历史记录行(底部小字)
static std::vector<std::wstring> g_chatMsgs;      // 对话记录 "HH:MM:SS 姓名: 内容"
static std::vector<std::string> g_sentChatIds;    // 已发送的聊天ID(中转回显去重)
static std::wstring g_lastCallId;                 // 最近一次叫号ID(重复 CALL 去重)
static std::wstring g_statusText = L"正在启动…";  // 顶部右侧状态文字
static std::wstring g_title = L"叫号";            // 大屏标题(设置里可改)
static std::wstring g_base = EC_DEFAULT_RELAY;    // 中转服务器基地址
static std::wstring g_room = L"101";              // 房间号
static std::wstring g_mode = L"lan";              // 运行模式: "lan" 直连 / "relay" 中转
static int g_port = EC_TCP_PORT;                  // 局域网监听端口
static int g_dpi = 96;                            // 屏幕 DPI(界面缩放基准)
static bool g_online = false;                     // 网络是否在线(状态灯色)
static bool g_black = false;                      // 是否黑屏(显示"保持安静")
static bool g_callHidden = false;   // 本机一键清屏: 仅隐藏当前叫号显示, 不清除队列
static int g_flash = 0;                            // 闪烁剩余次数(新叫号时背景闪几下)
static std::atomic<bool> g_stop{false};            // 后台线程退出标志
static HICON g_trayIcon = nullptr;                 // 托盘图标句柄(运行时绘制)
static NOTIFYICONDATAW g_nid{};                    // 托盘图标数据结构
static SOCKET g_listen = INVALID_SOCKET;           // 局域网监听套接字
static SOCKET g_client = INVALID_SOCKET;           // 当前教师端连接
static std::thread g_threadNet, g_threadBroad, g_threadPresence;   // 网络主线程/UDP广播线程/心跳线程
static HWND g_hoverBtn = nullptr;                  // 当前悬停的自绘按钮
static std::wstring g_uiSnapPath;                  // --ui 自截图输出路径
static int g_forceDpi = 0;                         // --ui 调试时强制 96 DPI
static int g_uiDelay = 0;   // --ui 截图延迟(毫秒), 0=默认1500
static bool (*g_chatSender)(const std::wstring&) = nullptr;   // 对话发送函数指针(BoardSendChat)
static void DoUiSnap();
static void HandlePayload(const std::string& payload, bool fromTcp, SOCKET replySock);
static void EnsureChatWindow(bool focus = true);
static bool AutoStartSet(bool enable);   // 注册/注销开机自启动(定义见后文)

// 功能: DPI 缩放: 设计稿 96DPI 下的像素值 -> 当前 DPI 下的实际像素
// 参数: px 设计稿像素值
// 返回: 缩放后的像素值
static int S(int px) { return MulDiv(px, g_dpi, 96); }

// 功能: 按 字号(磅)/字重 创建 Segoe UI 字体
// 参数: pt 字号(磅); weight 字重(FW_NORMAL/FW_BOLD 等)
// 返回: 字体句柄(用完需 DeleteObject)
static HFONT MakeFont(int pt, int weight) {
    return CreateFontW(-MulDiv(pt, g_dpi, 72), 0, 0, 0, weight, 0, 0, 0,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
}
// 功能: 次级窗口(设置/对话)通用控件上色: kind 0=静态标签 1=编辑框 2=下拉列表
// 参数: dc 控件设备上下文; kind 控件种类
// 返回: 背景画刷(交回系统填充控件背景)
static LRESULT DarkCtlColor(HDC dc, int kind) {
    if (kind == 0) {                       // 静态标签: 次要色+透明底
        SetTextColor(dc, C_DTEXT2);
        SetBkMode(dc, TRANSPARENT);
    } else {                               // 编辑框/下拉列表: 深底浅字
        SetTextColor(dc, C_DTEXT);
        SetBkMode(dc, OPAQUE);
        SetBkColor(dc, kind == 1 ? C_DEDIT : C_DLIST);
    }
    static HBRUSH brBg = CreateSolidBrush(C_DBG);
    static HBRUSH brEd = CreateSolidBrush(C_DEDIT);
    static HBRUSH brLs = CreateSolidBrush(C_DLIST);
    return (LRESULT)(kind == 0 ? brBg : kind == 1 ? brEd : brLs);
}
// 功能: 更新状态文字与在线标志(网络线程通过 WM_APP_STATUS 调用)
// 参数: t 状态文字; online 是否在线(影响顶部状态点颜色)
// 返回: 无
static void SetStatus(const std::wstring& t, bool online) {
    g_statusText = t;
    g_online = online;
}
// 功能: 拼一个学生的显示行: "学号   姓名　(班级)"
// 参数: it 学生数据
// 返回: 显示文本
static std::wstring MakeLine(const CallItem& it) {
    std::wstring s;
    if (!it.id.empty()) s += it.id + L"   ";
    s += it.name;
    if (!it.cls.empty()) s += L"　(" + it.cls + L")";   // 全角空格+括号显示班级
    return s;
}

// ---------------- 对话 ----------------
// 功能: 对话记录文件路径(EXE目录\chat_board.json)
// 参数: 无
// 返回: 文件完整路径
static std::wstring ChatFile() { return ExeDirW() + L"chat_board.json"; }
// 功能: 把内存中的对话记录保存为 JSON 数组文件 [{"t":"..."},...]
// 参数: 无
// 返回: 无
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
// 功能: 从 chat_board.json 载入历史对话(文件不存在则留空)
// 参数: 无
// 返回: 无
static void ChatLoad() {
    g_chatMsgs.clear();
    HANDLE h = CreateFileW(ChatFile().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD hi = 0;
    DWORD size = GetFileSize(h, &hi);
    if (hi || size == 0 || size > 4 * 1024 * 1024) { CloseHandle(h); return; }   // 上限4MB防异常
    std::string json(size, 0);
    DWORD got = 0;
    if (!ReadFile(h, &json[0], size, &got, nullptr) || got != size) { CloseHandle(h); return; }
    CloseHandle(h);
    size_t pos = 0;
    for (;;) {
        // 逐个提取 {...} 对象, 取字段 t
        size_t a = json.find('{', pos);
        if (a == std::string::npos) break;
        size_t b = json.find('}', a);
        if (b == std::string::npos) break;
        std::wstring t = U8W(JsonStrVal(json.substr(a, b - a + 1), "t"));
        if (!t.empty()) g_chatMsgs.push_back(t);
        pos = b + 1;
    }
}
// 功能: 删除对话记录文件并清空内存(大屏端关闭时调用)
// 参数: 无
// 返回: 无
static void ChatWipe() {
    DeleteFileW(ChatFile().c_str());
    g_chatMsgs.clear();
}
// 功能: 向对话窗口的日志框追加一行
// 参数: line 单行文本(不带换行)
// 返回: 无
static void ChatLogAppend(const std::wstring& line) {
    if (!g_chatLog) return;
    int len = GetWindowTextLengthW(g_chatLog);
    SendMessageW(g_chatLog, EM_SETSEL, len, len);   // 光标移到末尾
    std::wstring out = line + L"\r\n";
    SendMessageW(g_chatLog, EM_REPLACESEL, 0, (LPARAM)out.c_str());   // 末尾追加并自动滚动
}
// 功能: 追加一条对话(带时间前缀), 更新界面并持久化
// 参数: sender 发送者显示名; text 消息内容
// 返回: 无
static void ChatAppend(const std::wstring& sender, const std::wstring& text) {
    g_chatMsgs.push_back(NowTimeW() + L" " + sender + L": " + text);
    while (g_chatMsgs.size() > 500) g_chatMsgs.erase(g_chatMsgs.begin());   // 最多保留500条
    ChatLogAppend(g_chatMsgs.back());
    ChatSave();
}
// 功能: 处理收到的 CHAT 帧(来自网络线程投递的消息)
// CHAT 报文格式: CHAT\n<消息ID>\n<发送者>\n<内容...>(内容可多行, 以 \n 拼接)
// 参数: frame 完整报文(UTF-8)
// 返回: 无
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
// 功能: 大屏端发送一条对话: 构造 CHAT 报文(署名"教室") -> 按当前模式发送
// 参数: text 消息文本
// 返回: true=发送成功; false=文本为空或发送失败
static bool BoardSendChat(const std::wstring& text) {
    std::wstring t = text;
    {
        size_t a = t.find_first_not_of(L" \t\r\n");   // 手动去首尾空白
        if (a == std::wstring::npos) return false;
        size_t b = t.find_last_not_of(L" \t\r\n");
        t = t.substr(a, b - a + 1);
    }
    if (t.empty()) return false;
    std::string msgId = NowStampMs();   // 消息ID = 毫秒时间戳
    g_sentChatIds.push_back(msgId);
    while (g_sentChatIds.size() > 64) g_sentChatIds.erase(g_sentChatIds.begin());   // 只保留最近64个
    std::string payload = "CHAT\n" + msgId + "\n" + WU8(L"教室") + "\n" + WU8(t);   // 大屏端对话署名为"教室"
    bool ok = false;
    if (g_mode == L"relay") {
        // 中转模式: POST 到服务器 push.php
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
        // 直连模式: 经已连接的教师端 TCP 发帧
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
// 功能: 对话输入框子类过程: 回车即触发[发送]按钮
// 参数: h 输入框句柄; m 消息; w/l 消息参数
// 返回: 处理则返回0; 否则交回原窗口过程
static LRESULT CALLBACK ChatInputProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_KEYDOWN && w == VK_RETURN) {
        PostMessageW(GetParent(h), WM_COMMAND, MAKEWPARAM(IDC_CHAT_SEND, BN_CLICKED), (LPARAM)h);
        return 0;
    }
    return CallWindowProcW((WNDPROC)GetPropW(h, L"EcOrigProc"), h, m, w, l);   // 其余消息走原过程
}
// 功能: 对话窗口过程: 创建日志框/输入框/发送按钮, 处理缩放与发送
// 参数: hwnd 窗口句柄; msg 消息; wp/lp 消息参数
// 返回: 按 Win32 窗口过程约定
static LRESULT CALLBACK ChatProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        // 只读多行日志框
        g_chatLog = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                    WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE |
                                        ES_READONLY | ES_AUTOVSCROLL,
                                    0, 0, 10, 10, hwnd, (HMENU)IDC_CHAT_LOG, g_hInst, nullptr);
        // 单行输入框
        g_chatInput = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                      WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                      0, 0, 10, 10, hwnd, (HMENU)IDC_CHAT_INPUT, g_hInst, nullptr);
        // 子类化输入框: 保存原窗口过程, 替换为 ChatInputProc(回车发送)
        WNDPROC orig = (WNDPROC)GetWindowLongPtrW(g_chatInput, GWLP_WNDPROC);
        SetPropW(g_chatInput, L"EcOrigProc", (HANDLE)orig);
        SetWindowLongPtrW(g_chatInput, GWLP_WNDPROC, (LONG_PTR)ChatInputProc);
        // [发送]按钮(自绘 Fluent 暗色风格)
        {
            HWND b = CreateWindowExW(0, L"BUTTON", L"发送",
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                     0, 0, 10, 10, hwnd, (HMENU)IDC_CHAT_SEND, g_hInst, nullptr);
            SubmitButton(b);   // 挂悬停跟踪钩子
            HFONT f = MakeFont(11, FW_NORMAL);
            SendMessageW(g_chatLog, WM_SETFONT, (WPARAM)f, TRUE);
            SendMessageW(g_chatInput, WM_SETFONT, (WPARAM)f, TRUE);
            SendMessageW(b, WM_SETFONT, (WPARAM)f, TRUE);
        }
        for (auto& line : g_chatMsgs) ChatLogAppend(line);   // 载入历史对话
        return 0;
    }
    case WM_SIZE: {
        // 手动布局: 上日志, 下输入框+发送按钮
        RECT rc;
        GetClientRect(hwnd, &rc);
        int hh = S(30);
        MoveWindow(g_chatLog, S(8), S(8), rc.right - S(16), rc.bottom - S(52) - hh, TRUE);
        MoveWindow(g_chatInput, S(8), rc.bottom - hh - S(12), rc.right - S(96), hh, TRUE);
        MoveWindow(GetDlgItem(hwnd, IDC_CHAT_SEND), rc.right - S(80), rc.bottom - hh - S(13), S(72), hh + 2, TRUE);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;   // 背景统一在 WM_PAINT 里画, 防闪烁
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HBRUSH br = CreateSolidBrush(C_DBG);   // 统一深蓝背景
        FillRect(dc, &rc, br);
        DeleteObject(br);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CTLCOLORSTATIC:   // 静态标签
        return DarkCtlColor((HDC)wp, 0);
    case WM_CTLCOLORBTN:      // 按钮底色(自绘按钮不影响)
        return DarkCtlColor((HDC)wp, 0);
    case WM_CTLCOLOREDIT:     // 日志/输入编辑框: 深底浅字
        return DarkCtlColor((HDC)wp, 1);
    case WM_DRAWITEM: {
        // 自绘按钮绘制入口([发送]按钮, 固定深蓝底不受黑屏状态影响)
        DRAWITEMSTRUCT* d = (DRAWITEMSTRUCT*)lp;
        if (d->CtlType == ODT_BUTTON) {
            wchar_t txt[128];
            GetWindowTextW(d->hwndItem, txt, 128);
            bool ob = g_black;
            g_black = false;
            DrawDarkButton(d->hDC, d->rcItem, txt,
                           g_hoverBtn == d->hwndItem,
                           (d->itemState & ODS_SELECTED) != 0,
                           MakeFont(11, FW_NORMAL));
            g_black = ob;
            return TRUE;
        }
        break;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_CHAT_SEND) {
            if (g_chatSender) {
                std::wstring t;
                int n = GetWindowTextLengthW(g_chatInput);
                t.resize(n);
                if (n) GetWindowTextW(g_chatInput, &t[0], n + 1);
                if (!t.empty()) {
                    g_chatSender(t);            // 调用 BoardSendChat
                    SetWindowTextW(g_chatInput, L"");   // 清空输入框
                    SetFocus(g_chatInput);
                }
            }
            return 0;
        }
        break;
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);   // 点关闭只隐藏不销毁(可再次打开)
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
// 功能: 确保对话窗口存在并显示; 不存在则创建
// 参数: focus true=创建/显示后置前台; false=只显示不抢焦点
// 返回: 无
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
// ------------------------------------------------------------------
// 收到的报文格式(与教师端/ec_net 对应):
//   CALL\n<callId>\n<地点>\n<教师>\n<学生行>\n...   叫号, 学生行="学号\t姓名\t班级"
//   CHAT\n<消息ID>\n<发送者>\n<内容...>              对话(内容可多行)
//   CLEAR                                          清屏(清队列与历史)
//   BLACK                                          黑屏
//   PING (直连)                                    心跳, 需回 PONG
// ------------------------------------------------------------------
// 功能: 解析并分发一个完整报文(局域网与中转共用入口)
// 参数: payload 报文内容(UTF-8); fromTcp 是否来自 TCP 直连(影响 PING 应答);
//       replySock TCP 模式下用来回 PONG 的套接字(中转模式传 INVALID_SOCKET)
// 返回: 无
static void HandlePayload(const std::string& payload, bool fromTcp, SOCKET replySock) {
    std::vector<std::string> lines = SplitLines(payload);
    if (lines.empty()) return;
    if (lines[0] == "CALL") {
        if (lines.size() < 4) return;
        if (lines[1] == WU8(g_lastCallId)) return;   // 去重
        g_lastCallId = U8W(lines[1]);
        QCall q;
        q.place = lines[2].empty() ? L"台前" : U8W(lines[2]);   // 地点
        q.teacher = lines[3].empty() ? L"教师" : U8W(lines[3]); // 教师
        for (size_t i = 4; i < lines.size(); i++) {
            // 每个学生行按 Tab 拆成 学号/姓名/班级
            std::vector<std::string> f = SplitTabs(lines[i]);
            CallItem it;
            if (f.size() > 0) it.id = U8W(f[0]);
            if (f.size() > 1) it.name = U8W(f[1]);
            if (f.size() > 2) it.cls = U8W(f[2]);
            if (!it.id.empty() || !it.name.empty()) q.items.push_back(it);
        }
        if (!q.items.empty()) {
            QCall* m = new QCall(q);   // 堆上分配, 经消息把所有权交给主线程
            PostMessageW(g_hwnd, WM_APP_NEWCALL, 0, (LPARAM)m);
        }
    } else if (lines[0] == "CHAT") {
        PostMessageW(g_hwnd, WM_APP_CHAT, 0, (LPARAM)new std::string(payload));
    } else if (lines[0] == "CLEAR") {
        PostMessageW(g_hwnd, WM_APP_CLEAR, 0, 0);
    } else if (lines[0] == "BLACK") {
        PostMessageW(g_hwnd, WM_APP_BLACK, 0, 0);
    } else if (lines[0] == "PING" && fromTcp && replySock != INVALID_SOCKET) {
        TcpSendFrame(replySock, "PONG");   // 保活应答
    }
}

// ---------------- 局域网模式 ----------------
// 功能: 局域网网络主线程: 监听 TCP 端口, 接受教师端连接后循环收帧并
//       交给 HandlePayload; 连接断开后回到监听; 端口被占用则报错退出
// 参数: 无
// 返回: 无
static void LanThreadProc() {
    for (;;) {
        g_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (g_listen == INVALID_SOCKET) break;
        BOOL b = TRUE;
        setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, (const char*)&b, sizeof b);   // 允许快速重绑端口
        sockaddr_in sa;
        memset(&sa, 0, sizeof sa);
        sa.sin_family = AF_INET;
        sa.sin_port = htons((u_short)g_port);
        sa.sin_addr.s_addr = INADDR_ANY;   // 监听所有网卡
        if (bind(g_listen, (sockaddr*)&sa, sizeof sa) != 0 || listen(g_listen, 4) != 0) {
            // 端口被占用: 提示在设置里换端口
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
            g_client = accept(g_listen, (sockaddr*)&from, &fl);   // 阻塞等教师端
            if (g_client == INVALID_SOCKET) {
                if (g_stop.load()) break;   // 退出标志: accept 被 StopNet 关闭套接字打断
                continue;
            }
            char ip[64] = {0};
            inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
            PostMessageW(g_hwnd, WM_APP_STATUS, 1,
                         (LPARAM)new std::wstring(L"教师端已连接: " + U8W(ip)));
            for (;;) {
                bool ok = false;
                std::string f = TcpRecvFrame(g_client, 40000, &ok);   // 收帧(40秒超时)
                if (!ok) break;   // 断开/超时: 结束本次连接
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
// 功能: UDP 广播线程: 每2秒广播一次在线宣告(供教师端[扫描教室]发现)
// 参数: 无
// 返回: 无
static void BroadThreadProc() {
    while (!g_stop.load()) {
        UdpBroadcastPresence(g_title + L" · 教室大屏", (unsigned short)g_port);
        Sleep(2000);
    }
}

// ---------------- 服务器中转模式 ----------------
// 功能: 中转网络主线程: 循环 GET fetch.php?room=..&after=<seq> 增量拉取
//       新报文(长轮询32秒), 逐个交给 HandlePayload; 失败3秒后重试
// 参数: 无
// 返回: 无
static void RelayThreadProc() {
    std::wstring base = g_base;
    std::string room = WU8(g_room);
    long long after = 0;   // 已消费的消息序号
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
            long long seq = HttpSeqFromHeader(hdr);   // 服务器当前总序号
            if (seq > after) after = seq;
            std::vector<std::string> frames;
            ParseFrames(resp, frames);   // 响应体 = 多个 [4字节大端长度+负载] 帧
            for (auto& fr : frames) HandlePayload(fr, false, INVALID_SOCKET);
        } else {
            PostMessageW(g_hwnd, WM_APP_STATUS, 0,
                         (LPARAM)new std::wstring(L"服务器连接失败, 3秒后重试…"));
            Sleep(3000);
        }
    }
}
// 功能: 心跳线程(仅中转模式): 每8秒 POST presence.php 上报本房间在线
// 参数: 无
// 返回: 无
static void PresenceThreadProc() {
    while (!g_stop.load()) {
        std::string resp;
        std::wstring err;
        std::string body = "room=" + UrlEncode(WU8(g_room));
        HttpPostForm(g_base + L"presence.php", body, resp, err, 6);
        Sleep(8000);
    }
}
// 功能: 按当前模式启动后台网络线程(中转: 拉取+心跳; 直连: 监听+广播)
// 参数: 无
// 返回: 无
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
// 功能: 停止所有网络线程并关闭套接字(退出时调用)
// 参数: 无
// 返回: 无
static void StopNet() {
    g_stop.store(true);
    HttpAbortCurrent();   // 让卡在长轮询的线程尽快退出
    if (g_listen != INVALID_SOCKET) closesocket(g_listen);   // 关闭监听可打断 accept
    if (g_client != INVALID_SOCKET) closesocket(g_client);   // 关闭连接可打断 recv
    if (g_threadNet.joinable()) g_threadNet.join();
    if (g_threadBroad.joinable()) g_threadBroad.join();
    if (g_threadPresence.joinable()) g_threadPresence.join();
}

// ---------------- Fluent 暗色按钮 ----------------
// 功能: 自绘按钮的钩子过程: 跟踪鼠标悬停/离开/按下以刷新绘制状态,
//       其余消息交回原按钮过程
// 参数: h 按钮句柄; m 消息; w/l 消息参数
// 返回: 按 Win32 窗口过程约定
static LRESULT CALLBACK FluentBtnProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_MOUSEMOVE:
        if (g_hoverBtn != h) {
            g_hoverBtn = h;   // 记录悬停按钮(同一时刻只有一个)
            InvalidateRect(h, nullptr, TRUE);
        }
        {
            TRACKMOUSEEVENT tme;
            memset(&tme, 0, sizeof tme);
            tme.cbSize = sizeof tme;
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = h;
            TrackMouseEvent(&tme);   // 申请 WM_MOUSELEAVE 通知
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
        InvalidateRect(h, nullptr, TRUE);   // 按下/抬起重绘
        break;
    }
    return CallWindowProcW((WNDPROC)GetPropW(h, L"EcOrigProc"), h, m, w, l);
}
// 功能: 给按钮挂自绘钩子: 保存原窗口过程到属性, 换成 FluentBtnProc
// 参数: h 按钮句柄
// 返回: 无
static void SubmitButton(HWND h) {
    WNDPROC orig = (WNDPROC)GetWindowLongPtrW(h, GWLP_WNDPROC);
    SetPropW(h, L"EcOrigProc", (HANDLE)orig);
    SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)FluentBtnProc);
}
// 功能: COLORREF -> Gdiplus Color(恒不透明)
// 参数: c 颜色值(0x00BBGGRR)
// 返回: 对应 GDI+ 颜色
static Gdiplus::Color FluentColor(COLORREF c) {
    return Gdiplus::Color(255, GetRValue(c), GetGValue(c), GetBValue(c));
}
// 功能: 绘制暗色风格自绘按钮: 背景填窗底色 -> 圆角深灰底 -> 边框 -> 居中文字
// 参数: dc 设备上下文; rc 按钮区域; text 按钮文字;
//       hover 悬停态; pressed 按下态; font 文字字体
// 返回: 无
static void DrawDarkButton(HDC dc, const RECT& rc, const wchar_t* text,
                           bool hover, bool pressed, HFONT font) {
    Graphics g(dc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);   // 抗锯齿
    {
        // 背景: 黑屏时为纯黑, 否则与大屏底色一致
        SolidBrush bg(FluentColor(g_black ? RGB(0, 0, 0) : RGB(13, 27, 48)));
        g.FillRectangle(&bg, (INT)rc.left, (INT)rc.top,
                        (INT)(rc.right - rc.left), (INT)(rc.bottom - rc.top));
    }
    int rad = 4;   // 圆角半径
    int w = rc.right - rc.left - 1, h = rc.bottom - rc.top - 1;
    int d = rad * 2;
    GraphicsPath path;   // 四个角各一段90度圆弧拼出圆角矩形
    path.AddArc(rc.left, rc.top, d, d, 180, 90);                // 左上角
    path.AddArc(rc.left + w - d, rc.top, d, d, 270, 90);        // 右上角
    path.AddArc(rc.left + w - d, rc.top + h - d, d, d, 0, 90);  // 右下角
    path.AddArc(rc.left, rc.top + h - d, d, d, 90, 90);         // 左下角
    path.CloseFigure();
    // 按悬停/按下选择深灰底色与边框色
    SolidBrush fill(FluentColor(pressed ? RGB(22, 22, 22) : hover ? RGB(48, 48, 48) : RGB(36, 36, 36)));
    Pen pen(FluentColor(pressed ? RGB(70, 70, 70) : hover ? RGB(96, 96, 96) : RGB(78, 78, 78)), 1.0f);
    g.FillPath(&fill, &path);
    g.DrawPath(&pen, &path);
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ of = SelectObject(dc, font);
    SetTextColor(dc, RGB(0xEA, 0xEA, 0xEA));   // 浅色文字
    RECT r = rc;
    DrawTextW(dc, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, of);
}

// ---------------- 绘制 ----------------
// 功能: 大屏主界面全部绘制(黑屏页 / 顶栏 / 叫号内容 / 历史记录)
// 黑屏页: 纯黑底 + "保持安静"/"请安静自习"
// 正常页: 深蓝底 -> 顶部标题栏(标题+状态) -> 叫号(自动缩小字号
//          以适应窗口宽度) 或 "等待叫号…" -> 底部历史小字
// 参数: dc 设备上下文; rc 客户区矩形
// 返回: 无
static void PaintDraw(HDC dc, const RECT& rc) {
    if (g_black) {
        // ---- 黑屏状态 ----
        HBRUSH br = CreateSolidBrush(RGB(0, 0, 0));
        FillRect(dc, &rc, br);
        DeleteObject(br);
        SetBkMode(dc, TRANSPARENT);
        int cy = rc.bottom / 2;   // 垂直中线
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

    bool flashOn = (g_flash > 0) && ((g_flash & 1) == 1);   // 新叫号闪烁: 奇数拍亮色
    COLORREF bg = flashOn ? RGB(58, 88, 132) : RGB(13, 27, 48);   // 主体深蓝底
    HBRUSH br = CreateSolidBrush(bg);
    FillRect(dc, &rc, br);
    DeleteObject(br);

    int topH = S(64);   // 顶部栏高度
    RECT tr = { 0, 0, rc.right, topH };
    br = CreateSolidBrush(RGB(8, 18, 34));   // 顶部栏更深一档
    FillRect(dc, &tr, br);
    DeleteObject(br);
    SetBkMode(dc, TRANSPARENT);

    // 左: 大屏标题
    HFONT fTitle = MakeFont(26, FW_BOLD);
    SelectObject(dc, fTitle);
    SetTextColor(dc, RGB(255, 255, 255));
    RECT ttl = { S(24), 0, rc.right - S(360), topH };
    DrawTextW(dc, (g_title + L" · EasyCall").c_str(), -1, &ttl, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DeleteObject(fTitle);

    // 右: 网络状态(在线绿 / 离线红)
    HFONT fSt = MakeFont(11, FW_NORMAL);
    SelectObject(dc, fSt);
    SetTextColor(dc, g_online ? RGB(110, 220, 130) : RGB(235, 120, 110));
    RECT st = { S(24), 0, rc.right - S(24), topH };
    DrawTextW(dc, g_statusText.c_str(), -1, &st, DT_RIGHT | DT_VCENTER | DT_END_ELLIPSIS | DT_SINGLELINE);
    DeleteObject(fSt);

    int mainTop = topH + S(26);   // 主内容区上缘
    if (!g_queue.empty() && !g_callHidden) {
        // ---- 有叫号: 显示队首 ----
        const QCall& q = g_queue.front();
        HFONT fHead = MakeFont(20, FW_BOLD);
        SelectObject(dc, fHead);
        SetTextColor(dc, RGB(255, 214, 90));   // 金色提示行
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

        // 按人数预选字号: 人越多字越小
        int pts = 46;
        if (q.items.size() > 8) pts = 40;
        if (q.items.size() > 14) pts = 32;
        if (q.items.size() > 20) pts = 26;
        if (q.items.size() > 28) pts = 20;
        int maxW = rc.right - S(80);   // 允许的最大文字宽度
        HFONT fBig = nullptr;
        for (;;) {
            // 逐次缩小字号直到最宽行能放下
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
            if (widest <= maxW || pts <= 16) break;   // 放得下或已到最小字号
            pts -= 2;
        }
        HGDIOBJ oldF = SelectObject(dc, fBig);
        SetTextColor(dc, RGB(245, 245, 245));
        SetTextAlign(dc, TA_CENTER | TA_TOP);   // 居中逐行输出学生名
        int cx = rc.right / 2;
        int lineH = MulDiv(pts, g_dpi, 72) + S(18);   // 行高 = 字号+间距
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
        // ---- 空闲: 等待叫号 ----
        HFONT fIdle = MakeFont(24, FW_NORMAL);
        SelectObject(dc, fIdle);
        SetTextColor(dc, RGB(110, 130, 160));
        RECT ir = { S(40), mainTop + S(60), rc.right - S(40), mainTop + S(160) };
        DrawTextW(dc, L"等待叫号…", -1, &ir, DT_CENTER | DT_VCENTER);
        DeleteObject(fIdle);
    }

    if (!g_history.empty()) {
        // ---- 底部历史记录(从下往上排) ----
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
// 功能: WM_PAINT 处理: BeginPaint -> PaintDraw -> EndPaint
// 参数: hwnd 窗口句柄
// 返回: 无
static void Paint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    PaintDraw(dc, rc);
    EndPaint(hwnd, &ps);
}

// ---------------- 设置对话框 ----------------
// 功能: 设置对话框内部布局(手动摆放各控件)
// 参数: 无
// 返回: 无
static void DlgLayout() {
    int x = S(16), w = S(300);
    auto move = [&](int id, int yy, int hh) { MoveWindow(GetDlgItem(g_dlg, id), x, yy, w, hh, TRUE); };
    move(IDC_ED_MODE, S(38), S(120));   // 模式下拉框(高度120为下拉部分)
    int y = S(66);
    move(IDC_ED_BASE, y + S(22), S(24)); y += S(54);    // 服务器地址
    move(IDC_ED_ROOM, y + S(22), S(24)); y += S(54);    // 房间号
    move(IDC_ED_PORT, y + S(22), S(24)); y += S(54);    // 端口
    move(IDC_ED_TITLE, y + S(22), S(24)); y += S(54);   // 标题
    MoveWindow(GetDlgItem(g_dlg, IDC_CK_AUTOSTART), x, S(278), w, S(22), TRUE);   // 自启动开关
    MoveWindow(GetDlgItem(g_dlg, IDC_BTN_OK), x, S(304), S(120), S(32), TRUE);      // [保存]
    MoveWindow(GetDlgItem(g_dlg, IDC_BTN_CANCEL), x + S(160), S(304), S(120), S(32), TRUE);   // [取消]
}
// 功能: 设置对话框过程: 编辑模式/服务器/房间/端口/标题, [保存]写 INI
// 参数: hwnd 窗口句柄; msg 消息; wp/lp 消息参数
// 返回: 按 Win32 窗口过程约定
static LRESULT CALLBACK DlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_dlg = hwnd;
        auto mkLabel = [&](const wchar_t* t, int yy) {   // 便捷 lambda: 建标签(Fluent 次要色+统一字体)
            HWND lbl = CreateWindowExW(0, L"STATIC", t, WS_CHILD | WS_VISIBLE, S(16), yy, S(300), S(20),
                                       hwnd, nullptr, g_hInst, nullptr);
            SendMessageW(lbl, WM_SETFONT, (WPARAM)MakeFont(11, FW_NORMAL), TRUE);
        };
        auto mkEdit = [&](const wchar_t* t, INT_PTR id, DWORD style, int yy, int hh) {   // 建编辑框
            HWND e = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", t,
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | style,
                                     S(16), yy, S(300), hh, hwnd, (HMENU)id, g_hInst, nullptr);
            SendMessageW(e, WM_SETFONT, (WPARAM)MakeFont(11, FW_NORMAL), TRUE);
            return e;
        };
        mkLabel(L"运行模式:", S(18));
        // 模式下拉框(两项, 不可编辑)
        g_dlgMode = CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", L"",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
                                    S(16), S(38), S(300), S(300), hwnd,
                                    (HMENU)IDC_ED_MODE, g_hInst, nullptr);
        SendMessageW(g_dlgMode, WM_SETFONT, (WPARAM)MakeFont(11, FW_NORMAL), TRUE);   // 统一字体
        SendMessageW(g_dlgMode, CB_ADDSTRING, 0, (LPARAM)L"局域网直连(同一网络)");
        SendMessageW(g_dlgMode, CB_ADDSTRING, 0, (LPARAM)L"服务器中转(跨网络)");
        SendMessageW(g_dlgMode, CB_SETCURSEL, g_mode == L"relay" ? 1 : 0, 0);   // 回显当前模式
        mkLabel(L"中转服务器地址:", S(66));
        g_dlgBase = mkEdit(g_base.c_str(), IDC_ED_BASE, ES_AUTOHSCROLL, S(88), S(24));
        mkLabel(L"房间号(两端一致):", S(120));
        g_dlgRoom = mkEdit(g_room.c_str(), IDC_ED_ROOM, ES_AUTOHSCROLL, S(142), S(24));
        mkLabel(L"局域网监听端口:", S(174));
        g_dlgPort = mkEdit(std::to_wstring(g_port).c_str(), IDC_ED_PORT, ES_AUTOHSCROLL | ES_NUMBER, S(196), S(24));
        mkLabel(L"大屏标题:", S(228));
        g_dlgTitle = mkEdit(g_title.c_str(), IDC_ED_TITLE, ES_AUTOHSCROLL, S(250), S(24));
        // 开机自启动开关(回显当前设置)
        {
            HWND ck = CreateWindowExW(0, L"BUTTON", L"开机自动启动(大屏)",
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                      S(16), S(278), S(300), S(22), hwnd,
                                      (HMENU)IDC_CK_AUTOSTART, g_hInst, nullptr);
            SendMessageW(ck, WM_SETFONT, (WPARAM)MakeFont(11, FW_NORMAL), TRUE);
            SendMessageW(ck, BM_SETCHECK,
                         IniGet(L"board", L"autostart", L"1") == L"1" ? BST_CHECKED : BST_UNCHECKED, 0);
        }
        // [保存]/[取消]按钮(自绘 Fluent 暗色风格)
        {
            HWND b = CreateWindowExW(0, L"BUTTON", L"保存",
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                     S(16), S(304), S(120), S(32), hwnd, (HMENU)IDC_BTN_OK,
                                     g_hInst, nullptr);
            SubmitButton(b);
            SendMessageW(b, WM_SETFONT, (WPARAM)MakeFont(11, FW_NORMAL), TRUE);
        }
        {
            HWND b = CreateWindowExW(0, L"BUTTON", L"取消",
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                     S(176), S(304), S(120), S(32), hwnd, (HMENU)IDC_BTN_CANCEL,
                                     g_hInst, nullptr);
            SubmitButton(b);
            SendMessageW(b, WM_SETFONT, (WPARAM)MakeFont(11, FW_NORMAL), TRUE);
        }
        DlgLayout();
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;   // 背景统一在 WM_PAINT 里画, 防闪烁
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HBRUSH br = CreateSolidBrush(C_DBG);   // 统一深蓝背景
        FillRect(dc, &rc, br);
        DeleteObject(br);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CTLCOLORSTATIC:   // 静态标签(含下拉框静态区)
        return DarkCtlColor((HDC)wp, 0);
    case WM_CTLCOLORBTN:      // 复选框底色(自绘按钮不影响)
        return DarkCtlColor((HDC)wp, 0);
    case WM_CTLCOLOREDIT:     // 各编辑框: 深底浅字
        return DarkCtlColor((HDC)wp, 1);
    case WM_CTLCOLORLISTBOX:  // 下拉列表: 深底浅字
        return DarkCtlColor((HDC)wp, 2);
    case WM_DRAWITEM: {
        // 自绘按钮绘制入口([保存]/[取消], 固定深蓝底不受黑屏状态影响)
        DRAWITEMSTRUCT* d = (DRAWITEMSTRUCT*)lp;
        if (d->CtlType == ODT_BUTTON) {
            wchar_t txt[128];
            GetWindowTextW(d->hwndItem, txt, 128);
            bool ob = g_black;
            g_black = false;
            DrawDarkButton(d->hDC, d->rcItem, txt,
                           g_hoverBtn == d->hwndItem,
                           (d->itemState & ODS_SELECTED) != 0,
                           MakeFont(11, FW_NORMAL));
            g_black = ob;
            return TRUE;
        }
        break;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_BTN_OK) {
            // 读取各编辑框 -> 校验端口 -> 全部写 INI(board节) -> 提示重启生效
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
            if (prt <= 0 || prt > 65535) prt = EC_TCP_PORT;   // 端口非法用默认值
            IniSet(L"board", L"mode", mode);
            IniSet(L"board", L"base", base);
            IniSet(L"board", L"room", room);
            IniSet(L"board", L"port", std::to_wstring(prt));
            IniSet(L"board", L"title", title);
            // 自启动开关: 写 INI + 立即同步注册表
            {
                bool as = SendMessageW(GetDlgItem(hwnd, IDC_CK_AUTOSTART), BM_GETCHECK, 0, 0) == BST_CHECKED;
                IniSet(L"board", L"autostart", as ? L"1" : L"0");
                AutoStartSet(as);
            }
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
        // 关闭设置后恢复主窗口可用
        g_dlg = nullptr;
        EnableWindow(g_hwnd, TRUE);
        SetForegroundWindow(g_hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
// 功能: 打开设置对话框(模态: 禁用主窗口, 独立消息循环直到关闭)
// 参数: 无
// 返回: 无
static void ShowSettings() {
    EnableWindow(g_hwnd, FALSE);   // 禁用主窗口形成模态
    RECT rc;
    GetWindowRect(g_hwnd, &rc);
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"EasyCallBoardDlg", L"大屏设置",
                               WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                               rc.left + (rc.right - rc.left) / 2 - S(190),   // 主窗口居中
                               rc.top + (rc.bottom - rc.top) / 2 - S(200),
                               S(380), S(420), g_hwnd, nullptr, g_hInst, nullptr);
    if (!dlg) { EnableWindow(g_hwnd, TRUE); return; }
    MSG m;
    while (IsWindow(dlg)) {
        // 独立消息循环: 直到设置对话框销毁
        BOOL got = GetMessageW(&m, nullptr, 0, 0);
        if (got <= 0) {
            if (got == 0) PostQuitMessage((int)m.wParam);   // 收到退出消息则转投主循环
            break;
        }
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}

// 功能: 注册/注销开机自启动项(HKCU\...\Run\EasyCallBoard, 当前用户级别无需管理员权限)
// 参数: enable true=写入注册表(路径随 exe 当前位置自动更新); false=删除注册表项
// 返回: true=操作成功; false=打开注册表键失败
static bool AutoStartSet(bool enable) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_SET_VALUE, &k) != ERROR_SUCCESS)
        return false;
    if (enable) {
        std::wstring cmd = L"\"" + ExeDirW() + L"EasyCall-Board.exe\"";   // 带引号完整路径
        RegSetValueExW(k, L"EasyCallBoard", 0, REG_SZ,
                       (const BYTE*)cmd.c_str(), (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, L"EasyCallBoard");
    }
    RegCloseKey(k);
    return true;
}

// ================= WinToast 辅助函数(未启用库时为空实现) =================
#ifdef HAVE_WINTOAST
// 功能: 初始化 WinToast: 设置应用名与 AUMID 后调用 initialize
// 说明: v1.3.2 的 initialize() 内部会自动创建开始菜单快捷方式并管理 COM,
//       这里绝不能自行 CoInitialize/CoUninitialize(会拆掉库的 COM 导致通知失败)
// 返回: 无(初始化失败时在状态栏提示)
static void WinToastInit() {
    using namespace WinToastLib;
    WinToast::instance()->setAppName(L"EasyCall 班级大屏");
    WinToast::instance()->setAppUserModelId(
        WinToast::configureAUMI(L"QuintinZong", L"EasyCall", L"Board"));
    WinToast::WinToastError err = WinToast::WinToastError::NoError;
    if (!WinToast::instance()->initialize(&err)) {
        PostMessageW(g_hwnd, WM_APP_STATUS, 0,
                     (LPARAM)new std::wstring(L"通知初始化失败(错误码 " +
                                              std::to_wstring((int)err) + L")"));
    }
}
// 功能: 通知事件回调(点击/关闭/失败等动作本系统无需处理, 全部空实现)
// 说明: v1.3.2 的 showToast 要求处理器非空, 且必须为全局实例(库在通知存活期内引用它)
class BoardToastHandler : public WinToastLib::IWinToastHandler {
public:
    void toastActivated() const override {}
    void toastActivated(int actionIndex) const override {}
    void toastActivated(std::wstring response) const override {}
    void toastDismissed(WinToastLib::IWinToastHandler::WinToastDismissalReason state) const override {}
    void toastFailed() const override {}
};
static BoardToastHandler g_toastHandler;   // 全局单例, 生命周期覆盖全部通知

// 功能: 弹出叫号通知(标题+人名); 先隐藏上一次通知再显示本次
// 参数: q 本次叫号(地点/教师/学生列表)
// 返回: 无
static INT64 g_lastToastId = -1;   // 上一次通知的 ID(下次弹通知前先清掉)
static void ToastCall(const QCall& q) {
    using namespace WinToastLib;
    std::wstring head = L"请以下同学到" + q.place + L"集合 (" + q.teacher + L")";
    std::wstring names;
    for (auto& it : q.items) {
        if (!names.empty()) names += L"、";
        names += it.name;
    }
    WinToastTemplate t(WinToastTemplate::Text02);         // 标题+正文两行模板
    t.setTextField(head, WinToastTemplate::FirstLine);
    t.setTextField(names, WinToastTemplate::SecondLine);
    t.setAudioOption(WinToastTemplate::AudioOption::Default);
    if (g_lastToastId >= 0) WinToast::instance()->hideToast(g_lastToastId);   // 先清空上一次叫号通知
    WinToast::WinToastError err = WinToast::WinToastError::NoError;
    g_lastToastId = WinToast::instance()->showToast(t, &g_toastHandler, &err);   // 显示本次并记住ID
    if (g_lastToastId < 0)
        PostMessageW(g_hwnd, WM_APP_STATUS, 0,
                     (LPARAM)new std::wstring(L"Toast发送失败(Error Code " +
                                              std::to_wstring((int)err) + L")"));
    else
        PostMessageW(g_hwnd, WM_APP_STATUS, 1, (LPARAM)new std::wstring(L"Toast已发送"));
}
#else
// 未集成 WinToast 库时的空实现(编译始终通过)
static void WinToastInit() {}
static void ToastCall(const QCall&) {}
#endif

// ---------------- 主窗口 ----------------
// 功能: 主窗口过程: 初始化/定时器/命令分发/自定义消息/自绘/退出清理
// 参数: hwnd 窗口句柄; msg 消息; wp/lp 消息参数
// 返回: 按 Win32 窗口过程约定
static LRESULT CALLBACK BoardProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_hwnd = hwnd;
        // 探测屏幕 DPI
        HDC dc = GetDC(nullptr);
        g_dpi = g_forceDpi > 0 ? g_forceDpi : GetDeviceCaps(dc, LOGPIXELSY);
        ReleaseDC(nullptr, dc);
        // 创建4个暗色自绘按钮: 设置/黑屏/对话/一键清空
        g_btnSettings = CreateWindowExW(0, L"BUTTON", L"设置",
                                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                        0, 0, 10, 10, hwnd, (HMENU)IDC_BTN_SETTINGS, g_hInst, nullptr);
        g_btnBlack = CreateWindowExW(0, L"BUTTON", L"黑屏",
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                     0, 0, 10, 10, hwnd, (HMENU)IDC_BTN_BLACK, g_hInst, nullptr);
        g_btnClear = CreateWindowExW(0, L"BUTTON", L"一键清空",
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                     0, 0, 10, 10, hwnd, (HMENU)IDC_BTN_CLEAR, g_hInst, nullptr);
        g_btnChat = CreateWindowExW(0, L"BUTTON", L"对话",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                    0, 0, 10, 10, hwnd, (HMENU)IDC_BTN_CHAT, g_hInst, nullptr);
        SubmitButton(g_btnSettings);   // 全部挂悬停跟踪钩子
        SubmitButton(g_btnBlack);
        SubmitButton(g_btnChat);
        SubmitButton(g_btnClear);
        ChatLoad();   // 载入历史对话
        SetTimer(hwnd, 1, 1000, nullptr);   // 定时器1: 每秒重绘(刷新状态/闪烁)
        SetTimer(hwnd, 2, 300, nullptr);    // 定时器2: 300ms 闪烁节奏与黑屏重绘
        SetTimer(hwnd, 3, 1000, nullptr);   // 定时器3: 每秒叫号队列轮换倒计时
        if (!g_uiSnapPath.empty())
            SetTimer(hwnd, 77, g_uiDelay > 0 ? g_uiDelay : 1500, nullptr);   // --ui 自截图定时器
        StartNet();   // 启动网络线程
        WinToastInit();   // 初始化 WinToast(未集成库时为空操作)
        return 0;
    }
    case WM_SIZE: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        MoveWindow(g_btnSettings, rc.right - S(102), rc.bottom - S(52), S(88), S(36), TRUE);
        MoveWindow(g_btnBlack, rc.right - S(198), rc.bottom - S(52), S(88), S(36), TRUE);
        MoveWindow(g_btnClear, rc.right - S(294), rc.bottom - S(52), S(88), S(36), TRUE);
        MoveWindow(g_btnChat, rc.right - S(390), rc.bottom - S(52), S(88), S(36), TRUE);
        return 0;
    }
    case WM_PAINT:
        Paint(hwnd);
        return 0;
    case WM_TIMER:
        if (wp == 77) {
            // --ui 调试: 截图并退出
            DoUiSnap();
            KillTimer(hwnd, 77);
            PostQuitMessage(0);
            return 0;
        }
        if (wp == 1) InvalidateRect(hwnd, nullptr, FALSE);   // 每秒刷新界面
        else if (wp == 2) {
            if (g_flash > 0) { g_flash--; InvalidateRect(hwnd, nullptr, FALSE); }   // 闪烁倒计时
            else if (g_black) InvalidateRect(hwnd, nullptr, FALSE);   // 黑屏时保持刷新(按钮文字)
        } else if (wp == 3) {
            // 叫号队列: 按入队顺序依次叫号, 每条显示约20秒
            if (!g_black && g_queue.size() > 1) {
                g_advanceLeft--;
                if (g_advanceLeft <= 0) {
                    g_queue.pop_front();   // 队首展示完毕, 出队显示下一位
                    g_callHidden = false;
                    g_advanceLeft = 20;
                    g_flash = 8;   // 换人时闪几下提醒
                    InvalidateRect(hwnd, nullptr, TRUE);
                }
            }
        }
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_BTN_SETTINGS) ShowSettings();
        else if (LOWORD(wp) == IDC_BTN_BLACK) {
            // [黑屏]/[恢复显示] 本地切换
            g_black = !g_black;
            SetWindowTextW(g_btnBlack, g_black ? L"恢复显示" : L"黑屏");
            InvalidateRect(hwnd, nullptr, TRUE);
        } else if (LOWORD(wp) == IDC_BTN_CHAT) EnsureChatWindow();
        else if (LOWORD(wp) == IDC_BTN_CLEAR) {
            // 本机[一键清空]: 只隐藏当前显示并清历史, 不清队列(下一条会自动补上)
            g_callHidden = true;
            g_history.clear();
            if (g_black) {
                g_black = false;   // 黑屏状态下一并退出黑屏
                SetWindowTextW(g_btnBlack, L"黑屏");
            }
            InvalidateRect(hwnd, nullptr, TRUE);
        }
        return 0;
    case WM_APP_NEWCALL: {
        // 新叫号入队: 接管堆上分配的对象所有权
        std::unique_ptr<QCall> m((QCall*)lp);
        // 主窗口最小化时弹 WinToast 通知(先隐藏上一次通知再显示本次)
        if (IsIconic(hwnd)) ToastCall(*m);
        // 下一次叫号先自动清空上一次叫号信息, 保证本次立即显示
        g_queue.clear();
        g_queue.push_back(*m);
        g_callHidden = false;   // 立即显示新叫号
        g_advanceLeft = 20;   // 重置本条显示时长
        g_flash = 10;         // 背景闪烁10拍
        std::wstring names;
        for (auto& it : m->items) {
            if (!names.empty()) names += L"、";
            names += it.name;
        }
        g_history.push_back(NowTimeW() + L" " + m->teacher + L" 叫号 " +
                            std::to_wstring(m->items.size()) + L" 人 → " + m->place + L": " + names);
        while (g_history.size() > 12) g_history.erase(g_history.begin());   // 历史最多12条
        if (g_black) {
            g_black = false;   // 来叫号自动退出黑屏
            SetWindowTextW(g_btnBlack, L"黑屏");
        }
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    }
    case WM_APP_CLEAR:
        // 收到教师端清屏: 清空队列与历史, 退出黑屏
        g_queue.clear();
        g_history.clear();
        if (g_black) {
            g_black = false;
            SetWindowTextW(g_btnBlack, L"黑屏");
        }
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    case WM_APP_BLACK:
        // 收到教师端黑屏指令: 进入黑屏
        g_black = true;
        SetWindowTextW(g_btnBlack, L"恢复显示");
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    case WM_APP_CHAT: {
        // 网络线程投递的 CHAT 帧(heap 上 new 的 string, 所有权转移)
        std::unique_ptr<std::string> f((std::string*)lp);
        OnChatFrame(*f);
        return 0;
    }
    case WM_APP_STATUS: {
        // 网络状态更新(wp=1在线, 0离线)
        std::unique_ptr<std::wstring> t((std::wstring*)lp);
        SetStatus(*t, wp != 0);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;   // 不让系统擦背景(WM_PAINT 里自绘, 防闪烁)
    case WM_PRINT: {
        // 支持 WM_PRINT: 供 DoUiSnap 把整窗(含按钮)绘制到内存 DC 截屏
        HDC dc = (HDC)wp;
        RECT rc;
        GetClientRect(hwnd, &rc);
        PaintDraw(dc, rc);
        if (lp & PRF_CHILDREN) {
            struct Ctx { HDC dc; POINT org; };   // 遍历子控件用的上下文
            Ctx ctx;
            ctx.dc = dc;
            POINT cOrg = { 0, 0 };
            MapWindowPoints(hwnd, nullptr, &cOrg, 1);   // 主窗口原点在屏幕坐标中的位置
            ctx.org = cOrg;
            EnumChildWindows(hwnd, [](HWND c, LPARAM l) -> BOOL {
                Ctx* p = (Ctx*)l;
                RECT cr;
                GetWindowRect(c, &cr);
                int ox = cr.left - p->org.x;   // 子控件相对主窗口客户区的偏移
                int oy = cr.top - p->org.y;
                int cid = GetDlgCtrlID(c);
                // 自绘按钮用 DrawDarkButton 手动重画
                if (cid == IDC_BTN_SETTINGS || cid == IDC_BTN_BLACK || cid == IDC_BTN_CHAT ||
                    cid == IDC_BTN_CLEAR) {
                    wchar_t txt[128];
                    GetWindowTextW(c, txt, 128);
                    RECT r = { ox, oy, ox + (cr.right - cr.left), oy + (cr.bottom - cr.top) };
                    DrawDarkButton(p->dc, r, txt, false, false, MakeFont(11, FW_NORMAL));
                    return TRUE;
                }
                // 普通控件: 平移视口后让它自己 WM_PRINT
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
        // 自绘按钮的绘制入口
        DRAWITEMSTRUCT* d = (DRAWITEMSTRUCT*)lp;
        if (d->CtlType == ODT_BUTTON) {
            wchar_t txt[128];
            GetWindowTextW(d->hwndItem, txt, 128);
            DrawDarkButton(d->hDC, d->rcItem, txt,
                           g_hoverBtn == d->hwndItem,                     // 悬停
                           (d->itemState & ODS_SELECTED) != 0,             // 按下
                           MakeFont(11, FW_NORMAL));
            return TRUE;
        }
        break;
    }
    case WM_DESTROY:
        // 退出清理: 移除托盘图标 -> 停定时器 -> 停网络线程 -> 清对话记录
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        if (g_trayIcon) { DestroyIcon(g_trayIcon); g_trayIcon = nullptr; }
        KillTimer(hwnd, 1);
        KillTimer(hwnd, 2);
        KillTimer(hwnd, 3);
        StopNet();
        ChatWipe();   // 关闭班级端后清空上一次对话
        PostQuitMessage(0);
        return 0;
    case WM_APP_TRAY: {
        // 托盘回调: 左键=恢复主窗口; 右键=弹出菜单(重启/关闭)
        if (LOWORD(lp) == WM_LBUTTONUP || LOWORD(lp) == WM_LBUTTONDBLCLK) {
            ShowWindow(hwnd, SW_RESTORE);
            ShowWindow(hwnd, SW_SHOW);
            SetForegroundWindow(hwnd);
        } else if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) {
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, 1, L"重启应用");
            AppendMenuW(menu, MF_STRING, 2, L"关闭应用");
            POINT pt;
            GetCursorPos(&pt);
            SetForegroundWindow(hwnd);
            int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY,
                                     pt.x, pt.y, 0, hwnd, nullptr);
            DestroyMenu(menu);
            if (cmd == 1) {
                // 重启: 启动自身新实例后退出本实例
                std::wstring exe = ExeDirW() + L"EasyCall-Board.exe";
                STARTUPINFOW si;
                memset(&si, 0, sizeof si);
                si.cb = sizeof si;
                PROCESS_INFORMATION pi;
                memset(&pi, 0, sizeof pi);
                if (CreateProcessW(nullptr, &exe[0], nullptr, nullptr, FALSE,
                                   0, nullptr, nullptr, &si, &pi)) {
                    CloseHandle(pi.hThread);
                    CloseHandle(pi.hProcess);
                }
                DestroyWindow(hwnd);
            } else if (cmd == 2) {
                DestroyWindow(hwnd);
            }
        }
        return 0;
    }
    case WM_CLOSE:
        // 点关闭按钮: 最小化到托盘(隐藏窗口, 不退出进程)
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// 功能: 注册托盘图标(关闭窗口后驻留后台)
// 参数: hwnd 接收托盘回调消息的窗口
// 返回: 无(失败静默, 不影响主功能)
static void TrayInit(HWND hwnd) {
    g_trayIcon = MakeTrayIcon(false);   // 教室端图标: 橙下箭头
    memset(&g_nid, 0, sizeof g_nid);
    g_nid.cbSize = sizeof g_nid;
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_APP_TRAY;
    g_nid.hIcon = g_trayIcon;
    wcsncpy_s(g_nid.szTip, L"EasyCall 教室端", _TRUNCATE);
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

// 功能: 尝试让窗口启用系统圆角(DwmSetWindowAttribute 33, 旧系统自动忽略)
// 参数: h 窗口句柄
// 返回: 无
static void EnableRoundedCorners(HWND h) {
    HMODULE d = LoadLibraryW(L"dwmapi.dll");
    if (!d) return;
    typedef HRESULT(WINAPI* Fn)(HWND, DWORD, LPCVOID, DWORD);
    Fn f = (Fn)GetProcAddress(d, "DwmSetWindowAttribute");
    if (f) {
        int pref = 2;   // DWMWCP_ROUND: 圆角
        f(h, 33, &pref, sizeof pref);
    }
}

// 功能: 把主窗口(含子控件)绘制到内存位图存为 PNG, 并把关键状态
//       写入 uisnap.log 供调试(--ui 模式)
// 参数: 无(使用全局 g_uiSnapPath)
// 返回: 无
static void DoUiSnap() {
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    if (rc.right <= 0 || rc.bottom <= 0 || g_uiSnapPath.empty()) return;
    BITMAPINFO bmi;
    memset(&bmi, 0, sizeof bmi);
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = rc.right;
    bmi.bmiHeader.biHeight = -(int)rc.bottom;   // 负高度: 自上而下
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;              // 32位 BGRA
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool ok = false;
    if (bmp && bits) {
        HDC memdc = CreateCompatibleDC(nullptr);
        HGDIOBJ old = SelectObject(memdc, bmp);
        SendMessageW(g_hwnd, WM_PRINT, (WPARAM)memdc, PRF_CLIENT | PRF_CHILDREN);   // 整窗绘制到位图
        GdiFlush();
        ok = SavePng32(g_uiSnapPath, (const uint8_t*)bits, rc.right, rc.bottom);
        SelectObject(memdc, old);
        DeleteDC(memdc);
    }
    if (bmp) DeleteObject(bmp);
    HANDLE lg = CreateFileW((ExeDirW() + L"uisnap.log").c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (lg != INVALID_HANDLE_VALUE) {
        // 记录 截图是否成功/队列长度/队首教师/状态文字, 供自动化比对
        std::string msg = "ok=" + std::to_string((int)ok) +
                          " queue=" + std::to_string((int)g_queue.size()) +
                          " head=" + (g_queue.empty() ? "-" : WU8(g_queue.front().teacher)) +
                          " status=" + WU8(g_statusText) + "\n";
        DWORD w = 0;
        WriteFile(lg, msg.data(), (DWORD)msg.size(), &w, nullptr);
        CloseHandle(lg);
    }
}

// ================================================================
// 主函数: 班级大屏端程序入口
// ----------------------------------------------------------------
// 执行流程分段说明:
//   ① 初始化: 解析命令行(--ui=<png>[,<延迟ms>] 调试截图) -> 设置 DPI
//      感知 -> 测屏幕 DPI -> 启动网络 -> 初始化公共控件 -> 启动 GDI+
//   ② 读取配置: 从 INI(board节)读 模式/服务器/房间/端口/标题
//   ③ 注册窗口类: 主窗口 BoardProc / 设置对话框 DlgProc / 对话窗口 ChatProc
//   ④ 创建主窗口: 圆角 + 最大化显示
//   ⑤ 启动网络线程(StartNet 在 WM_CREATE 里调用)
//   ⑥ 消息循环: 主窗口消息泵, 收到 WM_QUIT 时退出
//   ⑦ 清理: 停网络、关 GDI+
// ================================================================
// 功能: 班级大屏端入口
// 参数: hInst 实例句柄; (忽略前一实例句柄); lpCmdLine 命令行;
//       nShow 初始显示方式
// 返回: 退出码(0=正常, 1=创建主窗口失败)
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR lpCmdLine, int nShow) {
    g_hInst = hInst;
    // ---- ① 命令行与初始化 ----
    std::wstring cmd(lpCmdLine);
    size_t p = cmd.find(L"--ui=");
    if (p != std::wstring::npos) {
        std::wstring val = cmd.substr(p + 5);
        size_t c = val.find(L',');
        if (c != std::wstring::npos) {
            g_uiDelay = _wtoi(val.substr(c + 1).c_str());   // 可选截图延迟毫秒数
            val = val.substr(0, c);
        }
        g_uiSnapPath = val;   // 截图输出路径
        g_forceDpi = 96;      // 截图固定 96 DPI 保证结果稳定
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

    // ---- ② 读取配置 ----
    g_mode = IniGet(L"board", L"mode", L"lan");
    g_base = IniGet(L"board", L"base", EC_DEFAULT_RELAY);
    if (!g_base.empty() && g_base.back() != L'/') g_base += L'/';   // 保证以 '/' 结尾
    g_room = IniGet(L"board", L"room", L"101");
    g_port = _wtoi(IniGet(L"board", L"port", L"25800").c_str());
    if (g_port <= 0 || g_port > 65535) g_port = EC_TCP_PORT;   // 端口非法用默认值
    g_title = IniGet(L"board", L"title", L"叫号");

    // ---- 开机自启动: 首次启动自动注册; 以后每次启动按设置同步注册表 ----
    if (IniGet(L"board", L"autostart_init", L"").empty()) {
        // 第一次运行: 自动写入 HKCU\...\Run 并记录设置
        AutoStartSet(true);
        IniSet(L"board", L"autostart_init", L"1");
        IniSet(L"board", L"autostart", L"1");
    } else {
        // 非首次: 按用户在设置里的开关同步(0=移除注册表项, 1=注册/更新路径)
        AutoStartSet(IniGet(L"board", L"autostart", L"1") == L"1");
    }

    // ---- ③ 注册3个窗口类 ----
    WNDCLASSW wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = BoardProc;                          // 主窗口
    wc.hInstance = hInst;
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;                          // 背景自绘
    wc.lpszClassName = L"EasyCallBoardWnd";
    RegisterClassW(&wc);

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = DlgProc;                            // 设置对话框
    wc.hInstance = hInst;
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;                          // 背景自绘(Fluent 深蓝)
    wc.lpszClassName = L"EasyCallBoardDlg";
    RegisterClassW(&wc);

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = ChatProc;                           // 对话窗口
    wc.hInstance = hInst;
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;                          // 背景自绘(Fluent 深蓝)
    wc.lpszClassName = L"EasyCallChatWnd";
    RegisterClassW(&wc);

    // ---- ④ 创建并显示主窗口(最大化) ----
    HWND hwnd = CreateWindowExW(0, L"EasyCallBoardWnd", L"EasyCall 班级大屏",
                                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                CW_USEDEFAULT, CW_USEDEFAULT, S(1100), S(680),
                                nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;
    EnableRoundedCorners(hwnd);
    ShowWindow(hwnd, SW_MAXIMIZE);
    TrayInit(hwnd);   // 注册托盘图标
    UpdateWindow(hwnd);

    g_chatSender = BoardSendChat;   // 对话发送回调

    // ---- ⑥ 消息循环 ----
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    // ---- ⑦ 清理 ----
    EcNetStop();
    GdiplusShutdown(gdiToken);
    return 0;
}
