// bgi-dl Cookie Helper — sends decrypted mnetplus.world cookies to the
// Mnet Plus Downloader native host via Native Messaging.
//
// The cookie values read through the extension API are already decrypted by
// the browser itself, which sidesteps Chrome 127+ app-bound encryption.

const NATIVE_HOST = "com.bgidl.cookiehost";
const COOKIE_DOMAINS = ["mnetplus.world"];

function isFirefox() {
  return typeof browser !== "undefined" && typeof chrome === "undefined";
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

async function pushCookies() {
  const cookies = await collectCookies();
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

  try {
    const response = await sendNativeMessage(message);
    return { ok: true, count: cookies.length, response };
  } catch (error) {
    return { ok: false, count: cookies.length, error: String(error) };
  }
}

async function notifyResult(tabId, result) {
  const text = result.ok
    ? `bgi-dl: 已推送 ${result.count} 条 Mnet Plus Cookie，可以回到下载器点“解析”`
    : `bgi-dl: 推送失败 — ${result.error}。请确认已在下载器中点击“安装浏览器助手”完成注册`;
  try {
    if (isFirefox()) {
      await browser.action.setTitle({ tabId, title: text });
    } else if (tabId != null) {
      await chrome.action.setTitle({ tabId, title: text });
    }
  } catch (_) {
    // Title updates can fail for privileged pages; ignore.
  }
  // Also surface the result through a notification so the user always sees it.
  try {
    if (isFirefox()) {
      await browser.notifications.create({
        type: "basic",
        title: "bgi-dl Cookie Helper",
        message: text,
      });
    }
  } catch (_) {
    // Chrome MV3 service workers lack the notifications permission here; skip.
  }
}

chrome.action.onClicked.addListener(async (tab) => {
  const result = await pushCookies();
  await notifyResult(tab ? tab.id : null, result);
});

// The downloader can ask the extension to refresh its cached extension ID:
// after the first successful push we persist the extension id so the native
// manifest's allowed_origins can be updated by reinstalling the helper.
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
