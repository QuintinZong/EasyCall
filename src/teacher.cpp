// EasyCall 班级叫号系统 - 教师端 (Fluent/WinUI 界面风格, GDI+ 自绘)
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

struct Student { std::wstring id, name, cls; };

enum : INT_PTR {
    IDC_LV = 100, IDC_BTN_IMPORT, IDC_BTN_SELALL, IDC_BTN_SELNONE, IDC_BTN_INVERT,
    IDC_BTN_DEL, IDC_BTN_CALL, IDC_BTN_CLEAR, IDC_BTN_BLACK,
    IDC_RB_LAN, IDC_RB_RELAY, IDC_BTN_SCAN, IDC_COMBO_BOARD,
    IDC_ED_BASE, IDC_ED_ROOM, IDC_BTN_TEST, IDC_HIST, IDC_STATUS,
    IDC_LBL_BASE, IDC_LBL_ROOM, IDC_LBL_NET, IDC_LBL_HISTY
};

// ---------------- Fluent 配色 ----------------
static const COLORREF C_BG       = RGB(0xF3, 0xF3, 0xF3);
static const COLORREF C_WHITE    = RGB(0xFF, 0xFF, 0xFF);
static const COLORREF C_BORDER   = RGB(0xD1, 0xD1, 0xD1);
static const COLORREF C_ACCENT   = RGB(0x0F, 0x6C, 0xBD);
static const COLORREF C_ACCENT_H = RGB(0x0D, 0x5F, 0xA9);
static const COLORREF C_ACCENT_P = RGB(0x0B, 0x54, 0x95);
static const COLORREF C_BTN_H    = RGB(0xF7, 0xF7, 0xF7);
static const COLORREF C_BTN_P    = RGB(0xE9, 0xE9, 0xE9);
static const COLORREF C_TEXT     = RGB(0x1A, 0x1A, 0x1A);
static const COLORREF C_TEXT2    = RGB(0x60, 0x5E, 0x5C);

static HINSTANCE g_hInst;
static HWND g_hwnd, g_lv, g_hist, g_status;
static HWND g_rbLan, g_rbRelay, g_btnScan, g_comboBoard, g_edBase, g_edRoom, g_btnTest;
static HWND g_btnCall, g_btnBlack;
static HWND g_lblBase, g_lblRoom, g_lblNet, g_lblHisty;
static std::vector<HWND> g_controls;
static std::vector<Student> g_students;
static SOCKET g_sock = INVALID_SOCKET;
static std::atomic<bool> g_stop{false};
static volatile LONG g_presenceSec = -1;
static int g_dpi = 96;
static HFONT g_fontUi = nullptr, g_fontBold = nullptr, g_fontTitle = nullptr;
static HWND g_hoverBtn = nullptr;
static std::thread g_presenceThread;
static std::wstring g_uiSnapPath;   // --ui= 自截图调试功能
static int g_forceDpi = 0;          // --ui 调试时强制 96 DPI
static void DoUiSnap();

static int S(int px) { return MulDiv(px, g_dpi, 96); }

static void AddCtl(HWND h) { g_controls.push_back(h); }

static std::wstring WinText(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::wstring s(n, 0);
    if (n) GetWindowTextW(h, &s[0], n + 1);
    return s;
}
static std::wstring TrimW(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return L"";
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}

static bool IsRelayMode() {
    return SendMessageW(g_rbRelay, BM_GETCHECK, 0, 0) == BST_CHECKED;
}
static std::wstring GetRelayBase() {
    std::wstring s = TrimW(WinText(g_edBase));
    if (!s.empty() && s.back() != L'/') s += L'/';
    return s;
}
static std::wstring GetRoom() { return TrimW(WinText(g_edRoom)); }

static void SetStatus(const std::wstring& t) {
    if (g_status) SetWindowTextW(g_status, t.c_str());
}

