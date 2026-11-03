// The launcher page talks to the program around it with small JSON messages.
// On Windows that's RuffDogsLauncher.exe, which runs this page on its own bundled Chromium (CEF) and serves it from
// https://launcher.ruffdogs/: the page POSTs messages to /_native and the program calls window.__rdNative(msg) back.
// On the Mac, dev/devserver.py stands in for it over HTTP (/native and /native/events).
const Native = (() => {
  const listeners = [];
  const win = location.hostname === 'launcher.ruffdogs';
  const wv = window.chrome && window.chrome.webview;     // launchers built before the engine change (WebView2)
  const emit = (m) => listeners.forEach((f) => f(m));
  if (wv) {
    wv.addEventListener('message', (e) => emit(typeof e.data === 'string' ? JSON.parse(e.data) : e.data));
  } else if (win) {
    window.__rdNative = (m) => emit(typeof m === 'string' ? JSON.parse(m) : m);
  } else {
    const es = new EventSource('/native/events');
    es.onmessage = (e) => emit(JSON.parse(e.data));
  }
  return {
    kind: win ? 'windows' : 'dev',
    send(msg) {
      if (wv) wv.postMessage(JSON.stringify(msg));
      else if (win) fetch('/_native', { method: 'POST', body: JSON.stringify(msg) }).catch(() => {});
      else fetch('/native', { method: 'POST', body: JSON.stringify(msg) }).catch(() => {});
    },
    on(f) { listeners.push(f); },
  };
})();
