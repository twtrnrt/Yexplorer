// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/shell/IconDiskCache.cpp
//
// 「磁盘成品缓存」的实现：一个资产一个文件，读的时候逐字比对 key。
// 这里只做文件读写和清理，不碰界面、不碰图像列表。
// ============================================================================

#include "IconDiskCache.h"
#include "core/Logger.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

namespace shell
{

namespace
{

// 文件头固定 16 字节，格式见头文件注释
constexpr size_t kHeaderSize = 16;

// 边长上限：图标最大 256、缩略图最大 256，留出余量到 1024 防止坏文件把内存撑爆
constexpr int kMaxDimension = 1024;

// 像素数据上限（1024*1024*4 正好 4 MB，这里给 8 MB 余量）
constexpr size_t kMaxPixelBytes = 8u * 1024u * 1024u;

// 简易的文件句柄管家：出了作用域自动关，避免每条分支都手写 CloseHandle
struct ScopedHandle
{
    HANDLE value = INVALID_HANDLE_VALUE;

    ~ScopedHandle()
    {
        if (value != INVALID_HANDLE_VALUE)
        {
            ::CloseHandle(value);
        }
    }

    bool Valid() const { return value != INVALID_HANDLE_VALUE; }
};

// 老老实实读满指定字节（ReadFile 可能只读到一部分）
bool ReadAll(HANDLE file, void* buffer, size_t bytes)
{
    BYTE* cursor = static_cast<BYTE*>(buffer);
    size_t remain = bytes;

    while (remain > 0)
    {
        DWORD chunk = 0;
        if (!::ReadFile(file, cursor, static_cast<DWORD>(remain), &chunk, nullptr) || chunk == 0)
        {
            return false;
        }
        cursor += chunk;
        remain -= chunk;
    }
    return true;
}

// 老老实实写满指定字节
bool WriteAll(HANDLE file, const void* buffer, size_t bytes)
{
    const BYTE* cursor = static_cast<const BYTE*>(buffer);
    size_t remain = bytes;

    while (remain > 0)
    {
        DWORD chunk = 0;
        if (!::WriteFile(file, cursor, static_cast<DWORD>(remain), &chunk, nullptr) || chunk == 0)
        {
            return false;
        }
        cursor += chunk;
        remain -= chunk;
    }
    return true;
}

// 往字节数组里塞一个小端序的 16 位 / 32 位整数
void PutU16(std::vector<BYTE>& buffer, size_t offset, unsigned short value)
{
    buffer[offset]     = static_cast<BYTE>(value & 0xFF);
    buffer[offset + 1] = static_cast<BYTE>((value >> 8) & 0xFF);
}

void PutU32(std::vector<BYTE>& buffer, size_t offset, unsigned int value)
{
    buffer[offset]     = static_cast<BYTE>(value & 0xFF);
    buffer[offset + 1] = static_cast<BYTE>((value >> 8) & 0xFF);
    buffer[offset + 2] = static_cast<BYTE>((value >> 16) & 0xFF);
    buffer[offset + 3] = static_cast<BYTE>((value >> 24) & 0xFF);
}

unsigned short GetU16(const BYTE* data)
{
    return static_cast<unsigned short>(data[0] | (data[1] << 8));
}

unsigned int GetU32(const BYTE* data)
{
    return static_cast<unsigned int>(data[0]) |
           (static_cast<unsigned int>(data[1]) << 8) |
           (static_cast<unsigned int>(data[2]) << 16) |
           (static_cast<unsigned int>(data[3]) << 24);
}

} // namespace

// ============================================================================
// 生命周期
// ============================================================================

IconDiskCache::~IconDiskCache()
{
    if (m_lockReady)
    {
        ::DeleteCriticalSection(&m_lock);
        m_lockReady = false;
    }
}

bool IconDiskCache::Initialize(const std::wstring& subDirName, long long budgetBytes)
{
    if (subDirName.empty())
    {
        return false;
    }

    // 用环境变量拿 LOCALAPPDATA：不需要 COM，也不会被漫游配置带偏
    wchar_t localAppData[MAX_PATH] = {};
    if (::GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH) == 0)
    {
        Logger::Warn(L"图标磁盘缓存：取不到 LOCALAPPDATA，缓存层关闭");
        return false;
    }

    m_root = std::wstring(localAppData) + L"\\FileExplorer\\" + subDirName;

    // 目录不存在就建（含父级）
    std::error_code error;
    std::filesystem::create_directories(m_root, error);
    if (error)
    {
        Logger::Warn(L"图标磁盘缓存：建目录失败（" + m_root + L"），缓存层关闭");
        m_root.clear();
        return false;
    }

    m_budgetBytes = budgetBytes;

    ::InitializeCriticalSection(&m_lock);
    m_lockReady = true;