// ---------------- 学生名单持久化 ----------------
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
static void LoadStudents() {
    g_students.clear();
    HANDLE h = CreateFileW((ExeDirW() + L"students.json").c_str(), GENERIC_READ,
                           FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD hi = 0;
    DWORD size = GetFileSize(h, &hi);
    if (hi || size == 0 || size > 16 * 1024 * 1024) { CloseHandle(h); return; }
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
static void RefreshListView() {
    ListView_DeleteAllItems(g_lv);
    for (size_t i = 0; i < g_students.size(); i++) {
        LVITEMW it;
        memset(&it, 0, sizeof it);
        it.mask = LVIF_TEXT;
        it.iItem = (int)i;
        it.pszText = (LPWSTR)g_students[i].id.c_str();
        ListView_InsertItem(g_lv, &it);
        ListView_SetItemText(g_lv, (int)i, 1, (LPWSTR)g_students[i].name.c_str());
        ListView_SetItemText(g_lv, (int)i, 2, (LPWSTR)g_students[i].cls.c_str());
    }
}
static bool IsChecked(int i) {
    UINT st = ListView_GetItemState(g_lv, i, LVIS_STATEIMAGEMASK);
    return ((st >> 12) - 1) == 1;
}
static void SetAllChecked(bool c) {
    int n = ListView_GetItemCount(g_lv);
    for (int i = 0; i < n; i++)
        ListView_SetItemState(g_lv, i, INDEXTOSTATEIMAGEMASK(c ? 2 : 1), LVIS_STATEIMAGEMASK);
}
static void InvertChecked() {
    int n = ListView_GetItemCount(g_lv);
    for (int i = 0; i < n; i++)
        ListView_SetItemState(g_lv, i, INDEXTOSTATEIMAGEMASK(IsChecked(i) ? 1 : 2), LVIS_STATEIMAGEMASK);
}
static void HistAdd(const std::wstring& line) {
    SendMessageW(g_hist, LB_ADDSTRING, 0, (LPARAM)line.c_str());
    int n = (int)SendMessageW(g_hist, LB_GETCOUNT, 0, 0);
    if (n > 300) SendMessageW(g_hist, LB_DELETESTRING, 0, 0);
    SendMessageW(g_hist, LB_SETCURSEL, n - 1, 0);
    SendMessageW(g_hist, LB_SETCURSEL, -1, 0);
}

// ---------------- 导入 ----------------
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
    if (!GetOpenFileNameW(&ofn)) return;
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
static bool LanConnect(const std::wstring& hostPort, std::wstring& err) {
    std::wstring hp = TrimW(hostPort);
    size_t sp = hp.find_first_of(L" \t");
    if (sp != std::wstring::npos) hp = hp.substr(0, sp);
    hp = TrimW(hp);
    std::wstring host = hp;
    int port = EC_TCP_PORT;
    size_t c = hp.rfind(L':');
    if (c != std::wstring::npos) {
        std::wstring ps = hp.substr(c + 1);
        if (!ps.empty() && ps.find_first_not_of(L"0123456789") == std::wstring::npos) {
            host = hp.substr(0, c);
            port = _wtoi(ps.c_str());
        }
    }
    if (host.empty()) { err = L"请先填写教室端IP地址"; return false; }
    addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
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
    ioctlsocket(s, FIONBIO, &nb);
    int r = connect(s, (sockaddr*)&sa, sizeof sa);
    if (r == SOCKET_ERROR) {
        int e = WSAGetLastError();
        if (e == WSAEWOULDBLOCK || e == WSAEINPROGRESS || e == WSAEINVAL) {
            fd_set w; FD_ZERO(&w); FD_SET(s, &w);
            timeval tv; tv.tv_sec = 3; tv.tv_usec = 0;
            r = select(0, nullptr, &w, nullptr, &tv);
            if (r <= 0) { closesocket(s); err = L"连接超时: 请确认教室端已启动且与本机处于同一局域网"; return false; }
            int soerr = 0; int slen = sizeof soerr;
            getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&soerr, &slen);
            if (soerr != 0) { closesocket(s); err = L"连接被拒绝(教室端未监听端口 " + std::to_wstring(port) + L")"; return false; }
        } else {
            closesocket(s);
            err = L"连接失败";
            return false;
        }
    }
    nb = 0;
    ioctlsocket(s, FIONBIO, &nb);
    g_sock = s;
    return true;
}
static void CloseLan() {
    if (g_sock != INVALID_SOCKET) { closesocket(g_sock); g_sock = INVALID_SOCKET; }
}

// ---------------- 发送 ----------------
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
            std::wstring host = WinText(g_comboBoard);
            if (host.empty()) { errOut = L"请先点击[扫描教室]或手动填写教室端IP地址"; return false; }
            std::wstring e2;
            if (!LanConnect(host, e2)) { errOut = e2; return false; }
            IniSet(L"net", L"lan_host", TrimW(host));
        }
        if (!TcpSendFrame(g_sock, payload)) {
            CloseLan();
            errOut = L"发送失败, 与教室端的连接已断开";
            return false;
        }
        return true;
    }
}

