#include "localization.h"

#include <QLocale>

#include <array>
#include <atomic>

namespace {
struct TranslationEntry
{
    const char *source;
    const char *english;
    const char *japanese;
    const char *korean;
};

// Source text is Simplified Chinese so existing Chinese installs keep their UI intact.
constexpr std::array kTranslations = {
    TranslationEntry{"就绪", "Ready", "準備完了", "준비 완료"},
    TranslationEntry{"视频下载器", "Video Downloader", "ビデオダウンローダー", "비디오 다운로더"},
    TranslationEntry{"浏览器会话：等待连接", "Browser session: waiting", "ブラウザーセッション: 接続待機中", "브라우저 세션: 연결 대기 중"},
    TranslationEntry{"语言", "Language", "言語", "언어"},
    TranslationEntry{"视频页面", "Video page", "動画ページ", "동영상 페이지"},
    TranslationEntry{"Mnet Plus 视频页面地址", "Mnet Plus video page URL", "Mnet Plus 動画ページ URL", "Mnet Plus 동영상 페이지 URL"},
    TranslationEntry{"浏览器会话来源", "Browser session source", "ブラウザーセッションの取得元", "브라우저 세션 소스"},
    TranslationEntry{"自动读取会话", "Auto-detect session", "セッションを自動検出", "세션 자동 감지"},
    TranslationEntry{"解析", "Resolve", "解析", "분석"},
    TranslationEntry{"媒体信息", "Media information", "メディア情報", "미디어 정보"},
    TranslationEntry{"视频", "Video", "動画", "비디오"},
    TranslationEntry{"音频", "Audio", "音声", "오디오"},
    TranslationEntry{"字幕", "Captions", "字幕", "자막"},
    TranslationEntry{"批量下载", "Batch download", "一括ダウンロード", "일괄 다운로드"},
    TranslationEntry{"每行一个视频页面地址，可一次粘贴多个", "One video page URL per line. Paste multiple URLs at once.", "動画ページ URL を1行に1件ずつ入力します。複数行を一度に貼り付けできます。", "한 줄에 동영상 페이지 URL 하나를 입력하세요. 여러 URL을 한 번에 붙여 넣을 수 있습니다."},
    TranslationEntry{"批量视频地址", "Batch video URLs", "一括動画 URL", "일괄 동영상 URL"},
    TranslationEntry{"输出设置", "Output settings", "出力設定", "출력 설정"},
    TranslationEntry{"内封字幕", "Embed captions", "字幕を埋め込む", "자막 포함"},
    TranslationEntry{"单独导出 SRT", "Export separate SRT files", "SRT を個別に出力", "별도 SRT 내보내기"},
    TranslationEntry{"将下载全部可用字幕", "All available captions will be downloaded", "利用可能なすべての字幕をダウンロードします", "사용 가능한 모든 자막을 다운로드합니다"},
    TranslationEntry{"输出目录", "Output directory", "出力フォルダー", "출력 폴더"},
    TranslationEntry{"选择输出目录", "Choose output directory", "出力フォルダーを選択", "출력 폴더 선택"},
    TranslationEntry{"文件名模板", "Filename template", "ファイル名テンプレート", "파일명 템플릿"},
    TranslationEntry{"可用占位符：{date} {title} {res} {resolution} {codec} {tag} {ext}", "Available placeholders: {date} {title} {res} {resolution} {codec} {tag} {ext}", "使用可能なプレースホルダー: {date} {title} {res} {resolution} {codec} {tag} {ext}", "사용 가능한 자리표시자: {date} {title} {res} {resolution} {codec} {tag} {ext}"},
    TranslationEntry{"恢复默认模板", "Restore default template", "既定のテンプレートに戻す", "기본 템플릿 복원"},
    TranslationEntry{"下载并合并 MKV", "Download and merge MKV", "MKV をダウンロードして結合", "MKV 다운로드 및 병합"},
    TranslationEntry{"取消当前任务", "Cancel current task", "現在のタスクをキャンセル", "현재 작업 취소"},
    TranslationEntry{"等待解析", "Waiting to resolve", "解析待ち", "분석 대기 중"},
    TranslationEntry{"任务日志", "Task log", "タスクログ", "작업 로그"},
    TranslationEntry{"文件名模板已恢复默认", "Filename template restored to default", "ファイル名テンプレートを既定に戻しました", "파일명 템플릿을 기본값으로 복원했습니다"},
    TranslationEntry{"已取消", "Canceled", "キャンセル済み", "취소됨"},
    TranslationEntry{"批量任务已取消", "Batch task canceled", "一括タスクをキャンセルしました", "일괄 작업이 취소되었습니다"},
    TranslationEntry{"解析已取消", "Resolution canceled", "解析をキャンセルしました", "분석이 취소되었습니다"},
    TranslationEntry{"正在连接 %1", "Connecting to %1", "%1 に接続中", "%1에 연결 중"},
    TranslationEntry{"正在读取 %1 的 Mnet Plus 会话", "Reading the Mnet Plus session from %1", "%1 の Mnet Plus セッションを読み込み中", "%1의 Mnet Plus 세션을 읽는 중"},
    TranslationEntry{"已连接 %1", "Connected to %1", "%1 に接続済み", "%1에 연결됨"},
    TranslationEntry{"%1 会话已连接，仅使用 Mnet Plus 域 Cookie", "%1 session connected; only Mnet Plus domain cookies are used", "%1 セッションに接続しました。Mnet Plus ドメインの Cookie だけを使用します", "%1 세션이 연결되었습니다. Mnet Plus 도메인 쿠키만 사용합니다"},
    TranslationEntry{"解析完成（%1/%2）：%3", "Resolved (%1/%2): %3", "解析完了 (%1/%2): %3", "분석 완료 (%1/%2): %3"},
    TranslationEntry{"解析失败（%1/%2）：%3", "Resolution failed (%1/%2): %3", "解析失敗 (%1/%2): %3", "분석 실패 (%1/%2): %3"},
    TranslationEntry{"解析失败", "Resolution failed", "解析失敗", "분석 실패"},
    TranslationEntry{"完成：%1", "Completed: %1", "完了: %1", "완료: %1"},
    TranslationEntry{"任务失败", "Task failed", "タスク失敗", "작업 실패"},
    TranslationEntry{"任务已取消", "Task canceled", "タスクをキャンセルしました", "작업이 취소되었습니다"},
    TranslationEntry{"请输入有效的视频页面地址", "Enter a valid video page URL", "有効な動画ページ URL を入力してください", "유효한 동영상 페이지 URL을 입력하세요"},
    TranslationEntry{"正在连接浏览器会话", "Connecting to browser session", "ブラウザーセッションに接続中", "브라우저 세션에 연결 중"},
    TranslationEntry{"Cookie 只用于 Mnet Plus 域", "Cookies are used only for the Mnet Plus domain", "Cookie は Mnet Plus ドメインにのみ使用されます", "쿠키는 Mnet Plus 도메인에서만 사용됩니다"},
    TranslationEntry{"开始解析：%1", "Resolving: %1", "解析開始: %1", "분석 시작: %1"},
    TranslationEntry{"游客模式", "Guest mode", "ゲストモード", "게스트 모드"},
    TranslationEntry{"正在解析页面", "Resolving page", "ページを解析中", "페이지 분석 중"},
    TranslationEntry{"预览流", "Preview stream", "プレビューストリーム", "미리보기 스트림"},
    TranslationEntry{"已找到", "Found", "検出済み", "찾음"},
    TranslationEntry{"随 HLS", "From HLS", "HLS から取得", "HLS에서 가져옴"},
    TranslationEntry{"将使用 HLS 中的音轨", "The audio track in HLS will be used", "HLS 内の音声トラックを使用します", "HLS의 오디오 트랙을 사용합니다"},
    TranslationEntry{"未公开", "Unavailable", "未公開", "공개되지 않음"},
    TranslationEntry{"页面未提供字幕配置", "The page does not provide a caption configuration", "ページに字幕設定がありません", "페이지에서 자막 구성을 제공하지 않습니다"},
    TranslationEntry{"字幕配置 %1", "Caption configuration %1", "字幕設定 %1", "자막 구성 %1"},
    TranslationEntry{"全部下载 · %1", "Download all · %1", "すべてダウンロード · %1", "모두 다운로드 · %1"},
    TranslationEntry{"解析完成", "Resolution complete", "解析完了", "분석 완료"},
    TranslationEntry{"当前会话只返回预览流", "The current session returned only a preview stream", "現在のセッションではプレビューストリームのみ返されました", "현재 세션은 미리보기 스트림만 반환했습니다"},
    TranslationEntry{"可以开始下载", "Ready to download", "ダウンロードを開始できます", "다운로드할 준비가 되었습니다"},
    TranslationEntry{"媒体解析完成：%1", "Media resolved: %1", "メディア解析完了: %1", "미디어 분석 완료: %1"},
    TranslationEntry{"请选择输出目录", "Choose an output directory", "出力フォルダーを選択してください", "출력 폴더를 선택하세요"},
    TranslationEntry{"请至少输入一个视频页面地址", "Enter at least one video page URL", "動画ページ URL を1件以上入力してください", "동영상 페이지 URL을 하나 이상 입력하세요"},
    TranslationEntry{"批量下载：共 %1 个地址，开始逐个解析", "Batch download: %1 URLs. Resolving one at a time.", "一括ダウンロード: %1 件の URL を順番に解析します。", "일괄 다운로드: URL %1개를 하나씩 분석합니다."},
    TranslationEntry{"批量解析", "Batch resolution", "一括解析", "일괄 분석"},
    TranslationEntry{"批量解析 %1/%2", "Batch resolution %1/%2", "一括解析 %1/%2", "일괄 분석 %1/%2"},
    TranslationEntry{"没有可下载的媒体，批量任务结束", "No downloadable media found. Batch task finished.", "ダウンロード可能なメディアが見つかりません。一括タスクを終了します。", "다운로드할 미디어가 없습니다. 일괄 작업을 종료합니다."},
    TranslationEntry{"批量结束", "Batch finished", "一括処理終了", "일괄 작업 완료"},
    TranslationEntry{"解析完成，开始下载 %1 个视频", "Resolution complete. Downloading %1 videos.", "解析完了。%1 件の動画をダウンロードします。", "분석 완료. 동영상 %1개 다운로드를 시작합니다."},
    TranslationEntry{"批量完成", "Batch complete", "一括完了", "일괄 완료"},
    TranslationEntry{"全部下载完成", "All downloads complete", "すべてのダウンロードが完了しました", "모든 다운로드가 완료되었습니다"},
    TranslationEntry{"批量下载全部完成", "All batch downloads completed", "一括ダウンロードがすべて完了しました", "모든 일괄 다운로드가 완료되었습니다"},
    TranslationEntry{"开始下载（剩余 %1）：%2", "Downloading (%1 remaining): %2", "ダウンロード開始（残り %1 件）: %2", "다운로드 시작(남은 항목 %1개): %2"},
    TranslationEntry{"批量下载中", "Batch downloading", "一括ダウンロード中", "일괄 다운로드 중"},
    TranslationEntry{"等待解析视频页面", "Waiting for a video page", "動画ページ待ち", "동영상 페이지 대기 중"},
    TranslationEntry{"等待", "Waiting", "待機中", "대기 중"},
    TranslationEntry{"浏览器", "Browser", "ブラウザー", "브라우저"},
    TranslationEntry{"退出下载器", "Exit downloader", "ダウンローダーを終了", "다운로더 종료"},
    TranslationEntry{"当前任务仍在运行。确定要取消并退出吗？", "A task is still running. Cancel it and exit?", "タスクがまだ実行中です。キャンセルして終了しますか？", "작업이 아직 실행 중입니다. 취소하고 종료할까요?"},
    TranslationEntry{"界面语言已切换为 %1", "Interface language changed to %1", "表示言語を %1 に切り替えました", "인터페이스 언어를 %1(으)로 변경했습니다"},
    TranslationEntry{"正在刷新媒体权限", "Refreshing media access", "メディアアクセスを更新中", "미디어 접근 권한을 갱신하는 중"},
    TranslationEntry{"正在请求 CloudFront 签名", "Requesting CloudFront signature", "CloudFront 署名をリクエスト中", "CloudFront 서명을 요청하는 중"},
    TranslationEntry{"正在验证视频流权限", "Verifying video stream access", "動画ストリームのアクセスを確認中", "동영상 스트림 접근 권한을 확인하는 중"},
    TranslationEntry{"正在并行下载", "Downloading in parallel", "並列ダウンロード中", "병렬 다운로드 중"},
    TranslationEntry{"正在本地合并音视频", "Merging video and audio locally", "動画と音声をローカルで結合中", "동영상과 오디오를 로컬에서 병합하는 중"},
    TranslationEntry{"并发下载完成，正在进行本地流复制", "Parallel downloads complete; copying streams locally", "並列ダウンロード完了。ローカルでストリームをコピー中", "병렬 다운로드가 완료되었습니다. 로컬 스트림을 복사하는 중"},
    TranslationEntry{"正在查询字幕列表", "Checking caption list", "字幕リストを確認中", "자막 목록을 확인하는 중"},
    TranslationEntry{"字幕完成，正在下载媒体", "Captions complete; downloading media", "字幕の処理完了。メディアをダウンロード中", "자막 완료. 미디어를 다운로드하는 중"},
    TranslationEntry{"正在快速内封字幕", "Embedding captions", "字幕を埋め込み中", "자막을 포함하는 중"},
    TranslationEntry{"正在进行本地流复制", "Copying streams locally", "ローカルでストリームをコピー中", "로컬 스트림을 복사하는 중"},
    TranslationEntry{"媒体完成，正在获取字幕", "Media complete; fetching captions", "メディア完了。字幕を取得中", "미디어 완료. 자막을 가져오는 중"},
    TranslationEntry{"已完成", "Completed", "完了", "완료됨"},
    TranslationEntry{"MKV 与 SRT 已保存", "MKV and SRT files saved", "MKV と SRT を保存しました", "MKV 및 SRT 파일이 저장되었습니다"},
    TranslationEntry{"MKV 已保存", "MKV saved", "MKV を保存しました", "MKV가 저장되었습니다"},
    TranslationEntry{"视频分片 %1/%2 · 音频同时下载", "Video segments %1/%2 · audio downloading concurrently", "動画セグメント %1/%2 ・音声を同時にダウンロード中", "비디오 세그먼트 %1/%2 · 오디오 동시 다운로드 중"},
    TranslationEntry{"媒体已下载 %1", "Media downloaded %1", "メディアをダウンロード済み %1", "미디어 다운로드됨 %1"},
    TranslationEntry{"字幕已内封 %1", "Captions embedded %1", "字幕を埋め込み済み %1", "자막 포함됨 %1"},
    TranslationEntry{"未找到 Mnet Plus 浏览器会话，将使用游客模式", "No Mnet Plus browser session found. Using guest mode.", "Mnet Plus のブラウザーセッションが見つかりません。ゲストモードを使用します。", "Mnet Plus 브라우저 세션을 찾지 못했습니다. 게스트 모드를 사용합니다."},
    TranslationEntry{"浏览器会话仍在读取，将暂时使用游客模式", "Browser session is still loading. Using guest mode for now.", "ブラウザーセッションを読み込み中です。いったんゲストモードを使用します。", "브라우저 세션을 아직 읽는 중입니다. 우선 게스트 모드를 사용합니다."},
    TranslationEntry{"没有检测到可读取的浏览器配置，将使用游客模式", "No readable browser profile was detected. Using guest mode.", "読み取り可能なブラウザープロファイルが検出されませんでした。ゲストモードを使用します。", "읽을 수 있는 브라우저 프로필이 감지되지 않았습니다. 게스트 모드를 사용합니다."},
    TranslationEntry{"未找到 Mnet Plus 登录会话，将使用游客模式（%1）", "No Mnet Plus sign-in session found. Using guest mode (%1).", "Mnet Plus のログインセッションが見つかりません。ゲストモードを使用します (%1)。", "Mnet Plus 로그인 세션을 찾지 못했습니다. 게스트 모드를 사용합니다(%1)."},
    TranslationEntry{"%1 配置不存在", "%1 profile does not exist", "%1 のプロファイルが存在しません", "%1 프로필이 없습니다"},
    TranslationEntry{"无法读取 %1 的浏览器解密密钥", "Could not read the browser decryption key for %1", "%1 のブラウザー復号鍵を読み取れませんでした", "%1의 브라우저 복호화 키를 읽을 수 없습니다"},
    TranslationEntry{"%1 中没有 Mnet Plus Cookie", "No Mnet Plus cookies found in %1", "%1 に Mnet Plus の Cookie がありません", "%1에 Mnet Plus 쿠키가 없습니다"},
    TranslationEntry{"请输入有效的 mnetplus.world 视频页面地址", "Enter a valid mnetplus.world video page URL", "有効な mnetplus.world 動画ページ URL を入力してください", "유효한 mnetplus.world 동영상 페이지 URL을 입력하세요"},
    TranslationEntry{"地址中没有找到 Mnet Plus 视频 ID", "No Mnet Plus video ID was found in the URL", "URL に Mnet Plus 動画 ID がありません", "URL에서 Mnet Plus 동영상 ID를 찾지 못했습니다"},
    TranslationEntry{"站点拒绝访问（HTTP %1），请确认浏览器已登录 Mnet Plus", "The site denied access (HTTP %1). Check that the browser is signed in to Mnet Plus.", "サイトに拒否されました (HTTP %1)。ブラウザーで Mnet Plus にログインしているか確認してください。", "사이트가 액세스를 거부했습니다(HTTP %1). 브라우저가 Mnet Plus에 로그인되어 있는지 확인하세요."},
    TranslationEntry{"页面请求失败：%1", "Page request failed: %1", "ページリクエストに失敗しました: %1", "페이지 요청 실패: %1"},
    TranslationEntry{"页面中没有找到可公开访问的 HLS 视频流；内容可能需要登录、购买或受 DRM 保护", "No publicly accessible HLS video stream was found. The content may require sign-in, purchase, or DRM.", "公開アクセス可能な HLS 動画ストリームが見つかりません。ログイン、購入、または DRM が必要な可能性があります。", "공개적으로 액세스할 수 있는 HLS 동영상 스트림을 찾지 못했습니다. 로그인, 구매 또는 DRM이 필요할 수 있습니다."},
    TranslationEntry{"HLS 清单地址无效或不受支持", "The HLS manifest URL is invalid or unsupported", "HLS マニフェスト URL が無効または未対応です", "HLS 매니페스트 URL이 유효하지 않거나 지원되지 않습니다"},
    TranslationEntry{"无法创建视频分片目录", "Could not create the video segment directory", "動画セグメントフォルダーを作成できません", "동영상 세그먼트 폴더를 만들 수 없습니다"},
    TranslationEntry{"HLS 清单层级或地址无效", "The HLS manifest depth or URL is invalid", "HLS マニフェストの階層または URL が無効です", "HLS 매니페스트 깊이 또는 URL이 유효하지 않습니다"},
    TranslationEntry{"服务器返回的不是有效 HLS 清单", "The server did not return a valid HLS manifest", "サーバーが有効な HLS マニフェストを返しませんでした", "서버가 유효한 HLS 매니페스트를 반환하지 않았습니다"},
    TranslationEntry{"HLS master 没有可用的视频流", "The HLS master has no usable video stream", "HLS マスターに利用可能な動画ストリームがありません", "HLS 마스터에 사용할 수 있는 동영상 스트림이 없습니다"},
    TranslationEntry{"视频清单确认：%1 个资源，%2 路并发下载", "Video manifest confirmed: %1 resources, %2 concurrent downloads", "動画マニフェストを確認: %1 リソース、%2 並列ダウンロード", "동영상 매니페스트 확인: 리소스 %1개, 동시 다운로드 %2개"},
    TranslationEntry{"视频分片下载失败（HTTP %1）：%2", "Video segment download failed (HTTP %1): %2", "動画セグメントのダウンロードに失敗しました (HTTP %1): %2", "동영상 세그먼트 다운로드 실패(HTTP %1): %2"},
    TranslationEntry{"视频分片写入失败：%1", "Could not write the video segment: %1", "動画セグメントを書き込めません: %1", "동영상 세그먼트를 쓸 수 없습니다: %1"},
    TranslationEntry{"媒体与字幕将同时下载", "Media and captions will download together", "メディアと字幕を同時にダウンロードします", "미디어와 자막을 동시에 다운로드합니다"},
    TranslationEntry{"输出目录：%1", "Output directory: %1", "出力フォルダー: %1", "출력 폴더: %1"},
    TranslationEntry{"已从 HLS master 解析到独立音轨：%1", "Found a separate audio track in the HLS master: %1", "HLS マスターから独立音声トラックを検出: %1", "HLS 마스터에서 별도 오디오 트랙을 찾음: %1"},
    TranslationEntry{"已从 variant 匹配独立音频流：%1", "Matched the separate audio stream from the variant: %1", "variant から独立音声ストリームを照合: %1", "variant에서 별도 오디오 스트림을 찾음: %1"},
    TranslationEntry{"视频下载失败：%1", "Video download failed: %1", "動画のダウンロードに失敗しました: %1", "동영상 다운로드 실패: %1"},
    TranslationEntry{"视频流权限验证失败（HTTP %1）。请确认所选浏览器已登录并可播放该视频", "Video stream access verification failed (HTTP %1). Check that the selected browser is signed in and can play this video.", "動画ストリームのアクセス確認に失敗しました (HTTP %1)。選択したブラウザーでログインして再生できるか確認してください。", "동영상 스트림 접근 권한 확인 실패(HTTP %1). 선택한 브라우저가 로그인되어 있고 이 동영상을 재생할 수 있는지 확인하세요."},
    TranslationEntry{"视频流权限验证失败：%1", "Video stream access verification failed: %1", "動画ストリームのアクセス確認に失敗しました: %1", "동영상 스트림 접근 권한 확인 실패: %1"},
    TranslationEntry{"视频流权限验证通过，分辨率将在下载时自动探测", "Video stream access verified; resolution will be detected during download", "動画ストリームのアクセスを確認しました。ダウンロード時に解像度を自動検出します", "동영상 스트림 접근 권한이 확인되었습니다. 다운로드 중 해상도를 자동 감지합니다"},
    TranslationEntry{"视频清单读取失败，正在重试（%1/2）", "Could not read the video manifest. Retrying (%1/2)", "動画マニフェストを読み取れません。再試行中 (%1/2)", "동영상 매니페스트를 읽을 수 없습니다. 재시도 중(%1/2)"},
    TranslationEntry{"已自动匹配独立音频流：%1", "Matched a separate audio stream automatically: %1", "独立音声ストリームを自動照合: %1", "별도 오디오 스트림을 자동으로 찾음: %1"},
    TranslationEntry{"视频分片并发下载完成", "Concurrent video segment downloads complete", "動画セグメントの並列ダウンロードが完了しました", "동영상 세그먼트 동시 다운로드가 완료되었습니다"},
    TranslationEntry{"视频与音频下载完成", "Video and audio downloads complete", "動画と音声のダウンロードが完了しました", "동영상 및 오디오 다운로드가 완료되었습니다"},
    TranslationEntry{"视频 ID 查询不到字幕配置，将继续下载媒体", "No caption configuration found for the video ID; continuing media download", "動画 ID に字幕設定がありません。メディアのダウンロードを続行します", "동영상 ID에서 자막 구성을 찾지 못했습니다. 미디어 다운로드를 계속합니다"},
    TranslationEntry{"页面未内嵌字幕配置，正在通过视频 ID 查询字幕列表", "The page has no embedded caption configuration; checking the caption list by video ID", "ページに字幕設定がありません。動画 ID で字幕リストを確認中", "페이지에 포함된 자막 구성이 없습니다. 동영상 ID로 자막 목록을 확인하는 중"},
    TranslationEntry{"页面没有提供可靠视频时长，为避免截断将跳过字幕", "The page did not provide a reliable duration; skipping captions to avoid truncation", "信頼できる動画時間がないため、途中で切れないよう字幕をスキップします", "페이지에서 신뢰할 수 있는 재생 시간을 제공하지 않아 잘림을 막기 위해 자막을 건너뜁니다"},
    TranslationEntry{"开始并行读取 %1 种字幕，最大并发数 %2", "Reading %1 caption languages in parallel, max concurrency %2", "%1 種類の字幕を並列取得中。最大同時実行数 %2", "자막 언어 %1개를 병렬로 읽는 중, 최대 동시성 %2"},
    TranslationEntry{"字幕窗口 %1/%2 · 已获取 %3 条", "Caption window %1/%2 · %3 cues received", "字幕ウィンドウ %1/%2 ・%3 件取得", "자막 창 %1/%2 · %3개 수신"},
    TranslationEntry{"已准备 %1 条字幕轨", "%1 caption tracks ready", "%1 件の字幕トラックを準備しました", "자막 트랙 %1개 준비됨"},
    TranslationEntry{"%1 未获取到可用字幕", "No usable captions received for %1", "%1 の利用可能な字幕を取得できませんでした", "%1에서 사용할 수 있는 자막을 받지 못했습니다"},
    TranslationEntry{"%1 字幕转换完成，共 %2 条", "%1 captions converted, %2 cues", "%1 の字幕変換完了、%2 件", "%1 자막 변환 완료, %2개"},
    TranslationEntry{"%1 字幕文件写入失败", "Could not write the %1 caption file", "%1 の字幕ファイルを書き込めません", "%1 자막 파일을 쓸 수 없습니다"},
    TranslationEntry{"%1 字幕窗口 %2 暂时失败，正在重试（%3/2）", "%1 caption window %2 failed temporarily; retrying (%3/2)", "%1 の字幕ウィンドウ %2 が一時的に失敗。再試行中 (%3/2)", "%1 자막 창 %2가 일시적으로 실패했습니다. 재시도 중(%3/2)"},
    TranslationEntry{"%1 字幕超过安全读取上限，已丢弃该语言", "%1 captions exceed the safe read limit; language discarded", "%1 の字幕が安全な読み取り上限を超えたため破棄しました", "%1 자막이 안전한 읽기 한도를 초과하여 언어를 버렸습니다"},
    TranslationEntry{"SRT 已导出：%1", "SRT exported: %1", "SRT を出力しました: %1", "SRT 내보내기 완료: %1"},
    TranslationEntry{"独立 SRT 文件导出失败", "Separate SRT export failed", "個別 SRT の出力に失敗しました", "별도 SRT 내보내기 실패"},
    TranslationEntry{"没有获取到完整字幕，因此未导出 SRT", "No complete captions received; SRT was not exported", "完全な字幕を取得できなかったため SRT を出力しませんでした", "완전한 자막을 받지 못해 SRT를 내보내지 않았습니다"},
    TranslationEntry{"实际视频：%1x%2 · %3", "Actual video: %1x%2 · %3", "実際の動画: %1x%2 ・%3", "실제 동영상: %1x%2 · %3"},
    TranslationEntry{"无法确定输出文件名", "Could not determine the output filename", "出力ファイル名を決定できません", "출력 파일명을 결정할 수 없습니다"},
    TranslationEntry{"无法提交已完成的 MKV 文件", "Could not commit the completed MKV file", "完成した MKV ファイルを確定できません", "완성된 MKV 파일을 저장할 수 없습니다"},
    TranslationEntry{"无法创建临时目录", "Could not create the temporary directory", "一時フォルダーを作成できません", "임시 폴더를 만들 수 없습니다"},
    TranslationEntry{"无法创建输出目录：%1", "Could not create the output directory: %1", "出力フォルダーを作成できません: %1", "출력 폴더를 만들 수 없습니다: %1"},
    TranslationEntry{"未找到 ffmpeg，请先安装并确保它位于 PATH 中", "ffmpeg was not found. Install it and add it to PATH.", "ffmpeg が見つかりません。インストールして PATH に追加してください", "ffmpeg를 찾지 못했습니다. 설치 후 PATH에 추가하세요."},
    TranslationEntry{"未找到 ffmpeg，无法下载独立音频", "ffmpeg was not found; cannot download separate audio", "ffmpeg がないため独立音声をダウンロードできません", "ffmpeg가 없어 별도 오디오를 다운로드할 수 없습니다"},
    TranslationEntry{"未找到 ffmpeg，无法合并视频与音频", "ffmpeg was not found; cannot merge video and audio", "ffmpeg がないため動画と音声を結合できません", "ffmpeg가 없어 동영상과 오디오를 병합할 수 없습니다"},
    TranslationEntry{"未找到 ffmpeg，无法内封字幕", "ffmpeg was not found; cannot embed captions", "ffmpeg がないため字幕を埋め込めません", "ffmpeg가 없어 자막을 포함할 수 없습니다"},
    TranslationEntry{"ffmpeg 启动失败：%1", "Could not start ffmpeg: %1", "ffmpeg を起動できません: %1", "ffmpeg를 시작할 수 없습니다: %1"},
    TranslationEntry{"ffmpeg 返回代码 %1", "ffmpeg returned code %1", "ffmpeg の戻りコード %1", "ffmpeg 반환 코드 %1"},
    TranslationEntry{"独立音频下载进程启动失败：%1", "Could not start the separate audio process: %1", "独立音声プロセスを起動できません: %1", "별도 오디오 프로세스를 시작할 수 없습니다: %1"},
    TranslationEntry{"独立音频下载失败：%1", "Separate audio download failed: %1", "独立音声のダウンロードに失敗しました: %1", "별도 오디오 다운로드 실패: %1"},
    TranslationEntry{"独立音频下载完成", "Separate audio download complete", "独立音声のダウンロードが完了しました", "별도 오디오 다운로드 완료"},
    TranslationEntry{"独立音频已与视频分片同时开始下载", "Separate audio started downloading with video segments", "独立音声を動画セグメントと同時にダウンロード開始", "별도 오디오가 동영상 세그먼트와 함께 다운로드를 시작했습니다"},
    TranslationEntry{"正在本地合并音视频", "Merging video and audio locally", "動画と音声をローカルで結合中", "동영상과 오디오를 로컬에서 병합하는 중"},
    TranslationEntry{"本地音视频合并启动失败：%1", "Could not start local video/audio merge: %1", "ローカルの動画・音声結合を開始できません: %1", "로컬 동영상/오디오 병합을 시작할 수 없습니다: %1"},
    TranslationEntry{"未找到 ffprobe，无法验证 MKV 轨道完整性", "ffprobe was not found; cannot validate MKV tracks", "ffprobe がないため MKV トラックを検証できません", "ffprobe가 없어 MKV 트랙을 검증할 수 없습니다"},
    TranslationEntry{"MKV 轨道验证超时", "MKV track validation timed out", "MKV トラックの検証がタイムアウトしました", "MKV 트랙 검증 시간이 초과되었습니다"},
    TranslationEntry{"ffprobe 验证失败：%1", "ffprobe validation failed: %1", "ffprobe の検証に失敗しました: %1", "ffprobe 검증 실패: %1"},
    TranslationEntry{"返回代码 %1", "Return code %1", "戻りコード %1", "반환 코드 %1"},
    TranslationEntry{"ffprobe 返回了无效的轨道信息", "ffprobe returned invalid track information", "ffprobe が無効なトラック情報を返しました", "ffprobe가 잘못된 트랙 정보를 반환했습니다"},
    TranslationEntry{"合并结果缺少视频或音频轨道，已删除不完整文件", "Merged output is missing a video or audio track; incomplete file removed", "結合結果に動画または音声トラックがないため不完全なファイルを削除しました", "병합 결과에 동영상 또는 오디오 트랙이 없어 불완전한 파일을 삭제했습니다"},
    TranslationEntry{"音视频时长不一致（视频 %1 秒，音频 %2 秒），已删除不完整文件", "Video and audio durations differ (video %1 s, audio %2 s); incomplete file removed", "動画と音声の長さが一致しないため不完全なファイルを削除しました (動画 %1 秒、音声 %2 秒)", "동영상과 오디오 길이가 다릅니다(동영상 %1초, 오디오 %2초). 불완전한 파일을 삭제했습니다"},
    TranslationEntry{"合并结果缺少字幕轨道，已删除不完整文件", "Merged output is missing a caption track; incomplete file removed", "結合結果に字幕トラックがないため不完全なファイルを削除しました", "병합 결과에 자막 트랙이 없어 불완전한 파일을 삭제했습니다"},
    TranslationEntry{"字幕内封失败：%1", "Caption embedding failed: %1", "字幕の埋め込みに失敗しました: %1", "자막 포함 실패: %1"},
    TranslationEntry{"没有获取到完整字幕，因此未导出 SRT", "No complete captions received; SRT was not exported", "完全な字幕を取得できなかったため SRT を出力しませんでした", "완전한 자막을 받지 못해 SRT를 내보내지 않았습니다"},
    TranslationEntry{"媒体下载失败：%1", "Media download failed: %1", "メディアのダウンロードに失敗しました: %1", "미디어 다운로드 실패: %1"},
    TranslationEntry{"CloudFront 媒体签名已刷新", "CloudFront media signature refreshed", "CloudFront メディア署名を更新しました", "CloudFront 미디어 서명을 갱신했습니다"},
    TranslationEntry{"HLS BYTERANGE 格式无效", "Invalid HLS BYTERANGE format", "HLS BYTERANGE の形式が無効です", "잘못된 HLS BYTERANGE 형식"},
    TranslationEntry{"HLS BYTERANGE 缺少有效偏移量", "HLS BYTERANGE is missing a valid offset", "HLS BYTERANGE に有効なオフセットがありません", "HLS BYTERANGE에 유효한 오프셋이 없습니다"},
    TranslationEntry{"HLS MAP BYTERANGE 格式无效", "Invalid HLS MAP BYTERANGE format", "HLS MAP BYTERANGE の形式が無効です", "잘못된 HLS MAP BYTERANGE 형식"},
    TranslationEntry{"HLS MAP BYTERANGE 缺少偏移量", "HLS MAP BYTERANGE is missing an offset", "HLS MAP BYTERANGE にオフセットがありません", "HLS MAP BYTERANGE에 오프셋이 없습니다"},
    TranslationEntry{"HLS 包含不受支持的视频分片地址", "HLS contains an unsupported video segment URL", "HLS に未対応の動画セグメント URL があります", "HLS에 지원되지 않는 동영상 세그먼트 URL이 있습니다"},
    TranslationEntry{"HLS 包含不受支持的资源地址", "HLS contains an unsupported resource URL", "HLS に未対応のリソース URL があります", "HLS에 지원되지 않는 리소스 URL이 있습니다"},
    TranslationEntry{"HLS 清单中没有可下载的视频分片", "The HLS manifest has no downloadable video segments", "HLS マニフェストにダウンロード可能な動画セグメントがありません", "HLS 매니페스트에 다운로드할 동영상 세그먼트가 없습니다"},
    TranslationEntry{"HLS 清单读取失败（HTTP %1）：%2", "Could not read the HLS manifest (HTTP %1): %2", "HLS マニフェストを読み取れません (HTTP %1): %2", "HLS 매니페스트 읽기 실패(HTTP %1): %2"},
    TranslationEntry{"媒体权限刷新失败：%1；将检查现有签名", "Media access refresh failed: %1; checking the existing signature", "メディアアクセスの更新に失敗しました: %1。既存の署名を確認します", "미디어 접근 권한 갱신 실패: %1. 기존 서명을 확인합니다"},
    TranslationEntry{"媒体权限刷新被拒绝（HTTP %1），将检查现有签名", "Media access refresh was denied (HTTP %1); checking the existing signature", "メディアアクセスの更新を拒否されました (HTTP %1)。既存の署名を確認します", "미디어 접근 권한 갱신이 거부되었습니다(HTTP %1). 기존 서명을 확인합니다"},
    TranslationEntry{"字幕列表查询失败（HTTP %1）：%2；将跳过字幕", "Caption list request failed (HTTP %1): %2; captions will be skipped", "字幕リストの取得に失敗しました (HTTP %1): %2。字幕をスキップします", "자막 목록 요청 실패(HTTP %1): %2. 자막을 건너뜁니다"},
    TranslationEntry{"尝试更高分辨率 %1p：%2", "Trying higher resolution %1p: %2", "高解像度 %1p を試行中: %2", "더 높은 해상도 %1p 시도 중: %2"},
    TranslationEntry{"已选择 %1p 视频流候选", "Selected the %1p video stream candidate", "%1p 動画ストリーム候補を選択しました", "%1p 동영상 스트림 후보를 선택했습니다"},
    TranslationEntry{"无法写入本地 HLS 清单", "Could not write the local HLS manifest", "ローカル HLS マニフェストを書き込めません", "로컬 HLS 매니페스트를 쓸 수 없습니다"},
    TranslationEntry{"暂不支持仍在更新的直播 HLS 清单", "Live HLS manifests that are still updating are not supported", "更新中のライブ HLS マニフェストにはまだ対応していません", "아직 업데이트 중인 라이브 HLS 매니페스트는 지원되지 않습니다"},
    TranslationEntry{"本地 HLS 清单写入失败", "Could not write the local HLS manifest", "ローカル HLS マニフェストの書き込みに失敗しました", "로컬 HLS 매니페스트 쓰기 실패"},
    TranslationEntry{"清单 %1 不可用（HTTP %2），尝试下一档 %3p", "Manifest %1 is unavailable (HTTP %2); trying the next %3p", "マニフェスト %1 は利用できません (HTTP %2)。次の %3p を試行します", "매니페스트 %1을 사용할 수 없습니다(HTTP %2). 다음 %3p를 시도합니다"},
    TranslationEntry{"视频分片内容为空", "Video segment content is empty", "動画セグメントの内容が空です", "동영상 세그먼트 내용이 비어 있습니다"},
    TranslationEntry{"视频分片字节范围长度不匹配", "Video segment byte range length does not match", "動画セグメントのバイト範囲の長さが一致しません", "동영상 세그먼트 바이트 범위 길이가 일치하지 않습니다"},
    TranslationEntry{"输出文件：%1", "Output file: %1", "出力ファイル: %1", "출력 파일: %1"},
    TranslationEntry{"没有可下载的视频流", "No downloadable video stream", "ダウンロード可能な動画ストリームがありません", "다운로드할 동영상 스트림이 없습니다"},
};

std::atomic<AppLocale::Language> s_language{AppLocale::Language::Chinese};

AppLocale::Language languageForCode(const QString &code, bool *valid = nullptr)
{
    const QString normalized = code.trimmed().toLower().replace(QLatin1Char('-'), QLatin1Char('_'));
    if (normalized == QStringLiteral("en") || normalized.startsWith(QStringLiteral("en_"))) {
        if (valid) *valid = true;
        return AppLocale::Language::English;
    }
    if (normalized == QStringLiteral("zh") || normalized.startsWith(QStringLiteral("zh_"))) {
        if (valid) *valid = true;
        return AppLocale::Language::Chinese;
    }
    if (normalized == QStringLiteral("ja") || normalized.startsWith(QStringLiteral("ja_"))) {
        if (valid) *valid = true;
        return AppLocale::Language::Japanese;
    }
    if (normalized == QStringLiteral("ko") || normalized.startsWith(QStringLiteral("ko_"))) {
        if (valid) *valid = true;
        return AppLocale::Language::Korean;
    }
    if (valid) *valid = false;
    return AppLocale::Language::Chinese;
}
} // namespace

