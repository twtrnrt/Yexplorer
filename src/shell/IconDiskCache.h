// ============================================================================
// 文件管理器（自研 Explorer 替代品）
// 文件：src/shell/IconDiskCache.h
//
// 图标 / 缩略图的「磁盘成品缓存」。
//
// 【为什么需要这一层】
//   取 exe 的图标必须把整个文件读进去，撞上杀毒软件重扫时，一个 276 MB 的
//   安装包要 3.8～6.1 秒。资源管理器不卡，是因为它把图标存成小图片放在硬盘上，
//   以后压根不碰源文件。这里补的就是这一层。
//
// 【存储形态】
//   一个资产一个文件：
//     %LOCALAPPDATA%\FileExplorer\icons\<哈希前两位>\<哈希>.icn
//   文件名是 key 的 64 位哈希（只用来定位）；key 全文存在文件里，
//   读的时候逐字比对，防止哈希撞车。
//
//   文件内容：
//     [0..3]   "YIC1"        魔数（换格式就改成 YIC2，老文件自动作废）
//     [4]      种类           0=图标 1=缩略图
//     [5]      标志位         bit0=负缓存  bit1=带掩码
//     [6..7]   宽（uint16）
//     [8..9]   高（uint16）
//     [10..11] 保留
//     [12..15] key 长度（uint32，单位是字符）
//     [16..]   key 全文（UTF-16）
//     之后     32 位 BGRA 像素（宽*高*4）
//     之后     1 位掩码（每行按 4 字节对齐；没有掩码时不存在）
//
// 【这个类只做文件读写】
//   不碰界面、不碰图像列表，方便单独验证；接进图标管线是下一步的事。
// ============================================================================

#pragma once

#include <windows.h>

#include <atomic>
#include <string>
#include <vector>
#include <unordered_set>

namespace shell
{

class IconDiskCache
{
public:
    // 缓存种类：图标和缩略图分目录存放，预算也分开算
    enum class Kind : unsigned char
    {
        Icon = 0,        // 图标（16/32/48…）
        Thumbnail = 1    // 缩略图（96/128/256…，二期用）
    };

    // 一条缓存内容
    struct Entry
    {
        int  width    = 0;              // 宽（像素）
        int  height   = 0;              // 高（像素）
        bool negative = false;          // 负缓存：确定取不到（例如没有图标资源的 exe）

        std::vector<BYTE> pixels;       // 32 位 BGRA：宽*高*4 字节
        std::vector<BYTE> mask;         // 1 位掩码：每行按 4 字节对齐；没有就空着
    };

    IconDiskCache() = default;
    ~IconDiskCache();

    IconDiskCache(const IconDiskCache&) = delete;
    IconDiskCache& operator=(const IconDiskCache&) = delete;

    // ------------------------------------------------------------------
    // 初始化
    //   subDirName —— %LOCALAPPDATA%\FileExplorer 下的子目录名（"icons" / "thumbs"）
    //   budgetBytes —— 该种类的体积上限（图标 32 MB，缩略图 200 MB）
    // 返回 false 表示目录建不出来，此时缓存层整体降级为"不缓存"，不影响功能
    // ------------------------------------------------------------------
    bool Initialize(const std::wstring& subDirName, long long budgetBytes);

    bool IsInitialized() const { return !m_root.empty(); }

    // ------------------------------------------------------------------
    // 读一条
    // 命中返回 true 并填好 entry；文件不存在、key 对不上、文件坏了都返回 false
    // （读失败时会顺手把坏文件删掉，下次重新生成）
    // ------------------------------------------------------------------
    bool Load(Kind kind, const std::wstring& key, Entry& entry);

    // ------------------------------------------------------------------
    // 写一条
    // 先写临时文件再改名替换（原子），不需要跨进程锁；
    // 写失败只意味着"下次还得重新取"，不影响使用
    // ------------------------------------------------------------------
    bool Save(Kind kind, const std::wstring& key, const Entry& entry);

    // ------------------------------------------------------------------
    // 记一次"使用"
    //
    // 做法：把缓存文件自身的修改时间刷成当前时间（不重写内容），
    // 清理时按这个时间从旧到新删。
    // 同一个 key 在一次运行里只刷一次，避免反复开同一目录时大量元数据写。
    // ------------------------------------------------------------------
    void Touch(Kind kind, const std::wstring& key);

    // ------------------------------------------------------------------
    // 全局停写开关（只在「恢复/重置 → 清除所有痕迹」时用一次）
    //
    // 删目录这一刻，后台取图标的线程可能手里正好有一条要存盘的任务；
    // 如果不管，目录刚删掉几毫秒就又被写回来，用户看起来就是"没清干净"。
    // 所以删之前先把这个开关按下：之后任何线程再想写/刷时间都直接放弃。
    // ------------------------------------------------------------------
    static void DisableAllWrites();

    // ------------------------------------------------------------------
    // 统计当前占用（后台线程调用；8000 个文件约 0.5～1 秒）
    // ------------------------------------------------------------------
    long long ScanBytes();

    // ------------------------------------------------------------------
    // 超预算就清扫：按"最后使用"从旧到新删，删到预算的 80%
    // 只在后台线程调用；不挡打开目录的路径
    // ------------------------------------------------------------------
    void SweepIfOverBudget();

    // 诊断用：本进程累计命中 / 未命中次数
    long long HitCount() const { return m_hits; }
    long long MissCount() const { return m_misses; }
    long long CurrentBytes() const { return m_bytesApprox; }

private:
    // 算出某个 key 对应的缓存文件完整路径
    std::wstring FilePath(Kind kind, const std::wstring& key) const;

    // FNV-1a 64 位哈希（不引第三方库）
    static unsigned long long HashKey(const std::wstring& key);

    // 1 位掩码每行的字节数（按 4 字节对齐）
    static unsigned int MaskStride(int width);

    // 计数（加锁，因为读可能在界面线程、写可能在后台线程）
    void CountHit();
    void CountMiss();

private:
    std::wstring m_root;            // 已经带好子目录的完整路径
    long long    m_budgetBytes = 0;
    long long    m_bytesApprox = 0;

    long long    m_hits   = 0;
    long long    m_misses = 0;

    // 「清除所有痕迹」后置位的全局停写开关：所有实例、所有线程都认它
    static std::atomic<bool> s_writesDisabled;

    // 本进程已经刷过"最后使用时间"的 key，避免重复刷
    std::unordered_set<std::wstring> m_touched;
    CRITICAL_SECTION m_lock = {};
    bool             m_lockReady = false;
};

} // namespace shell
