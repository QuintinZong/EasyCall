// ============================================================
// EasyCall 教室端 - 语音朗读模块(仅 MSVC 构建使用)
// 技术: WinRT Windows.Media.SpeechSynthesis(自然语音) + MediaPlayer 无窗口播放
// 线程: 独立后台线程(MTA 公寓), 命令队列串行处理, 新叫号打断上一条
// 注意: 本文件与 wintoastlib 分开编译, 避免 WRL 与 C++/WinRT 头冲突
// ============================================================
#pragma once
#include <windows.h>
#include <string>

// 启动朗读线程(内部 RoInitialize MTA + 创建引擎);
// 失败时向 statusWnd 投递 WM_APP_STATUS(WM_APP+3) 提示一次
// 返回: true=线程已启动
bool SpeechStart(HWND statusWnd);

// 朗读一句话(正在播放的立即被打断)
void SpeechSay(const std::wstring& text);

// 立即停止朗读
void SpeechStop();

// 停止线程并释放引擎(程序退出时调用)
void SpeechShutdown();