namespace AppLocale {

Language language()
{
    return s_language.load(std::memory_order_relaxed);
}

QString languageCode()
{
    switch (language()) {
    case Language::English: return QStringLiteral("en");
    case Language::Chinese: return QStringLiteral("zh_CN");
    case Language::Japanese: return QStringLiteral("ja");
    case Language::Korean: return QStringLiteral("ko");
    }
    return QStringLiteral("zh_CN");
}

QString defaultLanguageCode()
{
    const QStringList systemLanguages = QLocale::system().uiLanguages();
    for (const QString &candidate : systemLanguages) {
        bool valid = false;
        languageForCode(candidate, &valid);
        if (valid) return candidate;
    }
    return QStringLiteral("zh_CN");
}

bool setLanguage(const QString &code)
{
    bool valid = false;
    const Language selected = languageForCode(code, &valid);
    if (!valid) return false;
    s_language.store(selected, std::memory_order_relaxed);
    return true;
}

QString text(const char *source)
{
    const Language selected = language();
    if (selected == Language::Chinese) return QString::fromUtf8(source);

    for (const TranslationEntry &entry : kTranslations) {
        if (QString::fromUtf8(entry.source) != QString::fromUtf8(source)) continue;
        switch (selected) {
        case Language::English: return QString::fromUtf8(entry.english);
        case Language::Japanese: return QString::fromUtf8(entry.japanese);
        case Language::Korean: return QString::fromUtf8(entry.korean);
        case Language::Chinese: break;
        }
    }
    return QString::fromUtf8(source);
}

QString text(const QString &source)
{
    return text(source.toUtf8().constData());
}

} // namespace AppLocale
