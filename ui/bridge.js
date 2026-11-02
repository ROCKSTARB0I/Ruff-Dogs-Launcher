// The launcher page talks to the program around it with small JSON messages.
// On Windows that's RuffDogsLauncher.exe (WebView2 postMessage). On the Mac, dev/devserver.py stands in for it over HTTP.
const Native = (() => {
  const listeners = [];
  const wv = window.chrome && window.chrome.webview;
  const emit = (m) => listeners.forEach((f) => f(m));
  if (wv) {
    wv.addEventListener('message', (e) => emit(typeof e.data === 'string' ? JSON.parse(e.data) : e.data));
  } else {
    const es = new EventSource('/native/events');
    es.onmessage = (e) => emit(JSON.parse(e.data));
  }
  return {
    kind: wv ? 'windows' : 'dev',
    send(msg) {
      if (wv) wv.postMessage(JSON.stringify(msg));
      else fetch('/native', { method: 'POST', body: JSON.stringify(msg) }).catch(() => {});
    },
    on(f) { listeners.push(f); },
  };
})();
