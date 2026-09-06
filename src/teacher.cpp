// EasyCall 班级叫号系统 - 教师端
#define _WIN32_IE 0x0600
#include "ec_common.h"
#include "ec_xlsx.h"
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <winsock2.h>
// MinGW 旧版 gdiplus 头文件缺少 PROPID 定义, 先补上
typedef ULONG PROPID;
#include <gdiplus.h>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <algorithm>

using namespace Gdiplus;
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "gdiplus.lib")

// 自定义消息: 网络线程收到 CHAT 帧后投递到主线程处理
#define WM_APP_CHAT (WM_APP + 2)

// 学生数据: 学号/姓名/班级(备注)
struct Student { std::wstring id, name, cls; };

// 全部控件 ID(枚举, 无符号整型)
enum : INT_PTR {
    IDC_LV = 100, IDC_BTN_IMPORT, IDC_BTN_MANUAL, IDC_BTN_SELALL, IDC_BTN_SELNONE, IDC_BTN_INVERT,
    IDC_BTN_DEL, IDC_BTN_CHAT, IDC_BTN_CALL, IDC_BTN_CLEAR, IDC_BTN_BLACK,
    IDC_RB_LAN, IDC_RB_RELAY, IDC_BTN_SCAN, IDC_COMBO_BOARD,
    IDC_ED_BASE, IDC_ED_ROOM, IDC_BTN_TEST, IDC_HIST, IDC_STATUS,
    IDC_LBL_BASE, IDC_LBL_ROOM, IDC_LBL_NET, IDC_LBL_HISTY,
    IDC_LBL_PLACE, IDC_ED_PLACE, IDC_CK_CLS,
    IDC_CHAT_LOG = 200, IDC_CHAT_INPUT, IDC_CHAT_SEND,   // 对话窗口控件
    IDC_NAME_EDIT = 210, IDC_NAME_OK,                     // 教师名弹窗控件
    IDC_ADD_ID = 220, IDC_ADD_NAME, IDC_ADD_CLS, IDC_ADD_OK, IDC_ADD_DONE   // 手动添加弹窗控件
};

// ---------------- Fluent 配色 ----------------
// 界面统一配色常量(COLORREF 为 0x00BBGGRR)
static const COLORREF C_BG       = RGB(0xF3, 0xF3, 0xF3);   // 窗口背景浅灰
static const COLORREF C_WHITE    = RGB(0xFF, 0xFF, 0xFF);   // 普通按钮底色
static const COLORREF C_BORDER   = RGB(0xD1, 0xD1, 0xD1);   // 普通按钮边框
static const COLORREF C_ACCENT   = RGB(0x0F, 0x6C, 0xBD);   // 主题色(主按钮常态)
static const COLORREF C_ACCENT_H = RGB(0x0D, 0x5F, 0xA9);   // 主题色(悬停)
static const COLORREF C_ACCENT_P = RGB(0x0B, 0x54, 0x95);   // 主题色(按下)
static const COLORREF C_BTN_H    = RGB(0xF7, 0xF7, 0xF7);   // 普通按钮悬停底色
static const COLORREF C_BTN_P    = RGB(0xE9, 0xE9, 0xE9);   // 普通按钮按下底色
static const COLORREF C_TEXT     = RGB(0x1A, 0x1A, 0x1A);   // 主文字色
static const COLORREF C_TEXT2    = RGB(0x60, 0x5E, 0x5C);   // 次要文字色(静态标签)

// ---------------- 全局变量 ----------------
static HINSTANCE g_hInst;                                       // 应用实例句柄
static HWND g_hwnd, g_lv, g_hist, g_status;                     // 主窗口/学生列表/历史列表/状态栏
static HWND g_rbLan, g_rbRelay, g_btnScan, g_comboBoard, g_edBase, g_edRoom, g_btnTest;   // 网络面板控件
static HWND g_btnCall;                                          // [叫号] 主按钮(单独加粗字体)
static HWND g_lblBase, g_lblRoom, g_lblNet, g_lblHisty, g_lblPlace;   // 静态标签
static HWND g_edPlace, g_ckCls;                                 // 地点编辑框 / "大屏显示班级"复选框
static HWND g_chatWnd = nullptr, g_chatLog = nullptr, g_chatInput = nullptr;   // 对话窗口及控件
static HWND g_addDlg = nullptr, g_nameEdit = nullptr;           // 手动添加弹窗 / 教师名编辑框
static std::vector<HWND> g_controls;                            // 统一设字体的控件集合
static std::vector<Student> g_students;                         // 学生名单(内存)
static std::vector<std::wstring> g_chatMsgs;       // 对话记录 "HH:MM:SS 姓名: 内容"
static std::vector<std::string> g_sentChatIds;     // 已发送的聊天ID(中转回显去重用)
static std::wstring g_teacherName = L"教师";       // 教师姓名(叫号/对话署名)
static bool g_showCls = true;                     // 是否把班级随叫号发到大屏
static SOCKET g_sock = INVALID_SOCKET;            // 局域网模式下与大屏端的 TCP 连接
static std::atomic<bool> g_stop{false};           // 后台线程退出标志
static volatile LONG g_presenceSec = -1;          // 大屏最近心跳距现在秒数(-1=未知, 中转模式用)
static volatile LONG g_chatSeq = 0;               // 已消费的中转服务器消息序号
static int g_dpi = 96;                            // 屏幕 DPI(界面缩放基准)
static HFONT g_fontUi = nullptr, g_fontBold = nullptr, g_fontTitle = nullptr;   // 常规/加粗/标题字体
static HWND g_hoverBtn = nullptr;                 // 当前悬停的自绘按钮(绘制悬停态)
static std::thread g_presenceThread, g_lanRecvThread, g_chatFetchThread;   // 三个后台线程
static std::wstring g_uiSnapPath;   // --ui= 自截图调试功能
static int g_forceDpi = 0;          // --ui 调试时强制 96 DPI
static bool (*g_chatSender)(const std::wstring&) = nullptr;   // 对话发送函数指针(TeacherSendChat)
static void DoUiSnap();

// 前置声明(定义在文件后部)
static bool LanConnect(const std::wstring& hostPort, std::wstring& err);
static void CloseLan();
static void RefreshListView();
static void SaveStudents();
static void HistAdd(const std::wstring& line);
static void EnsureChatWindow(bool focus = true);

// 功能: DPI 缩放: 设计稿 96DPI 下的像素值 -> 当前 DPI 下的实际像素
// 参数: px 设计稿像素值
// 返回: 缩放后的像素值
static int S(int px) { return MulDiv(px, g_dpi, 96); }

// 功能: 控件加入全局集合(后面统一设置字体)
// 参数: h 控件句柄
// 返回: 无
static void AddCtl(HWND h) { g_controls.push_back(h); }

