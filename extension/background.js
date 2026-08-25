// bgi-dl Cookie Helper — sends decrypted mnetplus.world cookies to the
// Mnet Plus Downloader native host via Native Messaging.
//
// The cookie values read through the extension API are already decrypted by
// the browser itself, which sidesteps Chrome 127+ app-bound encryption.

const NATIVE_HOST = "com.bgidl.cookiehost";
const COOKIE_DOMAINS = ["mnetplus.world"];
const BADGE_TIMEOUT_MS = 8000;

function isFirefox() {
  return typeof browser !== "undefined" && typeof chrome === "undefined";
}

function actionApi() {
  return isFirefox() ? browser.action : chrome.action;
}

async function collectCookies() {
  const all = [];
  const api = isFirefox() ? browser.cookies : chrome.cookies;
  for (const domain of COOKIE_DOMAINS) {
    const cookies = await api.getAll({ domain });
    all.push(...cookies);
  }
  return all;
}

function sendNativeMessage(message) {
  return new Promise((resolve, reject) => {
    if (isFirefox()) {
      browser.runtime.sendNativeMessage(NATIVE_HOST, message).then(resolve, reject);
    } else {
      chrome.runtime.sendNativeMessage(NATIVE_HOST, message, (response) => {
        if (chrome.runtime.lastError) {
          reject(new Error(chrome.runtime.lastError.message));
        } else {
          resolve(response);
        }
      });
    }
  });
}

// Badge text sits directly ON the toolbar icon — the only feedback the user
// is guaranteed to see without hovering. Cleared automatically after a few
// seconds so the icon doesn't stay dirty forever.
function showBadge(text, color, tabId) {
  const api = actionApi();
  const details = { text, color, tabId: tabId == null ? undefined : tabId };
  try {
    api.setBadgeBackgroundColor({ color, tabId: details.tabId });
    api.setBadgeText({ text, tabId: details.tabId });
  } catch (_) {
    // Some contexts (e.g. no active tab) reject badge updates; ignore.
  }
  setTimeout(() => {
    try {
      api.setBadgeText({ text: "", tabId: details.tabId });
    } catch (_) {
      // ignore
    }
  }, BADGE_TIMEOUT_MS);
}

function showNotification(title, message) {
  try {
    const api = isFirefox() ? browser.notifications : chrome.notifications;
    if (api && api.create) {
      api.create({
        type: "basic",
        iconUrl: chrome.runtime.getURL("icon128.png"),
        title,
        message,
      });
    }
  } catch (_) {
    // Notifications are best-effort; the badge already told the story.
  }
}

function resultMessage(result) {
  if (result.ok) {
    return `已推送 ${result.count} 条 Mnet Plus Cookie。\n现在回到下载器，选择 Cookie 来源为“浏览器扩展”，点击“解析”即可。`;
  }
  if (result.stage === "no-cookies") {
    return "当前浏览器里没有 mnetplus.world 的 Cookie。\n请先在本浏览器中登录 Mnet Plus（mnetplus.world），登录成功后再点这个图标。";
  }
  if (result.stage === "host") {
    return "无法连接下载器的本地助手。\n请先打开下载器 → Cookie 来源选“浏览器扩展” → 点击“安装浏览器助手”，装完后再回到浏览器点这个图标。";
  }
  if (result.stage === "host-status") {
    return `本地助手处理 Cookie 失败（${result.error}）。\n请重装“浏览器助手”后再试。`;
  }
  return `推送失败 — ${result.error}`;
}

async function pushCookies() {
  const cookies = await collectCookies();

  if (!cookies.length) {
    return {
      ok: false,
      stage: "no-cookies",
      count: 0,
      error: "no mnetplus.world cookies in this browser profile",
    };
  }

  const message = {
    type: "cookies",
    source: "extension",
    browser: isFirefox() ? "firefox" : "chromium",
    timestamp: Date.now(),
    cookies: cookies.map((c) => ({
      domain: c.domain,
      name: c.name,
      value: c.value,
      path: c.path || "/",
      secure: !!c.secure,
      httpOnly: !!c.httpOnly,
      sameSite: c.sameSite || "unspecified",
      expirationDate: c.expirationDate || 0,
      hostOnly: !!c.hostOnly,
    })),
  };

  let response;
  try {
    response = await sendNativeMessage(message);
  } catch (error) {
    // Chrome's error for a missing/unregistered host is a long
    // "Error when communicating with the native messaging host" string.
    return {
      ok: false,
      stage: "host",
      count: cookies.length,
      error: String(error),
    };
  }

  if (!response || response.status !== "ok") {
    return {
      ok: false,
      stage: "host-status",
      count: cookies.length,
      error: (response && response.error) || "host returned no status",
      response,
    };
  }

  return {
    ok: true,
    count: (response && typeof response.cookies === "number")
      ? response.cookies
      : cookies.length,
    response,
  };
}

async function notifyResult(tabId, result) {
  const message = resultMessage(result);

  if (result.ok) {
    showBadge("OK", "#2e7d32", tabId);
  } else if (result.stage === "no-cookies") {
    showBadge("0", "#e65100", tabId);
  } else {
    showBadge("!", "#c62828", tabId);
  }

  showNotification(
    result.ok ? "bgi-dl Cookie 推送成功" : "bgi-dl Cookie 推送失败",
    message
  );

  // Tooltip as a third channel — useful for users who missed badge+toast.
  try {
    const api = actionApi();
    const details = { title: message };
    if (tabId != null) details.tabId = tabId;
    api.setTitle(details);
  } catch (_) {
    // Title updates can fail for privileged pages; ignore.
  }
}

chrome.action.onClicked.addListener(async (tab) => {
  const tabId = tab ? tab.id : null;
  // Immediate "working" pulse so the click always feels acknowledged.
  showBadge("…", "#1565c0", tabId);
  const result = await pushCookies();
  await notifyResult(tabId, result);
});

// The downloader can ask the extension to push cookies on demand (used by
// the app's "从浏览器推送" flow) and to reveal the extension ID so the
// native manifest's allowed_origins can be matched.
async function rememberExtensionId() {
  try {
    const id = chrome.runtime.id;
    if (id && chrome.storage && chrome.storage.local) {
      await chrome.storage.local.set({ extensionId: id });
    }
  } catch (_) {
    // storage is optional; never let it break the push flow
  }
}

chrome.runtime.onMessage.addListener((message, _sender, sendResponse) => {
  if (message && message.type === "push-cookies") {
    pushCookies().then((result) => {
      if (result.ok) rememberExtensionId();
      sendResponse(result);
    });
    return true; // async response
  }
  if (message && message.type === "get-extension-id") {
    sendResponse({ id: chrome.runtime.id });
    return false;
  }
  return false;
});

// Store the id once at startup so it is available even before any push.
rememberExtensionId();
