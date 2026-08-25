// bgi-dl Cookie Helper — sends decrypted mnetplus.world cookies to the
// Mnet Plus Downloader native host via Native Messaging.
//
// The cookie values read through the extension API are already decrypted by
// the browser itself, which sidesteps Chrome 127+ app-bound encryption.

const NATIVE_HOST = "com.bgidl.cookiehost";
const COOKIE_DOMAINS = ["mnetplus.world"];

async function collectCookies() {
  const all = [];
  for (const domain of COOKIE_DOMAINS) {
    const cookies = await chrome.cookies.getAll({ domain });
    all.push(...cookies);
  }
  return all;
}

async function pushCookies() {
  const cookies = await collectCookies();
  const message = {
    type: "cookies",
    source: "extension",
    browser: navigator.userAgent.includes("Firefox") ? "firefox" : "chromium",
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
    await chrome.runtime.sendNativeMessage(NATIVE_HOST, message);
    return { ok: true, count: cookies.length };
  } catch (error) {
    return { ok: false, error: String(error) };
  }
}

chrome.action.onClicked.addListener(async (tab) => {
  const result = await pushCookies();
  const title = result.ok
    ? `bgi-dl: sent ${result.count} cookies`
    : `bgi-dl: failed — ${result.error}`;
  await chrome.action.setTitle({ tabId: tab.id, title });
});

// The native host may also ping us to request a fresh push.
chrome.runtime.onMessage.addListener((message, _sender, sendResponse) => {
  if (message && message.type === "push-cookies") {
    pushCookies().then(sendResponse);
    return true; // async response
  }
  return false;
});