// 功能: 读取窗口/编辑框的文本
// 参数: h 控件句柄
// 返回: 控件当前文本(宽字符串)
static std::wstring WinText(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::wstring s(n, 0);
    if (n) GetWindowTextW(h, &s[0], n + 1);
    return s;
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

// 功能: 当前是否处于"服务器中转"模式(看单选按钮状态)
// 参数: 无
// 返回: true=中转模式; false=局域网直连模式
static bool IsRelayMode() {
    return SendMessageW(g_rbRelay, BM_GETCHECK, 0, 0) == BST_CHECKED;
}
// 功能: 取中转服务器地址(去掉首尾空白, 确保以 '/' 结尾)
// 参数: 无
// 返回: 服务器基地址, 如 "http://1.2.3.4:8080/"
static std::wstring GetRelayBase() {
    std::wstring s = TrimW(WinText(g_edBase));
    if (!s.empty() && s.back() != L'/') s += L'/';
    return s;
}
// 功能: 取房间号(去掉首尾空白)
// 参数: 无
// 返回: 房间号字符串
static std::wstring GetRoom() { return TrimW(WinText(g_edRoom)); }

// 功能: 更新底部状态栏文字
// 参数: t 要显示的文本
// 返回: 无
static void SetStatus(const std::wstring& t) {
    if (g_status) SetWindowTextW(g_status, t.c_str());
}

// ---------------- 对话: 持久化 ----------------
// 功能: 对话记录文件路径(EXE目录\chat_teacher.json)
// 参数: 无
// 返回: 文件完整路径
static std::wstring ChatFile() { return ExeDirW() + L"chat_teacher.json"; }
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
// 功能: 从 chat_teacher.json 载入历史对话(文件不存在则留空)
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
// 功能: 删除对话记录文件并清空内存(教师端关闭时调用)
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

// ---------------- 发送 ----------------
// 功能: 向大屏端发送一个报文(所有指令的统一出口)
//       中转模式: HTTP POST 到 <base>push.php, 表单 room=<房间>&data=<报文>
//       直连模式: 经 TCP 帧协议发给大屏(未连接则先连接并记住地址)
// 参数: payload 报文内容(UTF-8); errOut [出参] 失败原因(中文)
// 返回: true=发送成功; false=失败(原因见 errOut)
static bool SendToBoard(const std::string& payload, std::wstring& errOut) {
    if (IsRelayMode()) {
        if (GetRelayBase().empty()) { errOut = L"请先填写服务器地址(如 http://IP:端口/)"; return false; }
        std::string room = WU8(GetRoom());
        if (room.empty()) { errOut = L"请先填写房间号(与班级大屏端一致)"; return false; }
        std::string body = "room=" + UrlEncode(room) + "&data=" + UrlEncode(payload);
        std::string resp;
        std::wstring err;
        if (!HttpPostForm(GetRelayBase() + L"push.php", body, resp, err, 12)) { errOut = err; return false; }
        if (resp.find("OK") == std::string::npos) { errOut = L"服务器返回异常: " + U8W(resp); return false; }
        return true;
    } else {
        if (g_sock == INVALID_SOCKET) {
            std::wstring host = WinText(g_comboBoard);   // 下拉框里选中的大屏 "IP:端口"
            if (host.empty()) { errOut = L"请先点击[扫描教室]或手动填写教室端IP地址"; return false; }
            std::wstring e2;
            if (!LanConnect(host, e2)) { errOut = e2; return false; }
            IniSet(L"net", L"lan_host", TrimW(host));   // 记住地址, 下次自动连
        }
        if (!TcpSendFrame(g_sock, payload)) {
            CloseLan();
            errOut = L"发送失败, 与教室端的连接已断开";
            return false;
        }
        return true;
    }
}

// 功能: 教师端发送一条对话: 构造 CHAT 报文 -> 发大屏 -> 本地记录
//       并登记消息ID(供中转回显去重)
// 参数: text 消息文本
// 返回: true=发送成功; false=文本为空或发送失败
static bool TeacherSendChat(const std::wstring& text) {
    std::wstring t = TrimW(text);
    if (t.empty()) return false;
    std::string msgId = NowStampMs();   // 消息ID = 毫秒时间戳
    g_sentChatIds.push_back(msgId);
    while (g_sentChatIds.size() > 64) g_sentChatIds.erase(g_sentChatIds.begin());   // 只保留最近64个
    std::string payload = "CHAT\n" + msgId + "\n" + WU8(g_teacherName) + "\n" + WU8(t);
    std::wstring err;
    if (!SendToBoard(payload, err)) {
        ChatAppend(L"系统", L"发送失败: " + err);
        return false;
    }
    ChatAppend(g_teacherName, t);
    return true;
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
        // [发送]按钮
        CreateWindowExW(0, L"BUTTON", L"发送",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                        0, 0, 10, 10, hwnd, (HMENU)IDC_CHAT_SEND, g_hInst, nullptr);
        SendMessageW(g_chatLog, WM_SETFONT, (WPARAM)g_fontUi, TRUE);
        SendMessageW(g_chatInput, WM_SETFONT, (WPARAM)g_fontUi, TRUE);
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
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_CHAT_SEND) {
            if (g_chatSender) {
                std::wstring t = WinText(g_chatInput);
                if (!TrimW(t).empty()) {
                    g_chatSender(t);            // 调用 TeacherSendChat
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
    g_chatWnd = CreateWindowExW(0, L"EasyCallChatWnd", L"对话 - 教师端",
                                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                mrc.right - S(560), mrc.top + S(80), S(540), S(540),
                                g_hwnd, nullptr, g_hInst, nullptr);
    ShowWindow(g_chatWnd, SW_SHOW);
}

// ---------------- 教师名弹窗(启动时, owned 非独立窗口) ----------------
// 功能: 教师名弹窗过程: 输入教师姓名, 确定/关闭时保存
// 参数: hwnd 窗口句柄; msg 消息; wp/lp 消息参数
// 返回: 按 Win32 窗口过程约定
static LRESULT CALLBACK NameDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        // 提示标签
        CreateWindowExW(0, L"STATIC", L"请输入教师姓名(叫号时大屏会显示)",
                        WS_CHILD | WS_VISIBLE, S(18), S(22), S(280), S(20),
                        hwnd, nullptr, g_hInst, nullptr);
        std::wstring pre = IniGet(L"net", L"teacher_name", L"");   // 上次填过的名字
        g_nameEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", pre.c_str(),
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                     S(18), S(50), S(270), S(26), hwnd, (HMENU)IDC_NAME_EDIT,
                                     g_hInst, nullptr);
        // [进入叫号]按钮
        CreateWindowExW(0, L"BUTTON", L"进入叫号",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                        S(80), S(92), S(150), S(34), hwnd, (HMENU)IDC_NAME_OK, g_hInst, nullptr);
        SendMessageW(g_nameEdit, WM_SETFONT, (WPARAM)g_fontUi, TRUE);
        SetFocus(g_nameEdit);
        SendMessageW(g_nameEdit, EM_SETSEL, 0, -1);   // 预填文字全选, 可直接覆盖输入
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_NAME_OK) {
            // 先取名字再销毁(销毁后子编辑框不可用)
            std::wstring nm = TrimW(WinText(g_nameEdit));
            if (!nm.empty()) {
                g_teacherName = nm;                     // 非空才覆盖默认名
                IniSet(L"net", L"teacher_name", nm);    // 记住, 下次预填
            }
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_CLOSE: {
        // 直接点 X 关闭: 与[进入叫号]同样保存名字
        std::wstring nm = TrimW(WinText(g_nameEdit));
        if (!nm.empty()) {
            g_teacherName = nm;
            IniSet(L"net", L"teacher_name", nm);
        }
        DestroyWindow(hwnd);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------- 手动添加学生弹窗(owned, 非模态) ----------------
// 功能: 手动添加学生弹窗过程: 输入学号/姓名/班级, [添加]入名单, [完成]关闭
// 参数: hwnd 窗口句柄; msg 消息; wp/lp 消息参数
// 返回: 按 Win32 窗口过程约定
static LRESULT CALLBACK AddDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        // 便捷 lambda: 建标签
        auto mkLbl = [&](const wchar_t* t, int x, int y, int w) {
            CreateWindowExW(0, L"STATIC", t, WS_CHILD | WS_VISIBLE, S(x), S(y), S(w), S(18),
                            hwnd, nullptr, g_hInst, nullptr);
        };
        // 便捷 lambda: 建编辑框并设置字体
        auto mkEdit = [&](const wchar_t* t, INT_PTR id, int x, int y) -> HWND {
            HWND h = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", t,
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                     S(x), S(y), S(220), S(24), hwnd, (HMENU)id, g_hInst, nullptr);
            SendMessageW(h, WM_SETFONT, (WPARAM)g_fontUi, TRUE);
            return h;
        };
        mkLbl(L"学号:", 18, 18, 60);
        mkEdit(L"", IDC_ADD_ID, 70, 16);
        mkLbl(L"姓名:", 18, 50, 60);
        mkEdit(L"", IDC_ADD_NAME, 70, 48);
        mkLbl(L"班级/备注(可选):", 18, 82, 130);
        mkEdit(L"", IDC_ADD_CLS, 70, 80);
        // [添加]按钮: 把当前输入加入名单并清空输入框
        CreateWindowExW(0, L"BUTTON", L"添加",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                        S(40), S(118), S(100), S(30), hwnd, (HMENU)IDC_ADD_OK, g_hInst, nullptr);
        // [完成]按钮: 关闭弹窗
        CreateWindowExW(0, L"BUTTON", L"完成",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                        S(170), S(118), S(100), S(30), hwnd, (HMENU)IDC_ADD_DONE, g_hInst, nullptr);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == IDC_ADD_OK) {
            std::wstring sid = TrimW(WinText(GetDlgItem(hwnd, IDC_ADD_ID)));
            std::wstring sname = TrimW(WinText(GetDlgItem(hwnd, IDC_ADD_NAME)));
            std::wstring scls = TrimW(WinText(GetDlgItem(hwnd, IDC_ADD_CLS)));
            if (sname.empty()) {
                MessageBoxW(hwnd, L"请输入学生姓名", L"提示", MB_ICONINFORMATION);
                return 0;
            }
            g_students.push_back({ sid, sname, scls });
            RefreshListView();
            SaveStudents();
            HistAdd(NowTimeW() + L" 手动添加: " + sname + (sid.empty() ? L"" : L"(" + sid + L")"));
            // 清空三个输入框, 焦点回学号框(便于连续录入)
            SetWindowTextW(GetDlgItem(hwnd, IDC_ADD_ID), L"");
            SetWindowTextW(GetDlgItem(hwnd, IDC_ADD_NAME), L"");
            SetWindowTextW(GetDlgItem(hwnd, IDC_ADD_CLS), L"");
            SetFocus(GetDlgItem(hwnd, IDC_ADD_ID));
            return 0;
        }
        if (id == IDC_ADD_DONE) {
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    }
    case WM_DESTROY:
        g_addDlg = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// 功能: 打开手动添加学生弹窗(非模态, 已存在则置前台)
// 参数: 无
// 返回: 无
static void ShowAddDialog() {
    if (g_addDlg && IsWindow(g_addDlg)) {
        SetForegroundWindow(g_addDlg);
        return;
    }
    RECT mrc;
    GetWindowRect(g_hwnd, &mrc);
    g_addDlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"EasyCallAddDlg", L"手动添加学生",
                               WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                               mrc.left + (mrc.right - mrc.left) / 2 - S(160),   // 主窗口居中
                               mrc.top + (mrc.bottom - mrc.top) / 2 - S(100),
                               S(330), S(190), g_hwnd, nullptr, g_hInst, nullptr);
}