    Logger::Info(L"图标磁盘缓存已启用：" + m_root);
    return true;
}

// ============================================================================
// 路径与哈希
// ============================================================================

unsigned long long IconDiskCache::HashKey(const std::wstring& key)
{
    // FNV-1a 64：实现短、分布够用；只用来决定文件名，撞车由 key 全文比对兜底
    unsigned long long hash = 1469598103934665603ULL;
    const unsigned char* bytes = reinterpret_cast<const unsigned char*>(key.data());
    const size_t         count = key.size() * sizeof(wchar_t);

    for (size_t index = 0; index < count; ++index)
    {
        hash ^= bytes[index];
        hash *= 1099511628211ULL;
    }
    return hash;
}

unsigned int IconDiskCache::MaskStride(int width)
{
    return static_cast<unsigned int>(((width + 31) / 32) * 4);
}

std::wstring IconDiskCache::FilePath(Kind kind, const std::wstring& key) const
{
    // kind 已经由 m_root 的目录名区分（icons / thumbs），这里不用再进文件名
    (void)kind;

    const unsigned long long hash = HashKey(key);
    wchar_t hex[17] = {};
    ::swprintf_s(hex, L"%016llx", hash);

    std::wstring name(hex);
    return m_root + L"\\" + name.substr(0, 2) + L"\\" + name + L".icn";
}

// ============================================================================
// 读
// ============================================================================

bool IconDiskCache::Load(Kind kind, const std::wstring& key, Entry& entry)
{
    entry = Entry{};

    if (m_root.empty() || key.empty())
    {
        return false;
    }

    const std::wstring path = FilePath(kind, key);

    // 同一个句柄同时申请"读"和"写属性"：读完顺手就能刷最后使用时间，省一次开文件
    ScopedHandle file;
    file.value = ::CreateFileW(path.c_str(),
                               GENERIC_READ | FILE_WRITE_ATTRIBUTES,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr,
                               OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                               nullptr);

    if (!file.Valid())
    {
        CountMiss();
        return false;
    }

    BYTE header[kHeaderSize] = {};
    if (!ReadAll(file.value, header, kHeaderSize))
    {
        CountMiss();
        ::DeleteFileW(path.c_str());    // 半截文件直接扔掉
        return false;
    }

    if (std::memcmp(header, "YIC1", 4) != 0 || header[4] != static_cast<BYTE>(kind))
    {
        CountMiss();
        ::DeleteFileW(path.c_str());    // 不是一个版本 / 不是这种类，扔掉
        return false;
    }

    const unsigned int keyChars = GetU32(header + 12);
    if (keyChars == 0 || keyChars > 32768)
    {
        CountMiss();
        ::DeleteFileW(path.c_str());
        return false;
    }

    std::wstring storedKey(keyChars, L'\0');
    if (!ReadAll(file.value, &storedKey[0], static_cast<size_t>(keyChars) * sizeof(wchar_t)))
    {
        CountMiss();
        ::DeleteFileW(path.c_str());
        return false;
    }

    // key 必须逐字相同：哈希撞车、或者文件被换过，都在这里拦下
    if (storedKey != key)
    {
        CountMiss();
        ::DeleteFileW(path.c_str());
        return false;
    }

    const BYTE  flags  = header[5];
    const int   width  = static_cast<int>(GetU16(header + 6));
    const int   height = static_cast<int>(GetU16(header + 8));

    // 负缓存：确定取不到，没有像素数据
    if ((flags & 0x01) != 0)
    {
        entry.width    = 0;
        entry.height   = 0;
        entry.negative = true;
        CountHit();
        return true;
    }

    if (width <= 0 || height <= 0 || width > kMaxDimension || height > kMaxDimension)
    {
        CountMiss();
        ::DeleteFileW(path.c_str());
        return false;
    }

    const size_t pixelBytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
    if (pixelBytes > kMaxPixelBytes)
    {
        CountMiss();
        ::DeleteFileW(path.c_str());
        return false;
    }

    entry.width  = width;
    entry.height = height;
    entry.pixels.resize(pixelBytes);

    if (!ReadAll(file.value, entry.pixels.data(), pixelBytes))
    {
        entry = Entry{};
        CountMiss();
        ::DeleteFileW(path.c_str());
        return false;
    }

    if ((flags & 0x02) != 0)
    {
        const size_t maskBytes = static_cast<size_t>(MaskStride(width)) * static_cast<size_t>(height);
        entry.mask.resize(maskBytes);
        if (!ReadAll(file.value, entry.mask.data(), maskBytes))
        {
            entry = Entry{};
            CountMiss();
            ::DeleteFileW(path.c_str());
            return false;
        }
    }

    CountHit();

    // 顺手记"最后使用"（同一 key 本进程只刷一次）
    if (m_lockReady)
    {
        bool needTouch = false;
        ::EnterCriticalSection(&m_lock);
        if (m_touched.find(key) == m_touched.end())
        {
            m_touched.insert(key);
            needTouch = true;
        }
        ::LeaveCriticalSection(&m_lock);

        if (needTouch)
        {
            FILETIME now = {};
            ::GetSystemTimeAsFileTime(&now);
            ::SetFileTime(file.value, nullptr, nullptr, &now);
        }
    }

    return true;
}

