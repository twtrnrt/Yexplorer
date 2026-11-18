// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/core/Logger.cpp
//
// 日志实现。
//
// 编码说明：
//   spdlog 按 UTF-8 写文件；我们内部收到的是 UTF-16（Windows 原生宽字符），
//   写入前统一转 UTF-8，避免中文日志乱码。
// ============================================================================

#include "Logger.h"

// spdlog 头文件（header-only 模式，SPDLOG_HEADER_ONLY 已在工程预处理器里定义）
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>

#include <windows.h>      // WideCharToMultiByte / FormatMessageW
#include <memory>
#include <mutex>

namespace
{
// 全局日志器实例（可能为空，表示日志未初始化）
std::shared_ptr<spdlog::logger> g_logger;
std::mutex                      g_mutex;

// 日志文件的绝对路径（Init 时记下来，供目录监视跳过"日志自己增长"的变化）
std::wstring                    g_logPath;

// ----------------------------------------------------------------------------
// UTF-16 → UTF-8
// ----------------------------------------------------------------------------
std::string ToUtf8(const std::wstring& wide)
{
    if (wide.empty())
    {
        return {};
    }

    // 第一次调用只取需要的字节数
    int byteCount = ::WideCharToMultiByte(CP_UTF8, 0,
                                          wide.c_str(), static_cast<int>(wide.size()),
                                          nullptr, 0, nullptr, nullptr);
    if (byteCount <= 0)
    {
        return {};
    }

    std::string utf8(static_cast<size_t>(byteCount), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0,
                          wide.c_str(), static_cast<int>(wide.size()),
                          utf8.data(), byteCount, nullptr, nullptr);
    return utf8;
}

// ----------------------------------------------------------------------------
// 取日志器（调用方需已持有 g_mutex 或确认已初始化）
// ----------------------------------------------------------------------------
std::shared_ptr<spdlog::logger> GetLogger()
{
    return g_logger;
}

} // namespace

namespace Logger
{

void Init(const std::wstring& logFilePath)
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_logPath = logFilePath;      // 目录监视要按这个路径跳过日志自身的变化
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    try
    {
        // basic_file_sink_mt：多线程安全的文件输出
        g_logger = spdlog::basic_logger_mt("file_explorer", ToUtf8(logFilePath), /*truncate=*/true);
        // 只记信息级及以上；格式：时间 [级别] 内容
        g_logger->set_level(spdlog::level::info);
        g_logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
        g_logger->flush_on(spdlog::level::info);
        g_logger->info("========== 文件管理器启动 ==========");
    }
    catch (...)
    {
        // 日志初始化失败不应影响程序运行，静默降级
        g_logger.reset();
    }
}

void Shutdown()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_logger)
    {
        g_logger->info("========== 文件管理器退出 ==========");
        g_logger->flush();
        g_logger.reset();
    }
}

std::wstring FilePath()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_logPath;       // 返回值拷贝：调用方（别的线程）自己拿着用，不怕被改
}

void Info(const std::wstring& message)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_logger)
    {
        return;
    }
    g_logger->info(ToUtf8(message));
}

void Warn(const std::wstring& message)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_logger)
    {
        return;
    }
    g_logger->warn(ToUtf8(message));
}

void Error(const std::wstring& message)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_logger)
    {
        return;
    }
    g_logger->error(ToUtf8(message));
}

std::wstring FormatHresult(long hr)
{
    // 先拼十六进制码
    wchar_t codeText[32] = {};
    ::swprintf_s(codeText, L"0x%08lX", static_cast<unsigned long>(hr));

    // 再取系统消息文本
    wchar_t* sysText = nullptr;
    ::FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                     nullptr, static_cast<DWORD>(hr),
                     MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                     reinterpret_cast<LPWSTR>(&sysText), 0, nullptr);

    std::wstring result = codeText;
    if (sysText != nullptr)
    {
        std::wstring msg(sysText);
        ::LocalFree(sysText);

        // 去掉系统消息末尾的换行
        while (!msg.empty() && (msg.back() == L'\r' || msg.back() == L'\n' || msg.back() == L' '))
        {
            msg.pop_back();
        }
        if (!msg.empty())
        {
            result += L" (" + msg + L")";
        }
    }
    return result;
}

} // namespace Logger