// ---------------- 学生名单持久化 ----------------
// 功能: 把学生名单保存为 JSON 数组文件 students.json
//       格式: [{"i":"学号","n":"姓名","c":"班级"},...]
// 参数: 无
// 返回: 无
static void SaveStudents() {
    std::string json = "[";
    bool first = true;
    for (auto& s : g_students) {
        if (!first) json += ",";
        first = false;
        json += "{\"i\":\"" + JsonEscape(WU8(s.id)) + "\",\"n\":\"" + JsonEscape(WU8(s.name)) +
                "\",\"c\":\"" + JsonEscape(WU8(s.cls)) + "\"}";
    }
    json += "]";
    HANDLE h = CreateFileW((ExeDirW() + L"students.json").c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        WriteFile(h, json.data(), (DWORD)json.size(), &w, nullptr);
        CloseHandle(h);
    }
}
// 功能: 从 students.json 载入学生名单(文件不存在则留空)
// 参数: 无
// 返回: 无
static void LoadStudents() {
    g_students.clear();
    HANDLE h = CreateFileW((ExeDirW() + L"students.json").c_str(), GENERIC_READ,
                           FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD hi = 0;
    DWORD size = GetFileSize(h, &hi);
    if (hi || size == 0 || size > 16 * 1024 * 1024) { CloseHandle(h); return; }   // 上限16MB防异常
    std::string json(size, 0);
    DWORD got = 0;
    if (!ReadFile(h, &json[0], size, &got, nullptr) || got != size) { CloseHandle(h); return; }
    CloseHandle(h);
    size_t pos = 0;
    for (;;) {
        // 逐个提取 {...} 对象, 取字段 i/n/c
        size_t a = json.find('{', pos);
        if (a == std::string::npos) break;
        size_t b = json.find('}', a);
        if (b == std::string::npos) break;
        std::string obj = json.substr(a, b - a + 1);
        Student s;
        s.id = U8W(JsonStrVal(obj, "i"));
        s.name = U8W(JsonStrVal(obj, "n"));
        s.cls = U8W(JsonStrVal(obj, "c"));
        if (!s.id.empty() || !s.name.empty()) g_students.push_back(s);
        pos = b + 1;
    }
}

// ---------------- 列表 ----------------
// 功能: 用 g_students 刷新列表视图(三列: 学号/姓名/班级)
// 参数: 无
// 返回: 无
static void RefreshListView() {
    ListView_DeleteAllItems(g_lv);
    for (size_t i = 0; i < g_students.size(); i++) {
        LVITEMW it;
        memset(&it, 0, sizeof it);
        it.mask = LVIF_TEXT;
        it.iItem = (int)i;
        it.pszText = (LPWSTR)g_students[i].id.c_str();
        ListView_InsertItem(g_lv, &it);   // 第0列(学号)随插入
        ListView_SetItemText(g_lv, (int)i, 1, (LPWSTR)g_students[i].name.c_str());
        ListView_SetItemText(g_lv, (int)i, 2, (LPWSTR)g_students[i].cls.c_str());
    }
}
// 功能: 判断列表第 i 行是否被勾选(复选框状态图索引 1=未选, 2=已选)
// 参数: i 行号(0基)
// 返回: true=已勾选; false=未勾选
static bool IsChecked(int i) {
    UINT st = ListView_GetItemState(g_lv, i, LVIS_STATEIMAGEMASK);
    return ((st >> 12) - 1) == 1;
}
// 功能: 全选/全不选列表所有行
// 参数: c true=全选; false=全不选
// 返回: 无
static void SetAllChecked(bool c) {
    int n = ListView_GetItemCount(g_lv);
    for (int i = 0; i < n; i++)
        ListView_SetItemState(g_lv, i, INDEXTOSTATEIMAGEMASK(c ? 2 : 1), LVIS_STATEIMAGEMASK);
}
// 功能: 反选: 勾选<->未勾选互换
// 参数: 无
// 返回: 无
static void InvertChecked() {
    int n = ListView_GetItemCount(g_lv);
    for (int i = 0; i < n; i++)
        ListView_SetItemState(g_lv, i, INDEXTOSTATEIMAGEMASK(IsChecked(i) ? 1 : 2), LVIS_STATEIMAGEMASK);
}
// 功能: 向右侧历史列表框追加一行(最多保留300行, 自动滚到底)
// 参数: line 单行文本
// 返回: 无
static void HistAdd(const std::wstring& line) {
    SendMessageW(g_hist, LB_ADDSTRING, 0, (LPARAM)line.c_str());
    int n = (int)SendMessageW(g_hist, LB_GETCOUNT, 0, 0);
    if (n > 300) SendMessageW(g_hist, LB_DELETESTRING, 0, 0);   // 超出300行删最旧
    SendMessageW(g_hist, LB_SETCURSEL, n - 1, 0);
    SendMessageW(g_hist, LB_SETCURSEL, -1, 0);
}

// ---------------- 导入 ----------------
// 功能: [导入Excel]处理: 打开文件选择框 -> 调 ImportSpreadsheet 解析 ->
//       替换名单 -> 刷新列表并保存
// 参数: hwnd 主窗口句柄(文件对话框/错误弹窗的父窗口)
// 返回: 无
static void DoImport(HWND hwnd) {
    wchar_t file[MAX_PATH * 2] = L"";
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"Excel / CSV 文件 (*.xlsx;*.csv)\0*.xlsx;*.csv\0所有文件 (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH * 2;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return;   // 用户取消
    std::vector<XRow> rows;
    std::wstring err;
    if (!ImportSpreadsheet(file, rows, err)) {
        MessageBoxW(hwnd, err.c_str(), L"导入失败", MB_ICONWARNING);
        return;
    }
    g_students.clear();
    for (auto& r : rows) g_students.push_back({ r.id, r.name, r.cls });
    RefreshListView();
    SaveStudents();
    SetStatus(L"已导入 " + std::to_wstring(g_students.size()) + L" 名学生");
    HistAdd(NowTimeW() + L" 导入名单 " + std::to_wstring(g_students.size()) + L" 人: " + std::wstring(file));
}

// ---------------- 局域网连接 ----------------
// 功能: 连接大屏端(局域网直连): 解析 "IP[:端口]" -> TCP 非阻塞连接(3秒超时)
// 参数: hostPort 地址文本(如 "192.168.1.10" 或 "192.168.1.10:25800");
//       err [出参] 失败原因(中文)
// 返回: true=连接成功(套接字存入 g_sock); false=失败
static bool LanConnect(const std::wstring& hostPort, std::wstring& err) {
    std::wstring hp = TrimW(hostPort);
    size_t sp = hp.find_first_of(L" \t");
    if (sp != std::wstring::npos) hp = hp.substr(0, sp);   // 扫描结果里 "IP:端口  (名称)" 只取地址部分
    hp = TrimW(hp);
    std::wstring host = hp;
    int port = EC_TCP_PORT;   // 默认端口
    size_t c = hp.rfind(L':');
    if (c != std::wstring::npos) {
        std::wstring ps = hp.substr(c + 1);
        if (!ps.empty() && ps.find_first_not_of(L"0123456789") == std::wstring::npos) {
            host = hp.substr(0, c);   // 冒号后全是数字: 视为端口
            port = _wtoi(ps.c_str());
        }
    }
    if (host.empty()) { err = L"请先填写教室端IP地址"; return false; }
    addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;       // 仅 IPv4
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(WU8(host).c_str(), nullptr, &hints, &res) != 0 || !res) {
        err = L"无法解析地址: " + host;
        return false;
    }
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { freeaddrinfo(res); err = L"创建套接字失败"; return false; }
    sockaddr_in sa = *(sockaddr_in*)res->ai_addr;
    sa.sin_port = htons((u_short)port);
    freeaddrinfo(res);
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);   // 置非阻塞, 手动实现连接超时
    int r = connect(s, (sockaddr*)&sa, sizeof sa);
    if (r == SOCKET_ERROR) {
        int e = WSAGetLastError();
        if (e == WSAEWOULDBLOCK || e == WSAEINPROGRESS || e == WSAEINVAL) {
            // 连接进行中: 用 select 等最多3秒
            fd_set w; FD_ZERO(&w); FD_SET(s, &w);
            timeval tv; tv.tv_sec = 3; tv.tv_usec = 0;
            r = select(0, nullptr, &w, nullptr, &tv);
            if (r <= 0) { closesocket(s); err = L"连接超时: 请确认教室端已启动且与本机处于同一局域网"; return false; }
            int soerr = 0; int slen = sizeof soerr;
            getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&soerr, &slen);   // 取真实连接结果
            if (soerr != 0) { closesocket(s); err = L"连接被拒绝(教室端未监听端口 " + std::to_wstring(port) + L")"; return false; }
        } else {
            closesocket(s);
            err = L"连接失败";
            return false;
        }
    }
    nb = 0;
    ioctlsocket(s, FIONBIO, &nb);   // 恢复阻塞模式
    g_sock = s;
    return true;
}
// 功能: 断开与大屏端的 TCP 连接并复位 g_sock
// 参数: 无
// 返回: 无
static void CloseLan() {
    if (g_sock != INVALID_SOCKET) { closesocket(g_sock); g_sock = INVALID_SOCKET; }
}