// ============================================================================
// 写
// ============================================================================

bool IconDiskCache::Save(Kind kind, const std::wstring& key, const Entry& entry)
{
    if (m_root.empty() || key.empty())
    {
        return false;
    }

    // 数据自检：宁可这次不存，也不写坏文件
    if (!entry.negative)
    {
        if (entry.width <= 0 || entry.height <= 0 ||
            entry.width > kMaxDimension || entry.height > kMaxDimension)
        {
            return false;
        }

        const size_t pixelBytes = static_cast<size_t>(entry.width) * static_cast<size_t>(entry.height) * 4u;
        if (entry.pixels.size() != pixelBytes)
        {
            return false;
        }

        if (!entry.mask.empty() &&
            entry.mask.size() != static_cast<size_t>(MaskStride(entry.width)) * static_cast<size_t>(entry.height))
        {
            return false;
        }
    }

    // 头部 + key + 像素 + 掩码，一次组装好再一次写盘
    std::vector<BYTE> buffer;
    buffer.resize(kHeaderSize);

    buffer[0] = 'Y';
    buffer[1] = 'I';
    buffer[2] = 'C';
    buffer[3] = '1';
    buffer[4] = static_cast<BYTE>(kind);

    BYTE flags = 0;
    if (entry.negative)          { flags |= 0x01; }
    if (!entry.mask.empty())     { flags |= 0x02; }
    buffer[5] = flags;

    PutU16(buffer, 6,  static_cast<unsigned short>(entry.negative ? 0 : entry.width));
    PutU16(buffer, 8,  static_cast<unsigned short>(entry.negative ? 0 : entry.height));
    PutU16(buffer, 10, 0);
    PutU32(buffer, 12, static_cast<unsigned int>(key.size()));

    const BYTE* keyBytes = reinterpret_cast<const BYTE*>(key.data());
    buffer.insert(buffer.end(), keyBytes, keyBytes + key.size() * sizeof(wchar_t));

    if (!entry.negative)
    {
        buffer.insert(buffer.end(), entry.pixels.begin(), entry.pixels.end());
        buffer.insert(buffer.end(), entry.mask.begin(), entry.mask.end());
    }

    // 先保证子目录存在
    const std::wstring path = FilePath(kind, key);
    const size_t slashPos = path.find_last_of(L'\\');
    if (slashPos != std::wstring::npos)
    {
        const std::wstring dir = path.substr(0, slashPos);
        std::error_code error;
        std::filesystem::create_directories(dir, error);
        if (error)
        {
            return false;
        }
    }

    // 临时文件写到同一目录，再改名替换：要么是旧内容，要么是完整新内容，不会出现半截
    const std::wstring tempPath = path + L".tmp" + std::to_wstring(::GetCurrentProcessId());

    ScopedHandle file;
    file.value = ::CreateFileW(tempPath.c_str(),
                               GENERIC_WRITE,
                               0,
                               nullptr,
                               CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL,
                               nullptr);
    if (!file.Valid())
    {
        return false;
    }

    if (!WriteAll(file.value, buffer.data(), buffer.size()))
    {
        ::CloseHandle(file.value);
        file.value = INVALID_HANDLE_VALUE;
        ::DeleteFileW(tempPath.c_str());
        return false;
    }

    ::CloseHandle(file.value);
    file.value = INVALID_HANDLE_VALUE;

    if (!::MoveFileExW(tempPath.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
    {
        ::DeleteFileW(tempPath.c_str());
        return false;
    }

    if (m_lockReady)
    {
        ::EnterCriticalSection(&m_lock);
        m_bytesApprox += static_cast<long long>(buffer.size());
        ::LeaveCriticalSection(&m_lock);
    }
    return true;
}

// ============================================================================
// 记"使用"（供内存命中时调用；磁盘读命中已经在 Load 里顺手刷了）
// ============================================================================

void IconDiskCache::Touch(Kind kind, const std::wstring& key)
{
    if (m_root.empty() || key.empty() || !m_lockReady)
    {
        return;
    }

    // 同一个 key 本进程只刷一次：反复开同一目录时不产生大量元数据写
    ::EnterCriticalSection(&m_lock);
    const bool already = (m_touched.find(key) != m_touched.end());
    if (!already)
    {
        m_touched.insert(key);
    }
    ::LeaveCriticalSection(&m_lock);

    if (already)
    {
        return;
    }

    ScopedHandle file;
    file.value = ::CreateFileW(FilePath(kind, key).c_str(),
                               FILE_WRITE_ATTRIBUTES,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr,
                               OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL,
                               nullptr);
    if (!file.Valid())
    {
        return;
    }

    FILETIME now = {};
    ::GetSystemTimeAsFileTime(&now);
    ::SetFileTime(file.value, nullptr, nullptr, &now);
}

// ============================================================================
// 统计与清理
// ============================================================================

long long IconDiskCache::ScanBytes()
{
    if (m_root.empty())
    {
        return 0;
    }

    long long total = 0;

    std::error_code error;
    std::filesystem::recursive_directory_iterator iterator(
        m_root, std::filesystem::directory_options::skip_permission_denied, error);

    for (; !error && iterator != std::filesystem::recursive_directory_iterator(); iterator.increment(error))
    {
        if (error)
        {
            error.clear();
            continue;
        }

        std::error_code itemError;
        if (iterator->is_regular_file(itemError) &&
            iterator->path().extension() == L".icn")
        {
            total += static_cast<long long>(iterator->file_size(itemError));
        }
        itemError.clear();
    }

    if (m_lockReady)
    {
        ::EnterCriticalSection(&m_lock);
        m_bytesApprox = total;
        ::LeaveCriticalSection(&m_lock);
    }
    return total;
}

void IconDiskCache::SweepIfOverBudget()
{
    if (m_root.empty() || m_budgetBytes <= 0)
    {
        return;
    }

    long long approximate = 0;
    if (m_lockReady)
    {
        ::EnterCriticalSection(&m_lock);
        approximate = m_bytesApprox;
        ::LeaveCriticalSection(&m_lock);
    }

    // 没超就先不扫：扫一次要读几千个文件属性
    if (approximate <= m_budgetBytes)
    {
        return;
    }

    long long total = ScanBytes();
    if (total <= m_budgetBytes)
    {
        return;
    }

    // 收集所有缓存文件，按"最后使用"从旧到新排
    struct CacheFile
    {
        long long    stamp = 0;
        long long    size  = 0;
        std::wstring path;
    };

    std::vector<CacheFile> files;

    std::error_code error;
    std::filesystem::recursive_directory_iterator iterator(
        m_root, std::filesystem::directory_options::skip_permission_denied, error);

    for (; !error && iterator != std::filesystem::recursive_directory_iterator(); iterator.increment(error))
    {
        if (error)
        {
            error.clear();
            continue;
        }

        std::error_code itemError;
        if (!iterator->is_regular_file(itemError) ||
            iterator->path().extension() != L".icn")
        {
            itemError.clear();
            continue;
        }

        CacheFile file;
        file.path  = iterator->path().wstring();
        file.size  = static_cast<long long>(iterator->file_size(itemError));
        file.stamp = static_cast<long long>(
            std::filesystem::last_write_time(iterator->path(), itemError).time_since_epoch().count());
        itemError.clear();

        files.push_back(std::move(file));
    }

    std::sort(files.begin(), files.end(), [](const CacheFile& left, const CacheFile& right)
    {
        return left.stamp < right.stamp;    // 越旧越靠前
    });

    // 删到预算的 80%，留下一点余量，避免过两天又触发
    const long long target = m_budgetBytes * 8 / 10;
    long long removedBytes = 0;
    int       removedCount = 0;

    for (const auto& file : files)
    {
        if (total <= target)
        {
            break;
        }
        if (::DeleteFileW(file.path.c_str()))
        {
            total -= file.size;
            removedBytes += file.size;
            ++removedCount;
        }
    }

    if (m_lockReady)
    {
        ::EnterCriticalSection(&m_lock);
        m_bytesApprox = total;
        ::LeaveCriticalSection(&m_lock);
    }

    Logger::Info(L"图标磁盘缓存清理：删 " + std::to_wstring(removedCount) + L" 个文件，释放 " +
                 std::to_wstring(removedBytes / 1024) + L" KB，当前 " + std::to_wstring(total / 1024) + L" KB");
}

// ============================================================================
// 计数
// ============================================================================

void IconDiskCache::CountHit()
{
    if (!m_lockReady)
    {
        ++m_hits;
        return;
    }
    ::EnterCriticalSection(&m_lock);
    ++m_hits;
    ::LeaveCriticalSection(&m_lock);
}

void IconDiskCache::CountMiss()
{
    if (!m_lockReady)
    {
        ++m_misses;
        return;
    }
    ::EnterCriticalSection(&m_lock);
    ++m_misses;
    ::LeaveCriticalSection(&m_lock);
}

} // namespace shell