// ---------------- 叫号 / 清屏 / 黑屏 ----------------
static void OnCall(HWND hwnd) {
    std::vector<std::wstring> items;
    int n = ListView_GetItemCount(g_lv);
    for (int i = 0; i < n; i++) {
        if (!IsChecked(i)) continue;
        if ((size_t)i >= g_students.size()) continue;
        items.push_back(g_students[i].id + L"\t" + g_students[i].name + L"\t" + g_students[i].cls);
    }
    if (items.empty()) {
        MessageBoxW(hwnd, L"请先勾选要叫号的学生(可勾选多个)", L"提示", MB_ICONINFORMATION);
        return;
    }
    std::string callId;
    std::string payload = BuildCallPayload(items, &callId);
    std::wstring err;
    if (!SendToBoard(payload, err)) {
        MessageBoxW(hwnd, err.c_str(), L"叫号发送失败", MB_ICONWARNING);
        return;
    }
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
    HistAdd(NowTimeW() + L" 叫号 " + std::to_wstring(items.size()) + L" 人: " + names);
    SetAllChecked(false);
}
static void OnClear(HWND hwnd) {
    std::wstring err;
    if (!SendToBoard("CLEAR", err)) {
        MessageBoxW(hwnd, err.c_str(), L"清屏发送失败", MB_ICONWARNING);
        return;
    }
    HistAdd(NowTimeW() + L" 已发送清屏");
}
static void OnBlack(HWND hwnd) {
    std::wstring err;
    if (!SendToBoard("BLACK", err)) {
        MessageBoxW(hwnd, err.c_str(), L"发送失败", MB_ICONWARNING);
        return;
    }
    HistAdd(NowTimeW() + L" 已发送一键黑屏(大屏居中显示\"保持安静\")");
}

// ---------------- 扫描 ----------------
static void DoScan() {
    SetStatus(L"正在扫描局域网中的教室大屏(3秒)…");
    std::vector<BoardInfo> boards;
    int n = DiscoverBoards(boards, 3000);
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

// ---------------- 服务器测试 ----------------
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
        long sec = _wtol(U8W(pr).c_str());
        msg += sec >= 0 && sec <= 25 ? L"\n教室大屏: 在线" : L"\n教室大屏: 离线(未上报心跳, 请确认大屏端已运行且房间号一致)";
    }
    MessageBoxW(hwnd, msg.c_str(), L"测试结果", MB_ICONINFORMATION);
    SetStatus(L"中转服务器测试完成");
}

// ---------------- 状态 ----------------
static void UpdateStatus() {
    wchar_t buf[512];
    if (IsRelayMode()) {
        std::wstring on;
        if (g_presenceSec < 0) on = L"未知";
        else if (g_presenceSec <= 25) on = L"在线";
        else on = L"离线";
        swprintf(buf, 512, L"模式: 服务器中转 | 大屏: %s | 学生: %d 人", on.c_str(), (int)g_students.size());
    } else {
        std::wstring st = (g_sock != INVALID_SOCKET) ? WinText(g_comboBoard) : L"未连接";
        swprintf(buf, 512, L"模式: 局域网直连 | 教室端: %s | 学生: %d 人", st.c_str(), (int)g_students.size());
    }
    SetStatus(buf);
}
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
                InterlockedExchange(&g_presenceSec, (LONG)sec);
            }
        }
        Sleep(6000);
    }
}