// ---------------- 叫号 / 清屏 / 黑屏 ----------------
// 功能: [叫号]处理: 收集勾选学生 -> 构造 CALL 报文 -> 发送 -> 记历史 -> 取消勾选
// 参数: hwnd 主窗口句柄(错误弹窗父窗口)
// 返回: 无
static void OnCall(HWND hwnd) {
    std::vector<std::wstring> items;
    int n = ListView_GetItemCount(g_lv);
    for (int i = 0; i < n; i++) {
        if (!IsChecked(i)) continue;
        if ((size_t)i >= g_students.size()) continue;
        const Student& s = g_students[i];
        // 学生行格式: "学号\t姓名\t班级"(班级可选, 由 g_showCls 决定是否带)
        items.push_back(s.id + L"\t" + s.name + L"\t" + (g_showCls ? s.cls : L""));
    }
    if (items.empty()) {
        MessageBoxW(hwnd, L"请先勾选要叫号的学生(可同时勾选多个)", L"提示", MB_ICONINFORMATION);
        return;
    }
    std::wstring place = TrimW(WinText(g_edPlace));
    if (place.empty()) place = L"台前";   // 地点默认"台前"
    IniSet(L"net", L"place", place);
    std::string callId;
    std::string payload = BuildCallPayload(place, g_teacherName, items, &callId);
    std::wstring err;
    if (!SendToBoard(payload, err)) {
        MessageBoxW(hwnd, err.c_str(), L"叫号发送失败", MB_ICONWARNING);
        return;
    }
    // 拼历史记录里的人名列表(从 items 的 "学号\t姓名\t班级" 里取姓名)
    std::wstring names;
    for (auto& it : items) {
        size_t t1 = it.find(L'\t');
        size_t t2 = (t1 == std::wstring::npos) ? std::wstring::npos : it.find(L'\t', t1 + 1);
        std::wstring id = it.substr(0, t1);
        std::wstring name = (t1 == std::wstring::npos) ? L"" : it.substr(t1 + 1, (t2 == std::wstring::npos ? std::wstring::npos : t2 - t1 - 1));
        if (!names.empty()) names += L"、";
        names += name;
        if (!id.empty()) names += L"(" + id + L")";
    }
    HistAdd(NowTimeW() + L" 叫号 " + std::to_wstring(items.size()) + L" 人 → " + place +
            L" [" + g_teacherName + L"]: " + names);
    SetAllChecked(false);   // 叫完后清空勾选
}
// 功能: [发送清屏]处理: 向大屏发 CLEAR 指令(清空叫号队列与历史)
// 参数: hwnd 主窗口句柄
// 返回: 无
static void OnClear(HWND hwnd) {
    std::wstring err;
    if (!SendToBoard("CLEAR", err)) {
        MessageBoxW(hwnd, err.c_str(), L"清屏发送失败", MB_ICONWARNING);
        return;
    }
    HistAdd(NowTimeW() + L" 已发送清屏");
}
// 功能: [一键黑屏]处理: 向大屏发 BLACK 指令(大屏显示"保持安静")
// 参数: hwnd 主窗口句柄
// 返回: 无
static void OnBlack(HWND hwnd) {
    std::wstring err;
    if (!SendToBoard("BLACK", err)) {
        MessageBoxW(hwnd, err.c_str(), L"发送失败", MB_ICONWARNING);
        return;
    }
    HistAdd(NowTimeW() + L" 已发送一键黑屏");
}

// ---------------- 扫描 / 服务器测试 / 状态 ----------------
// 功能: [扫描教室]处理: UDP 发现局域网内的大屏端, 结果填入下拉框并选中第一台
// 参数: 无
// 返回: 无
static void DoScan() {
    SetStatus(L"正在扫描局域网中的教室大屏(3秒)…");
    std::vector<BoardInfo> boards;
    int n = DiscoverBoards(boards, 3000);   // 收集3秒
    SendMessageW(g_comboBoard, CB_RESETCONTENT, 0, 0);
    for (auto& b : boards) {
        std::wstring s = b.ip + L":" + std::to_wstring(b.port);
        if (!b.name.empty()) s += L"  (" + b.name + L")";
        SendMessageW(g_comboBoard, CB_ADDSTRING, 0, (LPARAM)s.c_str());
    }
    if (!boards.empty()) {
        SendMessageW(g_comboBoard, CB_SETCURSEL, 0, 0);
        SetStatus(L"发现 " + std::to_wstring(n) + L" 台教室大屏, 已选第一台");
    } else {
        SetStatus(L"未发现教室大屏: 请确认大屏端已启动且同一局域网(也可直接在框中输入大屏IP)");
    }
}
// 功能: [测试连接]处理(中转模式): 向服务器 push 一条 PING, 再查 presence.php
//       确认大屏在线状态, 结果弹窗显示
// 参数: hwnd 主窗口句柄
// 返回: 无
static void DoTestServer(HWND hwnd) {
    if (GetRelayBase().empty()) {
        MessageBoxW(hwnd, L"请先在右侧填写服务器地址(如 http://IP:端口/)", L"提示", MB_ICONINFORMATION);
        return;
    }
    std::wstring room = GetRoom();
    if (room.empty()) {
        MessageBoxW(hwnd, L"请先填写房间号", L"提示", MB_ICONINFORMATION);
        return;
    }
    SetStatus(L"正在测试中转服务器…");
    std::string body = "room=" + UrlEncode(WU8(room)) + "&data=" + UrlEncode("PING");
    std::string resp;
    std::wstring err;
    if (!HttpPostForm(GetRelayBase() + L"push.php", body, resp, err, 10)) {
        MessageBoxW(hwnd, err.c_str(), L"服务器测试失败", MB_ICONWARNING);
        SetStatus(L"中转服务器测试失败");
        return;
    }
    std::wstring msg = L"服务器可达, 消息已写入房间[" + room + L"]\n响应: " + U8W(resp);
    std::string pr;
    std::wstring e2;
    if (HttpGet(GetRelayBase() + L"presence.php?room=" + U8W(UrlEncode(WU8(room))), pr, e2, 6)) {
        long sec = _wtol(U8W(pr).c_str());   // 返回大屏最近心跳距现在秒数
        msg += sec >= 0 && sec <= 25 ? L"\n教室大屏: 在线" : L"\n教室大屏: 离线(未上报心跳, 请确认大屏端已运行且房间号一致)";
    }
    MessageBoxW(hwnd, msg.c_str(), L"测试结果", MB_ICONINFORMATION);
    SetStatus(L"中转服务器测试完成");
}
// 功能: 刷新状态栏: 显示当前模式/大屏在线状态或连接地址/学生数/教师名
// 参数: 无
// 返回: 无
static void UpdateStatus() {
    wchar_t buf[512];
    if (IsRelayMode()) {
        std::wstring on;
        if (g_presenceSec < 0) on = L"未知";          // 还没查到心跳
        else if (g_presenceSec <= 25) on = L"在线";   // 心跳在25秒内视为在线
        else on = L"离线";
        swprintf(buf, 512, L"模式: 服务器中转 | 大屏: %ls | 学生: %d 人 | 教师: %ls",
                 on.c_str(), (int)g_students.size(), g_teacherName.c_str());
    } else {
        std::wstring st = (g_sock != INVALID_SOCKET) ? WinText(g_comboBoard) : L"未连接";
        swprintf(buf, 512, L"模式: 局域网直连 | 教室端: %ls | 学生: %d 人 | 教师: %ls",
                 st.c_str(), (int)g_students.size(), g_teacherName.c_str());
    }
    SetStatus(buf);
}

