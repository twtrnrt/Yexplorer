// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/core/AsyncTask.h
//
// 异步任务基础设施。
//
// 为什么需要它：
//   枚举目录、读取图标这些都是慢操作（网络盘尤其慢）。如果放在 UI 线程做，
//   窗口会转圈"未响应"。所以统一约定：
//     - 所有慢操作丢到后台线程
//     - UI 线程只收结果、只重绘
//
// 取消令牌的作用：
//   用户在列表里快速切换目录时，上一个目录的枚举还没跑完。必须能立刻取消，
//   否则会堆积一堆后台任务，卡住磁盘。
// ============================================================================

#pragma once

#include <atomic>

namespace core
{

// ----------------------------------------------------------------------------
// 取消令牌
//
// 用法：
//   后台任务持有 const CancellationToken*，在耗时循环里定期检查 IsCancelled()；
//   UI 线程调用 Cancel() 即可让任务尽快退出。
//
// 线程安全：内部是原子布尔，多线程读写安全。
// ----------------------------------------------------------------------------
class CancellationToken
{
public:
    // 请求取消（可从任意线程调用，可重复调用）
    void Cancel() { m_flag.store(true, std::memory_order_release); }

    // 查询是否已取消（后台任务循环里检查）
    bool IsCancelled() const { return m_flag.load(std::memory_order_acquire); }

    // 复位，便于复用同一令牌对象跑下一个任务
    void Reset() { m_flag.store(false, std::memory_order_release); }

private:
    std::atomic<bool> m_flag{ false };
};

} // namespace core
