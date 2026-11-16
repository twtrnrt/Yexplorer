# ============================================================================
# 生成「汉字 → 拼音首字母」查找表（C++ 源码）
#
# 用途：
#   文件列表里按字母跳转时，输入 wd 也能命中「文档」这类中文名。
#   匹配时把中文名转换成「首字母串」（文档 → wd），再按前缀比较。
#
# 为什么用这张表，而不是调用系统接口：
#   系统没有现成的「汉字转拼音首字母」API；在线调用或 IME 接口都慢且不可靠。
#   这里把数据离线生成成静态表，编进 exe，运行时只做二分查找，零依赖、零延迟。
#
# 数据来源：https://github.com/mozillazg/pinyin-data 的 pinyin.txt
#   格式：U+XXXX: 拼音1,拼音2  # 汉字   （多音字用逗号分隔全部读音）
#   该文件按 Unicode 收录，约 1 MB。它只用来生成表，不随仓库分发。
#
# 多音字：
#   一个汉字的所有读音首字母都会记进掩码（「重」→ z、c、t 三个位都置 1），
#   所以输入 c 或 z 都能命中同一个字——这是需求明确要求的。
#
# 收录范围：
#   只收 GB2312 能编码的汉字（6763 个常用字，覆盖日常文件名 99% 以上）。
#   生僻字（扩展区）不收，表能小一半以上，实际用不上。
#
# 用法：
#   python tools/generate-pinyin-table.py <pinyin.txt 路径> src/core/PinyinInitials.cpp
#
#   pinyin.txt 获取方式（任选其一）：
#     git clone --depth 1 https://github.com/mozillazg/pinyin-data
#   生成出来的源文件是纯数据，直接提交进仓库，编译时不需要再跑这个脚本。
# ============================================================================

import re
import sys

# 汉字 → 首字母集合（bit 0 = 'a' ... bit 25 = 'z'）
entries = {}

with open(sys.argv[1], encoding="utf-8") as source:
    for line in source:
        line = line.strip()
        if not line or line.startswith("#"):
            continue

        match = re.match(r"U\+([0-9A-Fa-f]+):\s*(.*?)\s*#", line)
        if not match:
            continue

        code_point = int(match.group(1), 16)
        readings   = match.group(2)

        # 只收 GB2312 能编码的字（常用字），扩展区生僻字跳过
        try:
            chr(code_point).encode("gb2312")
        except UnicodeEncodeError:
            continue

        letters = entries.setdefault(code_point, set())

        for reading in readings.split(","):
            # 取这个读音的第一个 ASCII 字母（拼音都是字母开头，个别生僻音节跳过）
            for ch in reading.strip():
                low = ch.lower()
                if "a" <= low <= "z":
                    letters.add(low)
                    break

lines = []
lines.append("// ============================================================================")
lines.append("// 文件管理器（自研 Explorer 替代品）")
lines.append("// 文件：src/core/PinyinInitials.cpp")
lines.append("//")
lines.append("// 【这个文件是自动生成的，不要手工改】")
lines.append("//   生成脚本：tools/generate-pinyin-table.py")
lines.append("//   数据来源：https://github.com/mozillazg/pinyin-data 的 pinyin.txt")
lines.append("//")
lines.append("// 【它是干什么的】")
lines.append("//   文件列表里「按字母跳转」要支持中文名：输入 wd 命中「文档」。")
lines.append("//   所以每个汉字要知道它拼音的首字母；多音字把全部读音的首字母都记上，")
lines.append("//   比如「重」= zhòng/chóng/tóng，z、c、t 三位都置 1，输入哪个都能命中。")
lines.append("//")
lines.append("// 【收录范围】")
lines.append("//   只收 GB2312 能编码的常用汉字（6763 个），生僻字不收。")
lines.append("//   表是按码点升序排好的，查询用二分，一次几次比较，可以忽略不计。")
lines.append("// ============================================================================")
lines.append("")
lines.append('#include "core/PinyinInitials.h"')
lines.append("")
lines.append("namespace core")
lines.append("{")
lines.append("namespace")
lines.append("{")
lines.append("")
lines.append("// 一条：汉字码点 + 首字母掩码（bit0 = a … bit25 = z；多音字会有多个位）")
lines.append("struct PinyinEntry")
lines.append("{")
lines.append("    unsigned short codePoint;   // Unicode 码点（GB2312 里的汉字都在 0x4E00~0x9FFF）")
lines.append("    unsigned int   letters;     // 首字母掩码")
lines.append("};")
lines.append("")
lines.append("// %d 条，按码点升序" % len(entries))
lines.append("const PinyinEntry kPinyinTable[] =")
lines.append("{")

for code_point in sorted(entries):
    mask = 0
    for letter in entries[code_point]:
        mask |= 1 << (ord(letter) - ord("a"))
    lines.append("    {0x%04X, 0x%08X}," % (code_point, mask))

lines.append("};")
lines.append("")
lines.append("}  // 匿名命名空间")
lines.append("")
lines.append("unsigned int PinyinInitialMask(wchar_t ch)")
lines.append("{")
lines.append("    // 码点落在表外（非汉字 / 生僻字）直接返回 0，调用方按普通字符处理")
lines.append("    if (ch < 0x4E00 || ch > 0x9FFF)")
lines.append("    {")
lines.append("        return 0;")
lines.append("    }")
lines.append("")
lines.append("    // 二分查找：表不大，但按字母跳转可能连按很多次，二分比线性稳")
lines.append("    int low  = 0;")
lines.append("    int high = static_cast<int>(sizeof(kPinyinTable) / sizeof(kPinyinTable[0])) - 1;")
lines.append("")
lines.append("    while (low <= high)")
lines.append("    {")
lines.append("        const int middle = (low + high) / 2;")
lines.append("        if (kPinyinTable[middle].codePoint == static_cast<unsigned short>(ch))")
lines.append("        {")
lines.append("            return kPinyinTable[middle].letters;")
lines.append("        }")
lines.append("        if (kPinyinTable[middle].codePoint < static_cast<unsigned short>(ch))")
lines.append("        {")
lines.append("            low = middle + 1;")
lines.append("        }")
lines.append("        else")
lines.append("        {")
lines.append("            high = middle - 1;")
lines.append("        }")
lines.append("    }")
lines.append("")
lines.append("    return 0;")
lines.append("}")
lines.append("")
lines.append("} // namespace core")
lines.append("")

with open(sys.argv[2], "w", encoding="utf-8", newline="\r\n") as target:
    target.write("\n".join(lines))

print("生成完成：%s，共 %d 条" % (sys.argv[2], len(entries)))