// ---------------- Fluent 按钮(自绘) ----------------
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
static void DrawRoundedPath(Graphics& g, const RECT& rc, int rad, const SolidBrush& fill, const Pen& pen) {
    int w = rc.right - rc.left - 1, h = rc.bottom - rc.top - 1;
    int d = rad * 2;
    GraphicsPath path;
    path.AddArc(rc.left, rc.top, d, d, 180, 90);
    path.AddArc(rc.left + w - d, rc.top, d, d, 270, 90);
    path.AddArc(rc.left + w - d, rc.top + h - d, d, d, 0, 90);
    path.AddArc(rc.left, rc.top + h - d, d, d, 90, 90);
    path.CloseFigure();
    g.FillPath(&fill, &path);
    g.DrawPath(&pen, &path);
}
static void DrawFluentButton(HDC dc, const RECT& rc, const wchar_t* text,
                             bool primary, bool hover, bool pressed, bool disabled, HFONT font) {
    Graphics g(dc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    {
        SolidBrush bg(FluentColor(C_BG));   // 圆角以外的角落铺上窗口背景色
        g.FillRectangle(&bg, (INT)rc.left, (INT)rc.top,
                        (INT)(rc.right - rc.left), (INT)(rc.bottom - rc.top));
    }
    int rad = 4;
    SolidBrush fill(
        disabled ? FluentColor(RGB(0xF4, 0xF4, 0xF4)) :
        primary  ? FluentColor(pressed ? C_ACCENT_P : hover ? C_ACCENT_H : C_ACCENT) :
                   FluentColor(pressed ? C_BTN_P : hover ? C_BTN_H : C_WHITE));
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
static void MoveCtl(int id, int x, int y, int w, int h) {
    HWND c = GetDlgItem(g_hwnd, id);
    if (c) MoveWindow(c, x, y, w, h, TRUE);
}
static void ShowCtl(int id, bool show) {
    HWND c = GetDlgItem(g_hwnd, id);
    if (c) ShowWindow(c, show ? SW_SHOW : SW_HIDE);
}

static void Layout() {
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    int M = S(14);
    int y = S(14), h1 = S(34);
    MoveCtl(IDC_BTN_IMPORT, M, y, S(108), h1);
    MoveCtl(IDC_BTN_SELALL, M + S(116), y, S(62), h1);
    MoveCtl(IDC_BTN_SELNONE, M + S(186), y, S(74), h1);
    MoveCtl(IDC_BTN_INVERT, M + S(268), y, S(62), h1);
    MoveCtl(IDC_BTN_DEL, M + S(338), y, S(88), h1);

    int y2 = y + h1 + S(10), h2 = S(44);
    MoveCtl(IDC_BTN_CALL, M, y2, S(138), h2);
    MoveCtl(IDC_BTN_CLEAR, M + S(146), y2, S(106), h2);
    MoveCtl(IDC_BTN_BLACK, M + S(260), y2, S(106), h2);

    int mainTop = y2 + h2 + S(16);
    int mainBottom = rc.bottom - S(34) - S(8);
    int panelW = S(310);
    int panelX = rc.right - panelW - M;

    MoveCtl(IDC_LV, M, mainTop, panelX - M - S(12), mainBottom - mainTop);
    MoveCtl(IDC_STATUS, M, rc.bottom - S(28), rc.right - M - M, S(20));

    bool relay = IsRelayMode();
    int cY = mainTop;
    MoveCtl(IDC_LBL_NET, panelX, cY + S(2), panelW, S(22));
    MoveCtl(IDC_RB_LAN, panelX + S(6), cY + S(28), S(130), S(24));
    MoveCtl(IDC_RB_RELAY, panelX + S(6), cY + S(54), S(210), S(24));
    // 局域网行(仅局域网模式显示)
    ShowCtl(IDC_COMBO_BOARD, !relay);
    ShowCtl(IDC_BTN_SCAN, !relay);
    if (!relay) {
        MoveCtl(IDC_COMBO_BOARD, panelX + S(6), cY + S(86), panelW - S(6) - S(10) - S(86), S(26));
        MoveCtl(IDC_BTN_SCAN, panelX + panelW - S(90), cY + S(85), S(84), S(28));
    }
    // 中转行(仅中转模式显示)
    ShowCtl(IDC_LBL_BASE, relay);
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
    MoveCtl(IDC_HIST, panelX + S(6), cY + S(210), panelW - S(12), mainBottom - cY - S(210) - S(4));
}

// ---------------- 控件创建 ----------------
static void CreateControls(HWND hwnd) {
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

    struct BtnDef { const wchar_t* text; INT_PTR id; };
    static const BtnDef btns[] = {
        { L"导入Excel", IDC_BTN_IMPORT }, { L"全选", IDC_BTN_SELALL },
        { L"全不选", IDC_BTN_SELNONE }, { L"反选", IDC_BTN_INVERT },
        { L"删除选中", IDC_BTN_DEL }, { L"叫 号", IDC_BTN_CALL },
        { L"发送清屏", IDC_BTN_CLEAR }, { L"一键黑屏", IDC_BTN_BLACK },
        { L"扫描教室", IDC_BTN_SCAN }, { L"测试连接", IDC_BTN_TEST }
    };
    for (auto& b : btns) {
        HWND h = CreateWindowExW(0, L"BUTTON", b.text,
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                 0, 0, 10, 10, hwnd, (HMENU)b.id, g_hInst, nullptr);
        SubmitButton(h);
        AddCtl(h);
        if (b.id == IDC_BTN_CALL) g_btnCall = h;
        if (b.id == IDC_BTN_BLACK) g_btnBlack = h;
        if (b.id == IDC_BTN_SCAN) g_btnScan = h;
        if (b.id == IDC_BTN_TEST) g_btnTest = h;
    }
    g_rbLan = CreateWindowExW(0, L"BUTTON", L"局域网直连",
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON | WS_GROUP,
                              0, 0, 10, 10, hwnd, (HMENU)IDC_RB_LAN, g_hInst, nullptr);
    g_rbRelay = CreateWindowExW(0, L"BUTTON", L"服务器中转(跨网络)",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON,
                                0, 0, 10, 10, hwnd, (HMENU)IDC_RB_RELAY, g_hInst, nullptr);
    if (IniGet(L"net", L"mode", L"lan") == L"relay")
        SendMessageW(g_rbRelay, BM_SETCHECK, BST_CHECKED, 0);
    else
        SendMessageW(g_rbLan, BM_SETCHECK, BST_CHECKED, 0);
    g_comboBoard = CreateWindowExW(0, L"COMBOBOX", L"",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWN | WS_VSCROLL,
                                   0, 0, 10, 200, hwnd, (HMENU)IDC_COMBO_BOARD, g_hInst, nullptr);
    g_edBase = CreateWindowExW(0, L"EDIT", L"",
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                               0, 0, 10, 10, hwnd, (HMENU)IDC_ED_BASE, g_hInst, nullptr);
    g_edRoom = CreateWindowExW(0, L"EDIT", L"",
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                               0, 0, 10, 10, hwnd, (HMENU)IDC_ED_ROOM, g_hInst, nullptr);
    AddCtl(g_comboBoard); AddCtl(g_edBase); AddCtl(g_edRoom);

    auto mkLbl = [&](const wchar_t* t, INT_PTR id, bool bold) -> HWND {
        HWND h = CreateWindowExW(0, L"STATIC", t, WS_CHILD | WS_VISIBLE | SS_LEFT,
                                 0, 0, 10, 10, hwnd, (HMENU)id, g_hInst, nullptr);
        SendMessageW(h, WM_SETFONT, (WPARAM)(bold ? g_fontTitle : g_fontUi), TRUE);
        AddCtl(h);
        return h;
    };
    g_lblNet = mkLbl(L"网络连接", IDC_LBL_NET, true);
    g_lblHisty = mkLbl(L"叫号历史", IDC_LBL_HISTY, true);
    g_lblBase = mkLbl(L"服务器地址 (例: http://IP:端口/)", IDC_LBL_BASE, false);
    g_lblRoom = mkLbl(L"房间号(与大屏端一致)", IDC_LBL_ROOM, false);

    g_hist = CreateWindowExW(0, L"LISTBOX", L"",
                             WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOINTEGRALHEIGHT,
                             0, 0, 10, 10, hwnd, (HMENU)IDC_HIST, g_hInst, nullptr);
    AddCtl(g_hist);

    g_status = CreateWindowExW(0, L"STATIC", L"就绪",
                               WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
                               0, 0, 10, 10, hwnd, (HMENU)IDC_STATUS, g_hInst, nullptr);
    AddCtl(g_status);

    std::wstring base = IniGet(L"net", L"relay_base", EC_DEFAULT_RELAY);
    SetWindowTextW(g_edBase, base.c_str());
    SetWindowTextW(g_edRoom, IniGet(L"net", L"room", L"101").c_str());
    std::wstring host = IniGet(L"net", L"lan_host", L"");
    if (!host.empty()) {
        SendMessageW(g_comboBoard, CB_ADDSTRING, 0, (LPARAM)host.c_str());
        SendMessageW(g_comboBoard, CB_SETCURSEL, 0, 0);
    }
    for (HWND c : g_controls) {
        if (c != g_lv && c != g_hist)
            SendMessageW(c, WM_SETFONT, (WPARAM)g_fontUi, TRUE);
    }
    SendMessageW(g_btnCall, WM_SETFONT, (WPARAM)g_fontBold, TRUE);
}

// ---------------- 窗口过程 ----------------
static LRESULT CALLBACK TeacherProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_hwnd = hwnd;
        HDC dc = GetDC(nullptr);
        g_dpi = g_forceDpi > 0 ? g_forceDpi : GetDeviceCaps(dc, LOGPIXELSY);
        ReleaseDC(nullptr, dc);
        const wchar_t* ed = _wgetenv(L"EC_FORCE_DPI");
        if (ed && _wtoi(ed) > 0) g_dpi = _wtoi(ed);   // 开发验证用: 强制 DPI
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
        LoadStudents();
        RefreshListView();
        Layout();
        SetTimer(hwnd, 1, 1000, nullptr);
        SetTimer(hwnd, 2, 10000, nullptr);
        if (!g_uiSnapPath.empty()) SetTimer(hwnd, 77, 1500, nullptr);
        UpdateStatus();
        return 0;
    }
    case WM_SIZE:
        Layout();
        return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)lp;
        mmi->ptMinTrackSize.x = S(1000);
        mmi->ptMinTrackSize.y = S(620);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;   // 背景统一在 WM_PAINT 绘制
    case WM_PRINT: {   // 供自截图/其他捕获使用
        HDC dc = (HDC)wp;
        RECT rc;
        GetClientRect(hwnd, &rc);
        HBRUSH br = CreateSolidBrush(C_BG);
        FillRect(dc, &rc, br);
        DeleteObject(br);
        if (lp & PRF_CHILDREN) {
            struct Ctx { HDC dc; POINT org; };
            Ctx ctx;
            ctx.dc = dc;
            POINT cOrg = { 0, 0 };
            MapWindowPoints(hwnd, nullptr, &cOrg, 1);   // 客户端原点(屏幕坐标)
            ctx.org = cOrg;
            EnumChildWindows(hwnd, [](HWND c, LPARAM l) -> BOOL {
                Ctx* p = (Ctx*)l;
                RECT cr;
                GetWindowRect(c, &cr);
                int ox = cr.left - p->org.x;
                int oy = cr.top - p->org.y;
                // 自绘按钮: 直接绘制(避免 comctl 主题在 WM_PRINT 时覆盖自绘效果)
                int cid = GetDlgCtrlID(c);
                bool isFluent = cid == IDC_BTN_IMPORT || cid == IDC_BTN_SELALL ||
                                cid == IDC_BTN_SELNONE || cid == IDC_BTN_INVERT ||
                                cid == IDC_BTN_DEL || cid == IDC_BTN_CALL ||
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
                SetViewportOrgEx(p->dc, ox, oy, nullptr);
                SendMessageW(c, WM_PRINT, (WPARAM)p->dc,
                             PRF_CLIENT | PRF_ERASEBKGND | PRF_NONCLIENT | PRF_CHILDREN);
                SetViewportOrgEx(p->dc, 0, 0, nullptr);
                return TRUE;
            }, (LPARAM)&ctx);
        }
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HBRUSH br = CreateSolidBrush(C_BG);
        FillRect(dc, &rc, br);
        DeleteObject(br);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT* d = (DRAWITEMSTRUCT*)lp;
        if (d->CtlType == ODT_BUTTON) {
            wchar_t txt[128];
            GetWindowTextW(d->hwndItem, txt, 128);
            bool primary = (GetDlgCtrlID(d->hwndItem) == IDC_BTN_CALL);
            DrawFluentButton(d->hDC, d->rcItem, txt, primary,
                             g_hoverBtn == d->hwndItem,
                             (d->itemState & ODS_SELECTED) != 0,
                             (d->itemState & ODS_DISABLED) != 0,
                             primary ? g_fontBold : g_fontUi);
            return TRUE;
        }
        break;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        SetTextColor(dc, C_TEXT2);
        SetBkMode(dc, TRANSPARENT);
        static HBRUSH brFace = nullptr;
        if (!brFace) brFace = CreateSolidBrush(C_BG);
        return (LRESULT)brFace;
    }
    case WM_TIMER:
        if (wp == 77) {
            DoUiSnap();
            KillTimer(hwnd, 77);
            PostQuitMessage(0);
            return 0;
        }
        if (wp == 1) UpdateStatus();
        else if (wp == 2) {
            if (!IsRelayMode() && g_sock != INVALID_SOCKET) {
                if (!TcpSendFrame(g_sock, "PING")) CloseLan();
            }
        }
        return 0;
    case WM_COMMAND: {
        int id = LOWORD(wp);
        int code = HIWORD(wp);
        if (id == IDC_COMBO_BOARD && code == CBN_EDITCHANGE)
            IniSet(L"net", L"lan_host", WinText(g_comboBoard));
        if (code != BN_CLICKED && code != CBN_EDITCHANGE) break;
        switch (id) {
        case IDC_BTN_IMPORT: DoImport(hwnd); break;
        case IDC_BTN_SELALL: SetAllChecked(true); break;
        case IDC_BTN_SELNONE: SetAllChecked(false); break;
        case IDC_BTN_INVERT: InvertChecked(); break;
        case IDC_BTN_DEL: {
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
        case IDC_RB_LAN:
        case IDC_RB_RELAY:
            IniSet(L"net", L"mode", IsRelayMode() ? L"relay" : L"lan");
            if (IsRelayMode()) CloseLan();
            InterlockedExchange(&g_presenceSec, -1);
            Layout();   // 重新布局以显示/隐藏对应控件
            UpdateStatus();
            break;
        }
        return 0;
    }
    case WM_DESTROY:
        g_stop.store(true);
        CloseLan();
        KillTimer(hwnd, 1);
        KillTimer(hwnd, 2);
        if (g_presenceThread.joinable()) g_presenceThread.join();
        IniSet(L"net", L"relay_base", GetRelayBase());
        IniSet(L"net", L"room", GetRoom());
        SaveStudents();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Win11 圆角窗口(不支持则忽略)
static void EnableRoundedCorners(HWND h) {
    HMODULE d = LoadLibraryW(L"dwmapi.dll");
    if (!d) return;
    typedef HRESULT(WINAPI* Fn)(HWND, DWORD, LPCVOID, DWORD);
    Fn f = (Fn)GetProcAddress(d, "DwmSetWindowAttribute");
    if (f) {
        int pref = 2;   // DWMWCP_ROUND
        f(h, 33, &pref, sizeof pref);
    }
}

// 隐藏调试功能: --ui=<png路径> 启动1.5秒后自截图并退出 (便于开发验证界面)
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
        std::string msg = "ok=" + std::to_string((int)ok) + " bits=" + std::to_string((int)(bits != nullptr)) +
                          " dpi=" + std::to_string(g_dpi) + "\n";
        auto dump = [&](const wchar_t* name, HWND c) {
            RECT r;
            GetWindowRect(c, &r);
            msg += std::string("  ") + WU8(name) + "=(" + std::to_string(r.left) + "," +
                   std::to_string(r.top) + "," + std::to_string(r.right) + "," + std::to_string(r.bottom) + ")\n";
        };
        dump(L"hist", g_hist);
        dump(L"status", g_status);
        dump(L"lv", g_lv);
        dump(L"callBtn", g_btnCall);
        msg += std::string("  callCid=") + std::to_string(GetDlgCtrlID(g_btnCall)) + "\n";
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
        g_uiSnapPath = cmd.substr(p + 5);
        g_forceDpi = 96;   // 调试截图使用标准 DPI, 保证与真实设备一致
    }
    SetProcessDPIAware();
    EcNetStart();
    INITCOMMONCONTROLSEX ice;
    ice.dwSize = sizeof ice;
    ice.dwICC = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES | ICC_BAR_CLASSES;
    InitCommonControlsEx(&ice);
    GdiplusStartupInput gsi;
    ULONG_PTR gdiToken = 0;
    GdiplusStartup(&gdiToken, &gsi, nullptr);

    WNDCLASSW wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = TeacherProc;
    wc.hInstance = hInst;
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"EasyCallTeacherWnd";
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(0, L"EasyCallTeacherWnd", L"EasyCall 班级叫号系统 - 教师端",
                                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                CW_USEDEFAULT, CW_USEDEFAULT, S(1150), S(720),
                                nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;
    EnableRoundedCorners(hwnd);
    ShowWindow(hwnd, nShow);
    UpdateWindow(hwnd);

    g_stop.store(false);
    g_presenceThread = std::thread(PresenceThreadProc);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    EcNetStop();
    if (g_fontUi) DeleteObject(g_fontUi);
    if (g_fontBold) DeleteObject(g_fontBold);
    if (g_fontTitle) DeleteObject(g_fontTitle);
    GdiplusShutdown(gdiToken);
    return 0;
}