// ---------------- 后台线程 ----------------
// 功能: 心跳查询线程(仅中转模式): 每6秒查 presence.php 获取大屏在线状态
// 参数: 无
// 返回: 无
static void PresenceThreadProc() {
    while (!g_stop.load()) {
        if (IsRelayMode()) {
            std::wstring room = GetRoom();
            if (!room.empty()) {
                std::string resp;
                std::wstring err;
                long sec = -1;
                if (HttpGet(GetRelayBase() + L"presence.php?room=" + U8W(UrlEncode(WU8(room))), resp, err, 8))
                    sec = _wtol(U8W(resp).c_str());
                InterlockedExchange(&g_presenceSec, (LONG)sec);   // 原子更新, 主线程读取
            }
        }
        Sleep(6000);
    }
}
// 功能: 局域网收帧线程: 阻塞收大屏发来的帧(主要是 CHAT 回话), 收到后
//       通过 WM_APP_CHAT 投递到主线程处理; 断开则循环重连等待
// 参数: 无
// 返回: 无
static void LanRecvThreadProc() {
    while (!g_stop.load()) {
        if (!IsRelayMode() && g_sock != INVALID_SOCKET) {
            bool ok = false;
            std::string f = TcpRecvFrame(g_sock, 30000, &ok);   // 30秒超时
            if (!ok) {
                CloseLan();   // 连接断开: 关掉, 下次发送时自动重连
                continue;
            }
            PostMessageW(g_hwnd, WM_APP_CHAT, 0, (LPARAM)new std::string(f));   // 转移所有权给主线程
        } else {
            Sleep(400);   // 中转模式或无连接时低频轮询
        }
    }
}
// 功能: 中转拉取线程: 轮询 fetch.php?room=..&after=<seq> 增量取新消息,
//       把其中的 CHAT 帧投递到主线程; 序号取自响应头 X-EasyCall-Seq
// 参数: 无
// 返回: 无
static void ChatFetchThreadProc() {
    while (!g_stop.load()) {
        if (IsRelayMode() && !GetRelayBase().empty()) {
            std::wstring room = GetRoom();
            if (!room.empty()) {
                std::string resp, hdr;
                std::wstring err;
                std::wstring url = GetRelayBase() + L"fetch.php?room=" + U8W(UrlEncode(WU8(room))) +
                                   L"&after=" + std::to_wstring(InterlockedCompareExchange(&g_chatSeq, 0, 0));
                if (HttpGet(url, resp, err, 32, &hdr)) {   // 长轮询最长32秒
                    long long seq = HttpSeqFromHeader(hdr);
                    if (seq > 0) InterlockedExchange(&g_chatSeq, (LONG)seq);   // 记录已消费到哪
                    std::vector<std::string> frames;
                    ParseFrames(resp, frames);   // 响应体 = 多个 [4字节大端长度+负载] 帧
                    for (auto& fr : frames) {
                        std::vector<std::string> lines = SplitLines(fr);
                        if (!lines.empty() && lines[0] == "CHAT")
                            PostMessageW(g_hwnd, WM_APP_CHAT, 0, (LPARAM)new std::string(fr));
                    }
                }
            }
        } else {
            Sleep(2000);
        }
    }
}

// ---------------- Fluent 按钮(自绘) ----------------
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
// 功能: 用 GDI+ 画圆角矩形路径并填充+描边(四个角各用一段90度圆弧)
// 参数: g 图形对象; rc 目标矩形; rad 圆角半径;
//       fill 填充画刷; pen 描边画笔
// 返回: 无
static void DrawRoundedPath(Graphics& g, const RECT& rc, int rad, const SolidBrush& fill, const Pen& pen) {
    int w = rc.right - rc.left - 1, h = rc.bottom - rc.top - 1;
    int d = rad * 2;
    GraphicsPath path;
    path.AddArc(rc.left, rc.top, d, d, 180, 90);                // 左上角
    path.AddArc(rc.left + w - d, rc.top, d, d, 270, 90);        // 右上角
    path.AddArc(rc.left + w - d, rc.top + h - d, d, d, 0, 90);  // 右下角
    path.AddArc(rc.left, rc.top + h - d, d, d, 90, 90);         // 左下角
    path.CloseFigure();
    g.FillPath(&fill, &path);
    g.DrawPath(&pen, &path);
}
// 功能: 绘制一个 Fluent 风格按钮: 背景清灰 -> 圆角底色 -> 边框 -> 居中文字
// 参数: dc 设备上下文; rc 按钮区域; text 按钮文字;
//       primary 是否主题色主按钮; hover 悬停态; pressed 按下态;
//       disabled 禁用态; font 文字字体
// 返回: 无
static void DrawFluentButton(HDC dc, const RECT& rc, const wchar_t* text,
                             bool primary, bool hover, bool pressed, bool disabled, HFONT font) {
    Graphics g(dc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);   // 抗锯齿
    {
        SolidBrush bg(FluentColor(C_BG));
        g.FillRectangle(&bg, (INT)rc.left, (INT)rc.top,
                        (INT)(rc.right - rc.left), (INT)(rc.bottom - rc.top));   // 先填窗口底色
    }
    int rad = 4;
    // 按 状态/类型 选择填充色
    SolidBrush fill(
        disabled ? FluentColor(RGB(0xF4, 0xF4, 0xF4)) :
        primary  ? FluentColor(pressed ? C_ACCENT_P : hover ? C_ACCENT_H : C_ACCENT) :
                   FluentColor(pressed ? C_BTN_P : hover ? C_BTN_H : C_WHITE));
    // 按 状态/类型 选择边框色
    Pen pen(disabled ? FluentColor(RGB(0xE2, 0xE2, 0xE2)) :
            primary  ? FluentColor(pressed ? C_ACCENT_P : hover ? C_ACCENT_H : C_ACCENT) :
                       FluentColor(C_BORDER), 1.0f);
    DrawRoundedPath(g, rc, rad, fill, pen);
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ of = SelectObject(dc, font);
    SetTextColor(dc, disabled ? RGB(0xA0, 0xA0, 0xA0) : primary ? RGB(0xFF, 0xFF, 0xFF) : C_TEXT);
    RECT r = rc;
    DrawTextW(dc, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, of);
}

// ---------------- 布局 ----------------
// 功能: 按控件ID移动主窗口内的控件
// 参数: id 控件ID; x,y,w,h 新位置与大小(逻辑像素)
// 返回: 无
static void MoveCtl(int id, int x, int y, int w, int h) {
    HWND c = GetDlgItem(g_hwnd, id);
    if (c) MoveWindow(c, x, y, w, h, TRUE);
}
// 功能: 按控件ID显示/隐藏主窗口内的控件
// 参数: id 控件ID; show true=显示 false=隐藏
// 返回: 无
static void ShowCtl(int id, bool show) {
    HWND c = GetDlgItem(g_hwnd, id);
    if (c) ShowWindow(c, show ? SW_SHOW : SW_HIDE);
}

