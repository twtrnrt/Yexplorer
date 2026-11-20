using System;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading.Tasks;

using Listary.FileAppPlugin;

namespace Listary.FileAppPlugin.Yexplorer
{
    /// <summary>
    /// Listary 的第三方文件管理器插件（放在 Listary\FileAppPlugins\Yexplorer 下）。
    ///
    /// Listary 的「快速切换」和「打开文件夹」对 Explorer 之外的管理器是插件式的：
    /// 它加载本 DLL，对每个顶层窗口调用 BindFileWindow；插件认出自己的窗口后，
    /// 通过 GetCurrentFolder 报告当前目录。Listary 拿到目录后负责跳转保存/打开对话框。
    /// </summary>
    public sealed class YexplorerPlugin : IFileAppPlugin
    {
        private IFileAppPluginHost _host;

        public bool IsOpenedFolderProvider { get { return true; } }
        public bool IsQuickSwitchTarget { get { return true; } }
        public bool IsSharedAcrossApplications { get { return false; } }
        public SearchBarType SearchBarType { get { return global::Listary.FileAppPlugin.SearchBarType.Floating; } }

        public Task<bool> Initialize(IFileAppPluginHost host)
        {
            _host = host;
            DebugLog("Initialize");
            return Task.FromResult(true);
        }

        public IFileWindow BindFileWindow(IntPtr hWnd)
        {
            bool isOurs = YexplorerWindow.IsYexplorerWindow(hWnd);
            DebugLog("BindFileWindow hwnd=" + hWnd + " ours=" + isOurs);
            return isOurs ? new YexplorerWindow(hWnd) : null;
        }

        private static void DebugLog(string message)
        {
            if (Environment.GetEnvironmentVariable("YEXP_LISTARY_DEBUG") != "1")
            {
                return;
            }
            try
            {
                File.AppendAllText(Path.Combine(Path.GetTempPath(), "yexp-listary-plugin.log"),
                                   DateTime.Now.ToString("HH:mm:ss.fff") + "  " + message + Environment.NewLine);
            }
            catch
            {
            }
        }
    }

    public sealed class YexplorerWindow : IFileWindow
    {
        private readonly IntPtr _hwnd;

        public YexplorerWindow(IntPtr hwnd)
        {
            _hwnd = hwnd;
        }

        public IntPtr Handle { get { return _hwnd; } }

        public Task<IFileTab> GetCurrentTab()
        {
            return Task.FromResult<IFileTab>(new YexplorerTab(_hwnd));
        }

        public static bool IsYexplorerWindow(IntPtr hwnd)
        {
            if (hwnd == IntPtr.Zero)
            {
                return false;
            }

            var className = new StringBuilder(256);
            if (Native.GetClassName(hwnd, className, className.Capacity) == 0 ||
                className.ToString() != "FileExplorerMainWindow")
            {
                return false;
            }

            string exePath = Native.GetProcessPath(hwnd);
            return exePath != null &&
                   string.Equals(Path.GetFileName(exePath), "FileExplorer.exe", StringComparison.OrdinalIgnoreCase);
        }
    }

    public sealed class YexplorerTab : IFileTab, IGetFolder, IOpenFolder, IOpenFolderAndSelectFile, IOpenFile
    {
        private static readonly Regex TitlePattern =
            new Regex(@"^文件管理器 - (?<folder>.+)（\d+ 项）$", RegexOptions.Compiled);

        private readonly IntPtr _hwnd;

        public YexplorerTab(IntPtr hwnd)
        {
            _hwnd = hwnd;
        }

        public Task<string> GetCurrentFolder()
        {
            string folder = ReadFolderFromTitle();
            DebugLog("GetCurrentFolder -> " + (folder ?? "<null>"));
            return Task.FromResult(folder);
        }

        public Task<bool> OpenFolder(string path)
        {
            return Task.FromResult(StartOwnExplorer(path));
        }

        public Task<bool> OpenFolderAndSelectFile(string path)
        {
            return Task.FromResult(StartOwnExplorer(path));
        }

        public Task<bool> OpenFile(string path)
        {
            return Task.FromResult(false);
        }

        private string ReadFolderFromTitle()
        {
            var buffer = new StringBuilder(1024);
            Native.GetWindowText(_hwnd, buffer, buffer.Capacity);

            Match match = TitlePattern.Match(buffer.ToString());
            if (!match.Success)
            {
                return null;
            }

            string folder = match.Groups["folder"].Value.Trim();
            if (Directory.Exists(folder))
            {
                return folder;
            }

            // 「桌面」「文档」这类位置标题里是显示名，换回真实路径
            string profile = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
            switch (folder)
            {
                case "桌面":
                    return Environment.GetFolderPath(Environment.SpecialFolder.Desktop);
                case "文档":
                    return Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments);
                case "图片":
                    return Environment.GetFolderPath(Environment.SpecialFolder.MyPictures);
                case "音乐":
                    return Environment.GetFolderPath(Environment.SpecialFolder.MyMusic);
                case "视频":
                    return Environment.GetFolderPath(Environment.SpecialFolder.MyVideos);
                case "下载":
                    return Path.Combine(profile, "Downloads");
                default:
                    return null;
            }
        }

        private bool StartOwnExplorer(string argument)
        {
            try
            {
                string exePath = Native.GetProcessPath(_hwnd);
                if (string.IsNullOrEmpty(exePath) || !File.Exists(exePath))
                {
                    return false;
                }

                Process.Start(new ProcessStartInfo(exePath, "\"" + argument + "\"")
                {
                    UseShellExecute = false,
                });
                return true;
            }
            catch
            {
                return false;
            }
        }

        private static void DebugLog(string message)
        {
            if (Environment.GetEnvironmentVariable("YEXP_LISTARY_DEBUG") != "1")
            {
                return;
            }
            try
            {
                File.AppendAllText(Path.Combine(Path.GetTempPath(), "yexp-listary-plugin.log"),
                                   DateTime.Now.ToString("HH:mm:ss.fff") + "  " + message + Environment.NewLine);
            }
            catch
            {
            }
        }
    }

    internal static class Native
    {
        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        internal static extern int GetClassName(IntPtr hWnd, StringBuilder lpClassName, int nMaxCount);

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        internal static extern int GetWindowText(IntPtr hWnd, StringBuilder lpString, int nMaxCount);

        [DllImport("user32.dll")]
        internal static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint processId);

        [DllImport("kernel32.dll", SetLastError = true)]
        internal static extern IntPtr OpenProcess(uint access, bool inheritHandle, uint processId);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        internal static extern bool QueryFullProcessImageName(IntPtr process, uint flags, StringBuilder exeName, ref uint size);

        [DllImport("kernel32.dll")]
        internal static extern bool CloseHandle(IntPtr handle);

        internal static string GetProcessPath(IntPtr hwnd)
        {
            uint pid;
            GetWindowThreadProcessId(hwnd, out pid);
            if (pid == 0)
            {
                return null;
            }

            IntPtr process = OpenProcess(0x1000 /* PROCESS_QUERY_LIMITED_INFORMATION */, false, pid);
            if (process == IntPtr.Zero)
            {
                return null;
            }

            try
            {
                var buffer = new StringBuilder(1024);
                uint size = (uint)buffer.Capacity;
                return QueryFullProcessImageName(process, 0, buffer, ref size) ? buffer.ToString() : null;
            }
            finally
            {
                CloseHandle(process);
            }
        }
    }
}
