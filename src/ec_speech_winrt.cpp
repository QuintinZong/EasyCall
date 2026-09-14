// ============================================================
// EasyCall 教室端 - 语音朗读实现(仅 MSVC 构建)
// 引擎: SpeechSynthesizer(系统自然语音, 优先中文) -> MediaPlayer 无窗口播放
// 线程模型: 独立后台线程 + MTA; 阻塞式 .get() 等待合成, 无需协程
// 命令队列: Say(打断上一条)/ Stop(停止)/ Quit(退出)
// ============================================================
#include "ec_speech_winrt.h"

#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <sstream>
#include <iomanip>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Media.Core.h>
#include <winrt/Windows.Media.Playback.h>
#include <winrt/Windows.Media.SpeechSynthesis.h>

using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Media::Core;
using namespace winrt::Windows::Media::Playback;
using namespace winrt::Windows::Media::SpeechSynthesis;

// 状态栏消息: 与 board.cpp 的 WM_APP_STATUS 保持一致
#define WM_APP_SPEECH_STATUS (WM_APP + 3)

namespace {
enum class Cmd { None, Say, Stop, Quit };

std::thread g_thread;                       // 朗读线程
std::mutex g_mtx;                           // 命令互斥
std::condition_variable g_cv;               // 命令通知
Cmd g_cmd = Cmd::None;                      // 当前待处理命令
std::wstring g_pendingText;                 // 待朗读文本(Say 命令)
HWND g_statusWnd = nullptr;                 // 状态栏消息目标窗口
std::atomic<bool> g_started{false};         // 线程是否已创建
std::atomic<bool> g_reported{false};        // 失败提示只发一次

void PostStatus(const std::wstring& s) {
    if (g_statusWnd) PostMessageW(g_statusWnd, WM_APP_SPEECH_STATUS, 0, (LPARAM)new std::wstring(s));
}

// 功能: HRESULT 转 8 位十六进制字符串(状态提示用)
// 参数: hr 错误码
// 返回: 如 "80070005"
std::wstring HrHex(HRESULT hr) {
    std::wostringstream ss;
    ss << std::hex << std::uppercase << std::setfill(L'0') << std::setw(8) << (unsigned long)(long)hr;
    return ss.str();
}

// 功能: 朗读线程主体: 初始化引擎 -> 循环处理 Say/Stop/Quit
// 参数: 无
// 返回: 无
void SpeechThread() {
    // MTA 公寓: 与 WinToast 的 STA 互不干扰; init 抛异常时必须捕获(线程内未捕获异常=进程崩溃)
    try {
        winrt::init_apartment(apartment_type::multi_threaded);
    } catch (...) {
        PostStatus(L"语音初始化失败(RoInitialize)");
        return;
    }
    try {
        SpeechSynthesizer synth;
        // 优先选中文语音(如晓晓/云希), 没有则用系统默认
        {
            bool picked = false;
            for (auto&& v : synth.AllVoices()) {
                std::wstring lang = v.Language().c_str();
                if (lang.rfind(L"zh", 0) == 0) { synth.Voice(v); picked = true; break; }
            }
            if (!picked) PostStatus(L"提示: 系统未安装中文语音, 将使用默认语音");
        }
        MediaPlayer player;   // 无窗口纯音频播放器

        for (;;) {
            Cmd c = Cmd::None;
            std::wstring text;
            {
                std::unique_lock<std::mutex> lk(g_mtx);
                g_cv.wait(lk, [] { return g_cmd != Cmd::None; });
                c = g_cmd;
                text.swap(g_pendingText);
                g_cmd = Cmd::None;
            }
            if (c == Cmd::Quit) break;
            if (c == Cmd::Stop) {
                // 停止: 暂停并清空音源
                try { player.Pause(); player.Source(nullptr); } catch (...) {}
                continue;
            }
            if (c == Cmd::Say) {
                // 新叫号: 先打断上一条, 再合成+播放本条
                try { player.Pause(); player.Source(nullptr); } catch (...) {}
                try {
                    SpeechSynthesisStream stream = synth.SynthesizeTextToStreamAsync(text).get();
                    if (stream) {
                        player.Source(MediaSource::CreateFromStream(stream, stream.ContentType()));
                        player.Play();
                    }
                } catch (...) {
                    // 合成失败(缺语音包/无音频设备等): 提示一次, 不影响叫号显示
                    if (!g_reported.exchange(true))
                        PostStatus(L"语音朗读失败(系统可能缺少语音包), 已静默跳过");
                }
            }
        }
        try { player.Source(nullptr); } catch (...) {}
    } catch (hresult_error const& e) {
        PostStatus(L"语音初始化失败(0x" + HrHex(e.code()) + L")");
    } catch (...) {
        PostStatus(L"语音初始化失败");
    }
    winrt::uninit_apartment();   // 与 init_apartment 配对
}
}   // namespace

// 功能: 启动朗读线程
// 参数: statusWnd 状态栏消息目标窗口(可为空)
// 返回: true=线程已创建
bool SpeechStart(HWND statusWnd) {
    if (g_started.exchange(true)) return true;
    g_statusWnd = statusWnd;
    g_thread = std::thread(SpeechThread);
    return true;
}

// 功能: 朗读一句话(打断上一条)
// 参数: text 朗读文本
// 返回: 无
void SpeechSay(const std::wstring& text) {
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_pendingText = text;
        g_cmd = Cmd::Say;
    }
    g_cv.notify_one();
}

// 功能: 立即停止朗读
// 参数: 无
// 返回: 无
void SpeechStop() {
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_cmd = Cmd::Stop;
    }
    g_cv.notify_one();
}

// 功能: 停止线程并释放引擎(退出时调用)
// 参数: 无
// 返回: 无
void SpeechShutdown() {
    if (!g_started.load()) return;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_cmd = Cmd::Quit;
    }
    g_cv.notify_one();
    if (g_thread.joinable()) g_thread.join();
}