// 功能: 主窗口整体布局(创建/缩放/切换模式时调用):
//       顶部两排按钮 -> 左列表 + 右网络面板 -> 底部状态栏;
//       按当前模式(直连/中转)切换右侧控件显隐
// 参数: 无
// 返回: 无
static void Layout() {
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    int M = S(14);   // 边距
    int y = S(14), h1 = S(34);   // 第一排按钮
    MoveCtl(IDC_BTN_IMPORT, M, y, S(108), h1);
    MoveCtl(IDC_BTN_MANUAL, M + S(116), y, S(92), h1);
    MoveCtl(IDC_BTN_SELALL, M + S(216), y, S(62), h1);
    MoveCtl(IDC_BTN_SELNONE, M + S(286), y, S(74), h1);
    MoveCtl(IDC_BTN_INVERT, M + S(368), y, S(62), h1);
    MoveCtl(IDC_BTN_DEL, M + S(438), y, S(88), h1);
    MoveCtl(IDC_BTN_CHAT, M + S(534), y, S(70), h1);

    int y2 = y + h1 + S(10), h2 = S(44);   // 第二排: 叫号/清屏/黑屏/班级勾选/地点
    MoveCtl(IDC_BTN_CALL, M, y2, S(138), h2);
    MoveCtl(IDC_BTN_CLEAR, M + S(146), y2, S(106), h2);
    MoveCtl(IDC_BTN_BLACK, M + S(260), y2, S(106), h2);
    MoveCtl(IDC_CK_CLS, M + S(376), y2 + S(10), S(150), S(24));
    MoveCtl(IDC_LBL_PLACE, M + S(532), y2 + S(12), S(44), S(20));
    MoveCtl(IDC_ED_PLACE, M + S(576), y2 + S(9), S(170), S(26));

    int mainTop = y2 + h2 + S(16);         // 主区域上缘
    int mainBottom = rc.bottom - S(34) - S(8);   // 主区域下缘(状态栏之上)
    int panelW = S(310);                   // 右侧面板宽度
    int panelX = rc.right - panelW - M;    // 右侧面板X起点

    MoveCtl(IDC_LV, M, mainTop, panelX - M - S(12), mainBottom - mainTop);   // 左侧学生列表
    MoveCtl(IDC_STATUS, M, rc.bottom - S(28), rc.right - M - M, S(20));      // 底部状态栏

    bool relay = IsRelayMode();
    int cY = mainTop;   // 右侧面板各控件的基准Y
    MoveCtl(IDC_LBL_NET, panelX, cY + S(2), panelW, S(22));
    MoveCtl(IDC_RB_LAN, panelX + S(6), cY + S(28), S(130), S(24));
    MoveCtl(IDC_RB_RELAY, panelX + S(6), cY + S(54), S(210), S(24));
    ShowCtl(IDC_COMBO_BOARD, !relay);   // 直连模式: 显示大屏地址下拉框与扫描按钮
    ShowCtl(IDC_BTN_SCAN, !relay);
    if (!relay) {
        MoveCtl(IDC_COMBO_BOARD, panelX + S(6), cY + S(86), panelW - S(6) - S(10) - S(86), S(26));
        MoveCtl(IDC_BTN_SCAN, panelX + panelW - S(90), cY + S(85), S(84), S(28));
    }
    ShowCtl(IDC_LBL_BASE, relay);   // 中转模式: 显示服务器地址/房间号/测试按钮
    ShowCtl(IDC_ED_BASE, relay);
    ShowCtl(IDC_LBL_ROOM, relay);
    ShowCtl(IDC_ED_ROOM, relay);
    ShowCtl(IDC_BTN_TEST, relay);
    if (relay) {
        MoveCtl(IDC_LBL_BASE, panelX + S(6), cY + S(86), panelW - S(12), S(18));
        MoveCtl(IDC_ED_BASE, panelX + S(6), cY + S(104), panelW - S(12), S(24));
        MoveCtl(IDC_LBL_ROOM, panelX + S(6), cY + S(134), panelW - S(12), S(18));
        MoveCtl(IDC_ED_ROOM, panelX + S(6), cY + S(152), panelW - S(12) - S(10) - S(96), S(24));
        MoveCtl(IDC_BTN_TEST, panelX + panelW - S(100), cY + S(150), S(94), S(26));
    }
    MoveCtl(IDC_LBL_HISTY, panelX, cY + S(186), panelW, S(22));
    MoveCtl(IDC_HIST, panelX + S(6), cY + S(210), panelW - S(12), mainBottom - cY - S(210) - S(4));   // 历史列表
}

// ---------------- 控件创建 ----------------
// 功能: 创建主窗口全部子控件(列表/按钮/单选/下拉/编辑框/标签/历史/状态栏),
//       并恢复 INI 中保存的网络配置
// 参数: hwnd 主窗口句柄
// 返回: 无
static void CreateControls(HWND hwnd) {
    // 学生列表: 报表样式 + 全行选中 + 网格线 + 复选框 + 双缓冲防闪烁
    g_lv = CreateWindowExW(0, WC_LISTVIEWW, L"",
                           WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS,
                           0, 0, 10, 10, hwnd, (HMENU)IDC_LV, g_hInst, nullptr);
    ListView_SetExtendedListViewStyle(g_lv, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES |
                                             LVS_EX_CHECKBOXES | LVS_EX_DOUBLEBUFFER);
    LVCOLUMNW lc;
    memset(&lc, 0, sizeof lc);
    lc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    lc.pszText = (LPWSTR)L"学号"; lc.cx = S(100); lc.iSubItem = 0;
    ListView_InsertColumn(g_lv, 0, &lc);
    lc.pszText = (LPWSTR)L"姓名"; lc.cx = S(180); lc.iSubItem = 1;
    ListView_InsertColumn(g_lv, 1, &lc);
    lc.pszText = (LPWSTR)L"班级/备注"; lc.cx = S(260); lc.iSubItem = 2;
    ListView_InsertColumn(g_lv, 2, &lc);
    AddCtl(g_lv);

    struct BtnDef { const wchar_t* text; INT_PTR id; };   // 按钮定义表: 文字+ID
    static const BtnDef btns[] = {
        { L"导入Excel", IDC_BTN_IMPORT }, { L"手动添加", IDC_BTN_MANUAL },
        { L"全选", IDC_BTN_SELALL }, { L"全不选", IDC_BTN_SELNONE },
        { L"反选", IDC_BTN_INVERT }, { L"删除选中", IDC_BTN_DEL },
        { L"对话", IDC_BTN_CHAT }, { L"叫 号", IDC_BTN_CALL },
        { L"发送清屏", IDC_BTN_CLEAR }, { L"一键黑屏", IDC_BTN_BLACK },
        { L"扫描教室", IDC_BTN_SCAN }, { L"测试连接", IDC_BTN_TEST }
    };
    for (auto& b : btns) {
        HWND h = CreateWindowExW(0, L"BUTTON", b.text,
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,   // 自绘按钮
                                 0, 0, 10, 10, hwnd, (HMENU)b.id, g_hInst, nullptr);
        SubmitButton(h);   // 挂悬停跟踪钩子
        AddCtl(h);
        if (b.id == IDC_BTN_CALL) g_btnCall = h;
        if (b.id == IDC_BTN_SCAN) g_btnScan = h;
        if (b.id == IDC_BTN_TEST) g_btnTest = h;
    }
    // 网络模式单选按钮(互斥一组)
    g_rbLan = CreateWindowExW(0, L"BUTTON", L"局域网直连",
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON | WS_GROUP,
                              0, 0, 10, 10, hwnd, (HMENU)IDC_RB_LAN, g_hInst, nullptr);
    g_rbRelay = CreateWindowExW(0, L"BUTTON", L"服务器中转(跨网络)",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON,
                                0, 0, 10, 10, hwnd, (HMENU)IDC_RB_RELAY, g_hInst, nullptr);
    if (IniGet(L"net", L"mode", L"lan") == L"relay")   // 恢复上次模式
        SendMessageW(g_rbRelay, BM_SETCHECK, BST_CHECKED, 0);
    else
        SendMessageW(g_rbLan, BM_SETCHECK, BST_CHECKED, 0);
    // 大屏地址下拉框(可编辑, 直连模式)
    g_comboBoard = CreateWindowExW(0, L"COMBOBOX", L"",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWN | WS_VSCROLL,
                                   0, 0, 10, 200, hwnd, (HMENU)IDC_COMBO_BOARD, g_hInst, nullptr);
    // 中转服务器地址 / 房间号编辑框
    g_edBase = CreateWindowExW(0, L"EDIT", L"",
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                               0, 0, 10, 10, hwnd, (HMENU)IDC_ED_BASE, g_hInst, nullptr);
    g_edRoom = CreateWindowExW(0, L"EDIT", L"",
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                               0, 0, 10, 10, hwnd, (HMENU)IDC_ED_ROOM, g_hInst, nullptr);
    AddCtl(g_comboBoard); AddCtl(g_edBase); AddCtl(g_edRoom);

    auto mkLbl = [&](const wchar_t* t, INT_PTR id, bool bold) -> HWND {   // 便捷 lambda: 建标签
        HWND h = CreateWindowExW(0, L"STATIC", t, WS_CHILD | WS_VISIBLE | SS_LEFT,
                                 0, 0, 10, 10, hwnd, (HMENU)id, g_hInst, nullptr);
        SendMessageW(h, WM_SETFONT, (WPARAM)(bold ? g_fontTitle : g_fontUi), TRUE);
        AddCtl(h);
        return h;
    };
    g_lblNet = mkLbl(L"网络连接", IDC_LBL_NET, true);
    g_lblHisty = mkLbl(L"叫号历史", IDC_LBL_HISTY, true);
    g_lblBase = mkLbl(L"服务器地址 (例: http://IP:host/)", IDC_LBL_BASE, false);
    g_lblRoom = mkLbl(L"房间号(与大屏端一致)", IDC_LBL_ROOM, false);
    g_lblPlace = mkLbl(L"地点:", IDC_LBL_PLACE, false);

    // 集合地点编辑框
    g_edPlace = CreateWindowExW(0, L"EDIT", L"",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                0, 0, 10, 10, hwnd, (HMENU)IDC_ED_PLACE, g_hInst, nullptr);
    AddCtl(g_edPlace);
    // "大屏显示班级/备注"复选框
    g_ckCls = CreateWindowExW(0, L"BUTTON", L"大屏显示班级/备注",
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                              0, 0, 10, 10, hwnd, (HMENU)IDC_CK_CLS, g_hInst, nullptr);
    AddCtl(g_ckCls);

    // 叫号历史列表框
    g_hist = CreateWindowExW(0, L"LISTBOX", L"",
                             WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOINTEGRALHEIGHT,
                             0, 0, 10, 10, hwnd, (HMENU)IDC_HIST, g_hInst, nullptr);
    AddCtl(g_hist);

    // 底部状态栏标签
    g_status = CreateWindowExW(0, L"STATIC", L"就绪",
                               WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
                               0, 0, 10, 10, hwnd, (HMENU)IDC_STATUS, g_hInst, nullptr);
    AddCtl(g_status);

    // 从 INI 恢复上次的网络配置
    std::wstring base = IniGet(L"net", L"relay_base", EC_DEFAULT_RELAY);
    SetWindowTextW(g_edBase, base.c_str());
    SetWindowTextW(g_edRoom, IniGet(L"net", L"room", L"101").c_str());
    SetWindowTextW(g_edPlace, IniGet(L"net", L"place", L"台前").c_str());
    SendMessageW(g_ckCls, BM_SETCHECK,
                 IniGet(L"net", L"show_cls", L"1") == L"1" ? BST_CHECKED : BST_UNCHECKED, 0);
    std::wstring host = IniGet(L"net", L"lan_host", L"");
    if (!host.empty()) {
        SendMessageW(g_comboBoard, CB_ADDSTRING, 0, (LPARAM)host.c_str());   // 上次连接过的大屏地址
        SendMessageW(g_comboBoard, CB_SETCURSEL, 0, 0);
    }
    // 统一设置字体(列表/历史除外)
    for (HWND c : g_controls) {
        if (c != g_lv && c != g_hist)
            SendMessageW(c, WM_SETFONT, (WPARAM)g_fontUi, TRUE);
    }
    SendMessageW(g_btnCall, WM_SETFONT, (WPARAM)g_fontBold, TRUE);   // [叫号]用加粗大字体
}

