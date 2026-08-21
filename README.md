# bgi-dl
## Language Documentation

- English (default): this file
- [Simplified Chinese](README.zh-CN.md)

⚠️ IMPORTANT! DO NOT PROMOTE OR SHARE THIS REPOSITORY ON ANY FORUMS, YOUTUBE, OR OTHER PUBLIC PLATFORMS!
⚠️ 주의! 이 저장소를 어떤 포럼, 유튜브 또는 기타 공개적인 장소에서도 홍보하거나 공유하지 마세요!
⚠️ 注意！不要在任何论坛，哔哩哔哩等公开场合宣传，分享此仓库！

Mnet Plus Downloader is a Qt 6 desktop downloader for Mnet Plus. Paste an Mnet Plus video page URL and the application reads the page title, publicly available HLS video and audio streams, and subtitle configuration, then remuxes the result into an MKV file with FFmpeg.

The interface supports four languages: English, Simplified Chinese, Japanese, and Korean. The first launch follows the system language when it is supported; later launches remember the language selected in the application.

## Features

- Select multiple subtitle languages: Japanese, English, Korean, Simplified Chinese (`zh_CN`), and Traditional Chinese (`zh_TW`). Subtitles can be embedded as separate MKV tracks and exported as `video-title.language.srt` files.
- Download media and subtitles concurrently with bounded subtitle request concurrency.
- Parse HLS master playlists and probe for the highest available resolution from 2160p down through 1440p, 1080p, 720p, and lower variants. If the master lists only low resolutions, the downloader also follows variant URLs to find higher-quality playlists.
- Download video segments with six concurrent workers. Download a separate audio playlist (`_audio.m3u8` or `_audio.cmfa`) at the same time, then use local stream copy for remuxing.
- Remember the output directory in local application settings.
- Select all subtitles with one action, embed them in MKV, export SRT files, or do both.
- Paste multiple URLs for sequential batch downloads.
- Customize the output filename template. The default is `{date}.Mnet Plus.{title}.WEB-DL.{res}.{codec}.-{tag}.{ext}`. Available placeholders are `{date}` (YYMMDD publication date), `{title}`, `{res}` (for example `1080p`), `{resolution}` (for example `1080`), `{codec}` (automatically detected as H264 or HEVC), `{tag}` (release group, default `buguibgib`), and `{ext}`.

## Requirements

- Qt 6.5 or newer with Widgets, Network, Concurrent, and Sql components
- CMake 3.21 or newer
- `ffmpeg` and `ffprobe`

### Windows

- Visual Studio 2022 with **Desktop development with C++**, or another Qt 6.5+ C++17 toolchain
- Qt 6.5+ with Widgets, Network, Concurrent, and Sql components
- CMake 3.21+
- Put `ffmpeg.exe` and `ffprobe.exe` on `PATH`, or place them beside `MnetPlusDownloader.exe`

### macOS (Homebrew)

```bash
brew install qt cmake ffmpeg
```

## Build

### macOS and Linux

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
open build/MnetPlusDownloader.app
```

You can also run `build/MnetPlusDownloader.app/Contents/MacOS/MnetPlusDownloader` directly on macOS.

The source-only repository keeps the release build small. The optional QtTest sources used by the maintainer are kept locally and are enabled automatically when present; a fresh checkout builds the application without them.

### Windows (PowerShell)

Adjust the Qt installation path to match your machine:

```powershell
cmake -S . -B build -DCMAKE_PREFIX_PATH="C:\Qt\6.8.0\msvc2022_64"
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

With a Visual Studio generator, the executable is usually `build\Release\MnetPlusDownloader.exe`. Before distributing it, use `windeployqt` from the same Qt installation to collect Qt DLLs and platform plugins:

```powershell
& "C:\Qt\6.8.0\msvc2022_64\bin\windeployqt.exe" "build\Release\MnetPlusDownloader.exe"
```

## Browser Sessions and Privacy

The application reads Chrome or Edge browser sessions and keeps only cookies for `mnetplus.world` and its subdomains. The cookie database is copied to a temporary system directory for read-only access. Chromium Cookie decryption uses Keychain and AES on macOS, and DPAPI plus Windows CNG AES-GCM on Windows. Cookie values are never written to task logs or sent to sites other than Mnet Plus.

Cookie storage paths, PBKDF2/AES-CBC parameters, and Chrome 24+ host-key hashing follow the Chromium cookie implementation used by `yt-dlp`; the application does not call or depend on `yt-dlp`.

On the first browser-session read, macOS may ask for Keychain access. If access is declined, the application falls back to guest mode.

Full CDN streams use CloudFront signed cookies. The application uses FFmpeg's domain- and path-scoped cookie options instead of a bare `Cookie` header that could be sent across redirects. Because of FFmpeg's command-line interface, cookie values briefly appear in the FFmpeg process arguments during a download and disappear when FFmpeg exits.

When a download starts, the application uses `_mnet_atk` to create Bearer authorization, refreshes the CloudFront signed cookie through Mnet Plus's official media-cookie endpoint, preflights the video, and starts media and selected subtitle downloads concurrently.

## Access Limitations

The downloader handles media that the current browser session can access normally. It does not bypass purchase, regional, login, HDCP, or DRM restrictions. If the subtitle API returns `401` or `403`, the log explains the response and the application continues by creating an MKV without those subtitles.

Live authenticated-session diagnostics are skipped by default. To verify Edge login, media signing, HLS, audio, and `zh_CN` subtitle access without downloading a complete video:

```bash
MNETPLUS_LIVE_TEST=1 build/MnetPlusDownloaderLiveSessionTests -v1
```

## Downloads

Release packages are published on the [bgi-dl GitHub Releases page](https://github.com/IamYei/bgi-dl/releases/latest):

- `bgi-dl-Windows-x64.zip`
- `bgi-dl-macOS-arm64.zip`
- `bgi-dl-macOS-arm64.dmg`
