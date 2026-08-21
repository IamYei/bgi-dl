# bgi-dl

Mnet Plus Downloader 是一个 Qt 6 桌面下载器。输入 Mnet Plus 视频页后，程序会读取站内标题、公开的 HLS 视频流、独立音频流与字幕配置，再通过 FFmpeg 重封装为同名 MKV。

界面右上角可即时切换简体中文、English、日本語和한국어。首次启动会跟随系统界面语言（支持范围内），之后会记住选择。

字幕支持日本语、英语、韩语、简体中文（`zh_CN`）和繁体中文（`zh_TW`）多选；每种语言既可内封为独立 MKV 字幕轨，也可同时导出为 `视频标题.语言.srt`。

媒体和字幕会并行下载，字幕窗口使用有界并发请求。下载器会解析 HLS master，自动从 2160p 开始向下探测最高可用分辨率（2160 → 1440 → 1080 → 720 → …），即使 master 只列出低分辨率，也会从 variant 地址继续查找更高档清单。视频分片使用 6 路并发下载，独立音轨（`_audio.m3u8` 或 `_audio.cmfa`）同时下载，随后只进行本地流复制。输出目录会在本机设置中自动记忆。

字幕支持一键全选，也支持内封 MKV 和单独导出 SRT。批量下载允许一次粘贴多个地址，逐个解析后顺序下载。

文件名可自定义模板，默认 `{date}.Mnet Plus.{title}.WEB-DL.{res}.{codec}.-{tag}.{ext}`。可用占位符：`{date}`（YYMMDD 发布日期）、`{title}`、`{res}`（如 `1080p`）、`{resolution}`（如 `1080`）、`{codec}`（H264/HEVC 自动识别）、`{tag}`（发布组，默认 `buguibgib`）、`{ext}`。

## 环境

- Qt 6.5 或更新版本（Widgets、Network、Concurrent、Sql）
- CMake 3.21 或更新版本
- `ffmpeg` 和 `ffprobe`

Windows：

- Visual Studio 2022（Desktop development with C++）或其他支持 C++17 的 Qt 6.5+ 工具链
- Qt 6.5+（Widgets、Network、Concurrent、Sql）
- CMake 3.21+ 与 `ffmpeg`；把 `ffmpeg.exe` / `ffprobe.exe` 放入 `PATH`，或与下载器 `.exe` 放在同一目录

macOS/Homebrew：

```bash
brew install qt cmake ffmpeg
```

## 构建

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
open build/MnetPlusDownloader.app
```

也可以直接运行 `build/MnetPlusDownloader.app/Contents/MacOS/MnetPlusDownloader`。

源码仓库保持精简。维护者使用的 QtTest 测试源码仅保存在本地，检测到这些文件时 CMake 会自动启用测试；全新检出只构建应用程序本身。

Windows（PowerShell，Qt 安装目录按实际版本调整）：

```powershell
cmake -S . -B build -DCMAKE_PREFIX_PATH="C:\Qt\6.8.0\msvc2022_64"
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

使用 Visual Studio 生成器时，程序通常位于 `build\Release\MnetPlusDownloader.exe`。发布前请使用同一 Qt 安装目录中的 `windeployqt` 收集 Qt DLL 和平台插件：

```powershell
& "C:\Qt\6.8.0\msvc2022_64\bin\windeployqt.exe" "build\Release\MnetPlusDownloader.exe"
```

## 浏览器会话与隐私

程序自身读取 Chrome 或 Edge 浏览器会话，只保留域名为 `mnetplus.world` 或其子域的 Cookie。Cookie 数据库只复制到系统临时目录进行只读查询：macOS 使用 Keychain 与 AES，Windows 使用 DPAPI 与 Windows CNG AES-GCM 解密 Chromium Cookie。Cookie 值不会写入任务日志，也不会发送给 Mnet Plus 以外的站点。

Cookie 存储路径、PBKDF2/AES-CBC 参数和 Chrome 24+ 域名摘要均与 `yt-dlp` 的 Chromium Cookie 实现保持兼容，但程序不调用或依赖 `yt-dlp`。

macOS 首次读取浏览器时，系统可能询问钥匙串权限；拒绝授权时程序会回退到游客模式。

完整 CDN 流依赖 CloudFront 签名 Cookie。程序使用 FFmpeg 的 domain/path 受限 Cookie 选项，不使用会随重定向跨域发送的裸 `Cookie` 请求头。受 FFmpeg CLI 接口限制，下载期间 Cookie 值会短暂存在于 FFmpeg 进程参数中，因此同一系统用户的进程检查工具可能看到它；退出 FFmpeg 后即消失。

开始下载时，程序会使用 `_mnet_atk` 生成 Bearer 授权，调用 Mnet Plus 官方媒体 Cookie 接口刷新 CloudFront 签名，再预检视频，并同时启动媒体下载与所选字幕抓取。

## 访问限制

程序只处理当前浏览器会话可正常访问的媒体，不绕过购买、地区、登录、HDCP 或 DRM 限制。若字幕 API 返回 `401/403`，程序会在日志中说明，并继续生成不含字幕的 MKV。

真实登录会话诊断默认跳过。如需仅验证 Edge 登录、媒体签名、HLS、音频和 `zh_CN` 字幕访问（不下载完整视频）：

```bash
MNETPLUS_LIVE_TEST=1 build/MnetPlusDownloaderLiveSessionTests -v1
```

## 文档语言

- [English（默认）](README.md)
- 简体中文：当前文件

## 下载

发布包位于 [bgi-dl GitHub Releases 页面](https://github.com/IamYei/bgi-dl/releases/latest)：

- `bgi-dl-Windows-x64.zip`
- `bgi-dl-macOS-arm64.zip`
- `bgi-dl-macOS-arm64.dmg`

## 许可证

`bgi-dl` 使用 [MIT License](LICENSE) 发布。你可以复制、修改、二次发布本项目，
也可以用于商业用途，但每份副本或软件的重要部分都必须保留版权声明和完整许可证文本，
并标明作者 `IamYei (buguibgi)`。

版权归 `IamYei (buguibgi)` 所有，年份为 2026。