// ---------------- 窗口过程 ----------------
// 功能: 主窗口过程: 初始化/绘制/自绘按钮/命令分发/定时器/后台消息/退出清理
// 参数: hwnd 窗口句柄; msg 消息; wp/lp 消息参数
// 返回: 按 Win32 窗口过程约定
static LRESULT CALLBACK TeacherProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_hwnd = hwnd;
        // 探测屏幕 DPI
        HDC dc = GetDC(nullptr);
        g_dpi = g_forceDpi > 0 ? g_forceDpi : GetDeviceCaps(dc, LOGPIXELSY);
        ReleaseDC(nullptr, dc);
        // 创建三套字体: 常规UI/加粗(叫号按钮)/半粗标题(面板小标题)
        g_fontUi = CreateFontW(-MulDiv(10, g_dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        g_fontBold = CreateFontW(-MulDiv(13, g_dpi, 72), 0, 0, 0, FW_BOLD, 0, 0, 0,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        g_fontTitle = CreateFontW(-MulDiv(11, g_dpi, 72), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        CreateControls(hwnd);
        LoadStudents();      // 载入名单
        RefreshListView();
        ChatLoad();          // 载入历史对话
        Layout();
        SetTimer(hwnd, 1, 1000, nullptr);    // 定时器1: 每秒刷新状态栏
        SetTimer(hwnd, 2, 10000, nullptr);   // 定时器2: 每10秒发 PING 保活(直连模式)
        if (!g_uiSnapPath.empty()) SetTimer(hwnd, 77, 1500, nullptr);   // 定时器77: 自截图后退出
        UpdateStatus();
        return 0;
    }
    case WM_SIZE:
        Layout();   // 窗口大小变化时重排控件
        return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)lp;
        mmi->ptMinTrackSize.x = S(1000);   // 最小可缩尺寸
        mmi->ptMinTrackSize.y = S(620);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;   // 不让系统擦背景(WM_PAINT 里自绘, 防闪烁)
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HBRUSH br = CreateSolidBrush(C_BG);   // 统一浅灰背景
        FillRect(dc, &rc, br);
        DeleteObject(br);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_PRINT: {
        // 支持 WM_PRINT: 供 DoUiSnap 把整窗(含子控件)绘制到内存 DC 截屏
        HDC dc = (HDC)wp;
        RECT rc;
        GetClientRect(hwnd, &rc);
        HBRUSH br = CreateSolidBrush(C_BG);
        FillRect(dc, &rc, br);
        DeleteObject(br);
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
                // 自绘按钮需用 DrawFluentButton 手动重画
                bool isFluent = cid == IDC_BTN_IMPORT || cid == IDC_BTN_MANUAL ||
                                cid == IDC_BTN_SELALL || cid == IDC_BTN_SELNONE ||
                                cid == IDC_BTN_INVERT || cid == IDC_BTN_DEL ||
                                cid == IDC_BTN_CHAT || cid == IDC_BTN_CALL ||
                                cid == IDC_BTN_CLEAR || cid == IDC_BTN_BLACK ||
                                cid == IDC_BTN_SCAN || cid == IDC_BTN_TEST;
                if (isFluent) {
                    wchar_t txt[128];
                    GetWindowTextW(c, txt, 128);
                    RECT r = { ox, oy, ox + (cr.right - cr.left), oy + (cr.bottom - cr.top) };
                    bool isPrimary = (cid == IDC_BTN_CALL);
                    DrawFluentButton(p->dc, r, txt, isPrimary, false, false, false,
                                     isPrimary ? g_fontBold : g_fontUi);
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
            bool primary = (GetDlgCtrlID(d->hwndItem) == IDC_BTN_CALL);
            DrawFluentButton(d->hDC, d->rcItem, txt, primary,
                             g_hoverBtn == d->hwndItem,                     // 悬停
                             (d->itemState & ODS_SELECTED) != 0,             // 按下
                             (d->itemState & ODS_DISABLED) != 0,             // 禁用
                             primary ? g_fontBold : g_fontUi);
            return TRUE;
        }
        break;
    }
    case WM_CTLCOLORSTATIC: {
        // 静态标签: 透明背景+次要文字色, 与窗口底色一致
        HDC dc = (HDC)wp;
        SetTextColor(dc, C_TEXT2);
        SetBkMode(dc, TRANSPARENT);
        static HBRUSH brFace = nullptr;
        if (!brFace) brFace = CreateSolidBrush(C_BG);
        return (LRESULT)brFace;
    }
    case WM_TIMER:
        if (wp == 77) {
            // --ui 调试: 截图并退出
            DoUiSnap();
            KillTimer(hwnd, 77);
            PostQuitMessage(0);
            return 0;
        }
        if (wp == 1) UpdateStatus();   // 每秒刷新状态栏
        else if (wp == 2) {
            // 每10秒: 直连模式下发 PING 保活, 失败即断开(等待下次重连)
            if (!IsRelayMode() && g_sock != INVALID_SOCKET) {
                if (!TcpSendFrame(g_sock, "PING")) CloseLan();
            }
        }
        return 0;
    case WM_APP_CHAT: {
        // 网络线程投递的 CHAT 帧(heap 上 new 的 string, 所有权转移)
        std::unique_ptr<std::string> f((std::string*)lp);
        OnChatFrame(*f);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        int code = HIWORD(wp);
        // 编辑类控件的即时保存(改地址/地点即写 INI)
        if (id == IDC_COMBO_BOARD && code == CBN_EDITCHANGE)
            IniSet(L"net", L"lan_host", WinText(g_comboBoard));
        if (id == IDC_ED_PLACE && code == EN_CHANGE)
            IniSet(L"net", L"place", TrimW(WinText(g_edPlace)));
        if (code != BN_CLICKED && code != CBN_EDITCHANGE && code != EN_CHANGE) break;
        switch (id) {
        case IDC_BTN_IMPORT: DoImport(hwnd); break;
        case IDC_BTN_MANUAL: ShowAddDialog(); break;
        case IDC_BTN_CHAT: EnsureChatWindow(); break;
        case IDC_BTN_SELALL: SetAllChecked(true); break;
        case IDC_BTN_SELNONE: SetAllChecked(false); break;
        case IDC_BTN_INVERT: InvertChecked(); break;
        case IDC_BTN_DEL: {
            // 删除选中: 保留未勾选行, 重建名单
            int n = ListView_GetItemCount(g_lv);
            std::vector<Student> keep;
            for (int i = 0; i < n; i++)
                if (!IsChecked(i) && (size_t)i < g_students.size())
                    keep.push_back(g_students[i]);
            g_students = keep;
            RefreshListView();
            SaveStudents();
            break;
        }
        case IDC_BTN_CALL: OnCall(hwnd); break;
        case IDC_BTN_CLEAR: OnClear(hwnd); break;
        case IDC_BTN_BLACK: OnBlack(hwnd); break;
        case IDC_BTN_SCAN: DoScan(); break;
        case IDC_BTN_TEST: DoTestServer(hwnd); break;
        case IDC_CK_CLS:
            // 勾选"大屏显示班级": 写 INI 并更新 g_showCls
            IniSet(L"net", L"show_cls",
                   SendMessageW(g_ckCls, BM_GETCHECK, 0, 0) == BST_CHECKED ? L"1" : L"0");
            g_showCls = (SendMessageW(g_ckCls, BM_GETCHECK, 0, 0) == BST_CHECKED);
            break;
        case IDC_RB_LAN:
        case IDC_RB_RELAY:
            // 切换网络模式: 保存选择; 直连转中转时断开 TCP; 重置心跳与消息序号; 重布局
            IniSet(L"net", L"mode", IsRelayMode() ? L"relay" : L"lan");
            if (IsRelayMode()) CloseLan();
            InterlockedExchange(&g_presenceSec, -1);
            InterlockedExchange(&g_chatSeq, 0);
            Layout();
            UpdateStatus();
            break;
        }
        return 0;
    }
    case WM_DESTROY:
        // 退出清理: 停线程 -> 断连接 -> 停定时器 -> 保存配置与名单 -> 清对话记录
        g_stop.store(true);
        CloseLan();
        KillTimer(hwnd, 1);
        KillTimer(hwnd, 2);
        HttpAbortCurrent();   // 让卡在长轮询的线程尽快退出
        if (g_presenceThread.joinable()) g_presenceThread.join();
        if (g_lanRecvThread.joinable()) g_lanRecvThread.join();
        if (g_chatFetchThread.joinable()) g_chatFetchThread.join();
        IniSet(L"net", L"relay_base", GetRelayBase());
        IniSet(L"net", L"room", GetRoom());
        IniSet(L"net", L"place", TrimW(WinText(g_edPlace)));
        SaveStudents();
        ChatWipe();   // 关闭教师端后清空上一次对话
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Win11 圆角窗口(不支持则忽略)
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

// 隐藏调试功能: --ui=<png路径> 启动1.5秒后自截图并退出
// 功能: 把主窗口(含子控件)绘制到内存位图, 存为 PNG(经 ec_net 的 SavePng32)
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
}

// ================================================================
// 主函数: 教师端程序入口
// ----------------------------------------------------------------
// 执行流程分段说明:
//   ① 初始化: 解析命令行(--ui= 调试截图) -> 设置 DPI 感知 -> 测屏幕 DPI
//      -> 启动网络 -> 初始化公共控件 -> 启动 GDI+ -> 读 INI 配置
//   ② 注册窗口类: 主窗口 TeacherProc / 对话窗口 ChatProc /
//      教师名弹窗 NameDlgProc / 手动添加弹窗 AddDlgProc
//   ③ 创建并显示主窗口(圆角), 随后弹教师名弹窗
//   ④ 教师名弹窗: 模态等待输入(独立消息循环), 期间主窗口禁用;
//      结束后若无名字则用默认"教师", 写入 INI
//   ⑤ 启动后台线程: 心跳查询 / 局域网收帧 / 中转拉取
//   ⑥ 消息循环: 主窗口消息泵, 收到 WM_QUIT 时退出
//   ⑦ 清理: 停网络、删字体、关 GDI+
// ================================================================
// 功能: 教师端入口
// 参数: hInst 实例句柄; (忽略前一实例句柄); lpCmdLine 命令行;
//       nShow 初始显示方式
// 返回: 退出码(0=正常, 1=创建主窗口失败)
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR lpCmdLine, int nShow) {
    g_hInst = hInst;
    // ---- ① 命令行与初始化 ----
    std::wstring cmd(lpCmdLine);
    size_t p = cmd.find(L"--ui=");
    if (p != std::wstring::npos) {
        g_uiSnapPath = cmd.substr(p + 5);   // 截图输出路径
        g_forceDpi = 96;                    // 截图固定 96 DPI 保证结果稳定
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
    ice.dwICC = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES | ICC_BAR_CLASSES;
    InitCommonControlsEx(&ice);
    GdiplusStartupInput gsi;
    ULONG_PTR gdiToken = 0;
    GdiplusStartup(&gdiToken, &gsi, nullptr);

    // 从 INI 读上次的教师名与班级显示开关
    g_teacherName = IniGet(L"net", L"teacher_name", L"教师");
    g_showCls = IniGet(L"net", L"show_cls", L"1") != L"0";

    // ---- ② 注册4个窗口类 ----
    WNDCLASSW wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = TeacherProc;                       // 主窗口
    wc.hInstance = hInst;
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;                          // 背景自绘
    wc.lpszClassName = L"EasyCallTeacherWnd";
    RegisterClassW(&wc);

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = ChatProc;                           // 对话窗口
    wc.hInstance = hInst;
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"EasyCallChatWnd";
    RegisterClassW(&wc);

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = NameDlgProc;                        // 教师名弹窗
    wc.hInstance = hInst;
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"EasyCallNameDlg";
    RegisterClassW(&wc);

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = AddDlgProc;                         // 手动添加弹窗
    wc.hInstance = hInst;
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"EasyCallAddDlg";
    RegisterClassW(&wc);

    // ---- ③ 创建并显示主窗口 ----
    HWND hwnd = CreateWindowExW(0, L"EasyCallTeacherWnd", L"EasyCall 班级叫号系统 - 教师端",
                                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                CW_USEDEFAULT, CW_USEDEFAULT, S(1150), S(720),
                                nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;
    EnableRoundedCorners(hwnd);
    // 主窗口先显示, 教师名弹窗盖在其上(owned); 任何情况下教师端都能打开
    ShowWindow(hwnd, nShow);
    UpdateWindow(hwnd);

    // ---- ④ 教师名弹窗(模态等待) ----
    if (g_uiSnapPath.empty()) {
        RECT mrc;
        GetWindowRect(hwnd, &mrc);
        HWND ndlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"EasyCallNameDlg", L"进入 EasyCall - 教师端",
                                    WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                                    mrc.left + (mrc.right - mrc.left) / 2 - S(170),   // 主窗口居中
                                    mrc.top + (mrc.bottom - mrc.top) / 2 - S(100),
                                    S(340), S(190), hwnd, nullptr, hInst, nullptr);
        if (ndlg) {
            EnableWindow(hwnd, FALSE);   // 禁用主窗口, 形成"模态"效果
            MSG nm;
            while (IsWindow(ndlg)) {
                // 独立消息循环: 直到教师名弹窗销毁为止
                BOOL got = GetMessageW(&nm, nullptr, 0, 0);
                if (got <= 0) {
                    if (got == 0) PostQuitMessage((int)nm.wParam);   // 收到退出消息则转投主循环
                    break;
                }
                TranslateMessage(&nm);
                DispatchMessageW(&nm);
            }
            EnableWindow(hwnd, TRUE);   // 恢复主窗口
            SetForegroundWindow(hwnd);
        }
        if (TrimW(g_teacherName).empty()) g_teacherName = L"教师";   // 兜底默认名
        IniSet(L"net", L"teacher_name", g_teacherName);
    } else {
        g_teacherName = L"测试教师";   // --ui 截图模式用固定名
    }

    UpdateWindow(hwnd);

    // ---- ⑤ 启动后台线程 ----
    g_stop.store(false);
    g_chatSender = TeacherSendChat;
    g_presenceThread = std::thread(PresenceThreadProc);   // 大屏心跳查询(中转)
    g_lanRecvThread = std::thread(LanRecvThreadProc);     // 局域网收帧(直连)
    g_chatFetchThread = std::thread(ChatFetchThreadProc); // 中转消息拉取

    // ---- ⑥ 消息循环 ----
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    // ---- ⑦ 清理 ----
    EcNetStop();
    if (g_fontUi) DeleteObject(g_fontUi);
    if (g_fontBold) DeleteObject(g_fontBold);
    if (g_fontTitle) DeleteObject(g_fontTitle);
    GdiplusShutdown(gdiToken);
    return 0;
}
