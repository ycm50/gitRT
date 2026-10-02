#pragma once
// ---------------------------------------------------------------------------
// PostMessageW 所有权交接（src/gui 专用）
//
// 背景：GUI 里大量「工作线程算好一个对象 → PostMessageW 交给 UI 线程 delete」的写法。
//   Win32 消息是**异步**的：目标窗口可能已经销毁，此时 PostMessageW 返回 FALSE，
//   而对象永远不会有人接管 —— 这就是泄漏。项目里曾有 25 处漏写回收，
//   是 GCC -fanalyzer（-Wanalyzer-malloc-leak）把它们找出来的。
//
// 为什么用 unique_ptr + release() 而不是裸指针：
//   · 裸指针写法在任何一条早退/异常路径上漏掉 delete 都是泄漏，且编译器不吭声；
//   · unique_ptr 在所有路径（含早退）保证析构；只有**确认消息投递成功**后才 release()，
//     「所有权已移交」是显式写出来的，忘写 delete 在编译期不可达。
//   · 实测（GCC 16.2 + -fanalyzer）：裸指针写法稳定报 -Wanalyzer-malloc-leak，
//     改成本文件这种写法后**告警消失**（用把它包成模板函数的方式反而消不掉 ——
//     分析器会内联进去，仍然假设 PostMessageW 会抛异常）。
//
// ★ 接收端规约（必须遵守，否则仍有泄漏窗口）：
//   处理该消息的分支一律用 unique_ptr 接管，**包括窗口正在销毁时要丢弃的分支**：
//       auto p = std::unique_ptr<T>(reinterpret_cast<T*>(lp));
//   PostMessage 是异步的：消息投递成功、但窗口在处理前被销毁时，消息会被丢弃，
//   此时没有任何人接管 —— 这是 Win32 消息机制的固有性质，本 helper 不解决它，
//   只能靠「UI 线程在销毁前先处理完队列」这一惯例（见各窗口的 WM_NCDESTROY 处理）。
// ---------------------------------------------------------------------------

#include <windows.h>

#include <memory>
#include <utility>

namespace grt::gui {

// 把 p 的所有权通过窗口消息交给 hwnd；投递失败（窗口已销毁）则在此回收。
// 用法： PostOwned(hwnd, WM_GRT_TASK_LOG, std::make_unique<std::string>(out));
template <class T>
inline void PostOwned(HWND hwnd, UINT msg, std::unique_ptr<T> p, WPARAM wp = 0) {
    if (::PostMessageW(hwnd, msg, wp, reinterpret_cast<LPARAM>(p.get())))
        (void)p.release();   // 投递成功 → 所有权已移交接收端
    // 失败 → p 在此析构，自动回收
}

// 接收端统一接管口（配合 PostOwned 使用）。
// 用法： auto s = grt::gui::TakeOwned<std::string>(lp);
template <class T>
inline std::unique_ptr<T> TakeOwned(LPARAM lp) {
    return std::unique_ptr<T>(reinterpret_cast<T*>(lp));
}

}  // namespace grt::gui
