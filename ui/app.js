// Ruff Dogs Launcher: store, library, downloads, accounts. Plain JS, no framework.
// Talks to the program around it through Native (bridge.js). Games and the store list come from GitHub (see tools/sync.py).
'use strict';

const $ = (s, el = document) => el.querySelector(s);
const esc = (s) => String(s ?? '').replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
const store = {
  get(k, d) { try { const v = localStorage.getItem(k); return v == null ? d : JSON.parse(v); } catch { return d; } },
  set(k, v) { try { localStorage.setItem(k, JSON.stringify(v)); } catch {} },
  del(k) { try { localStorage.removeItem(k); } catch {} },
};
const ICON = {
  play: '<svg viewBox="0 0 24 24"><path d="M7 4.5v15l13-7.5z"/></svg>',
  down: '<svg viewBox="0 0 24 24"><path d="M11 3h2v10.2l3.6-3.6 1.4 1.4-6 6-6-6 1.4-1.4 3.6 3.6zM4 19h16v2H4z"/></svg>',
  stop: '<svg viewBox="0 0 24 24"><path d="M6 6h12v12H6z"/></svg>',
  dots: '<svg viewBox="0 0 24 24"><circle cx="5" cy="12" r="2"/><circle cx="12" cy="12" r="2"/><circle cx="19" cy="12" r="2"/></svg>',
};

const S = {
  config: {}, games: [], byId: {}, base: '', catalogWhen: '',
  native: { installed: {}, library: {}, running: null, app: {} },
  session: null, server: { url: '', status: 'checking' },
  queue: [], active: null, finished: [],
  route: { page: 'store', id: null }, libFilter: 'all', storeFilter: 'All', storeSearch: '', libSearch: '',
  settings: Object.assign({ autoUpdate: true, shortcut: true, notify: true }, store.get('rd_settings', {})),
};
const user = () => (S.session && S.session.name ? S.session.name.toLowerCase() : 'guest');
const owned = () => new Set(store.get('rd_owned_' + user(), []));
const own = (id) => { const o = owned(); o.add(id); store.set('rd_owned_' + user(), [...o]); };
const playData = () => store.get('rd_play_' + user(), {});
const installed = (id) => S.native.installed && S.native.installed[id];
const needsUpdate = (g) => { const i = installed(g.id); return i && i.commit !== g.commit; };

// ------------------------------------------------------------------------------------------------ formatting
const fmtSize = (b) => b >= 1073741824 ? (b / 1073741824).toFixed(1) + ' GB' : b >= 1048576 ? (b / 1048576).toFixed(1) + ' MB' : Math.max(1, Math.round(b / 1024)) + ' KB';
const fmtDate = (d) => d ? new Date(/^\d{4}-\d\d-\d\d$/.test(d) ? d + 'T12:00:00' : d).toLocaleDateString(undefined, { month: 'short', day: 'numeric', year: 'numeric' }) : '';
function fmtAgo(d) {
  if (!d) return 'Never';
  const s = (Date.now() - new Date(d)) / 1000;
  if (s < 90) return 'Just now';
  if (s < 3600) return Math.round(s / 60) + ' minutes ago';
  if (s < 86400) return Math.round(s / 3600) + ' hours ago';
  if (s < 86400 * 2) return 'Yesterday';
  if (s < 86400 * 14) return Math.round(s / 86400) + ' days ago';
  return fmtDate(d);
}
const fmtTime = (secs) => !secs ? '0 minutes' : secs < 3600 ? Math.max(1, Math.round(secs / 60)) + ' minutes' : (secs / 3600).toFixed(1) + ' hours';
const isNew = (g) => g.added && (Date.now() - new Date(g.added)) < 14 * 86400e3;

// ------------------------------------------------------------------------------------------------ art
// Real art when the game has it, otherwise a designed title card in the game's colour.
function artUrl(p) { return p ? S.base + p : ''; }
function art(g, kind, cls = '') {
  const a = g.art || {};
  const fallback = `onerror="this.remove()"`;
  if (kind === 'cover' && a.cover) return `<div class="art ${cls}">${gen(g, '')}<img src="${artUrl(a.cover)}" alt="" ${fallback}></div>`;
  if (kind === 'wide' && (a.hero || a.header)) return `<div class="art ${cls}">${gen(g, '', true)}<img src="${artUrl(a.hero || a.header)}" alt="" ${fallback}></div>`;
  if (kind === 'header' && (a.header || a.hero)) return `<div class="art ${cls}">${gen(g, '', true)}<img src="${artUrl(a.header || a.hero)}" alt="" ${fallback}></div>`;
  if (kind === 'icon' && a.icon) return `<div class="art ${cls}">${gen(g, 'icon')}<img src="${artUrl(a.icon)}" alt="" ${fallback}></div>`;
  return `<div class="art ${cls}">${gen(g, kind)}</div>`;
}
function gen(g, kind, bare) {
  const bg = (g.art || {}).header;
  const img = bg && kind !== 'icon' ? `<img class="bgimg" src="${artUrl(bg)}" alt="" onerror="this.parentNode.classList.remove('has-img');this.remove()"><div class="shade"></div>` : '';
  const title = kind === 'icon' ? esc(g.title.replace(/[^A-Za-z0-9 ]/g, '').split(/\s+/).map((w) => w[0]).join('').slice(0, 2)) : esc(g.title);
  return `<div class="gen ${img ? 'has-img' : ''}" style="--c:${esc(g.color || '#f0b43a')}">${img}${bare ? '' : `<div class="gt">${title}</div>${kind === 'icon' ? '' : '<div class="gs">RUFF DOGS</div>'}`}</div>`;
}
const badges = (g) => `<div class="badges">${isNew(g) ? '<span class="badge new">NEW</span>' : ''}${(g.modes || []).includes('VR') ? '<span class="badge vr">VR</span>' : ''}${g.early ? '<span class="badge early">EARLY ACCESS</span>' : ''}</div>`;

// ------------------------------------------------------------------------------------------------ boot
async function boot() {
  S.config = await fetch('config.json').then((r) => r.json()).catch(() => ({}));
  const hello = new Promise((ok) => { S.helloDone = ok; setTimeout(ok, 2500); });
  Native.on(onNative);
  Native.send({ t: 'hello' });
  await Promise.all([loadCatalog(), hello]);   // the program may name a different account server, so wait for it
  S.session = store.get('rd_session', null) || JSON.parse(sessionStorage.getItem('rd_session') || 'null');
  findServer();
  $('#splash').style.opacity = 0;
  setTimeout(() => $('#splash').remove(), 500);
  if (!S.session) showLogin(); else enter();
}

async function loadCatalog() {
  const repo = S.config.repo, branch = S.config.branch || 'main', ui = S.config.uiPath || 'ui/';
  const tries = [];
  if (Native.kind === 'windows' && repo) {
    tries.push(async () => {
      const sha = (await fetch(`https://api.github.com/repos/${repo}/commits/${branch}`, { headers: { Accept: 'application/vnd.github.sha' } }).then((r) => r.ok ? r.text() : Promise.reject())).trim();
      if (!/^[0-9a-f]{40}$/.test(sha)) throw 0;
      return `https://raw.githubusercontent.com/${repo}/${sha}/${ui}`;
    });
    tries.push(async () => `https://raw.githubusercontent.com/${repo}/${branch}/${ui}`);
  }
  tries.push(async () => new URL('./', location.href).href);
  for (const t of tries) {
    try {
      const base = await t();
      const cat = await fetch(base + 'catalog.json', { cache: 'no-store' }).then((r) => r.ok ? r.json() : Promise.reject());
      setCatalog(base, cat); store.set('rd_catalog', { base, cat }); return;
    } catch {}
  }
  const cached = store.get('rd_catalog', null);
  if (cached) setCatalog(cached.base, cached.cat);
}
function setCatalog(base, cat) {
  S.base = base; S.games = cat.games || []; S.catalogWhen = cat.generated;
  S.byId = Object.fromEntries(S.games.map((g) => [g.id, g]));
}

// ------------------------------------------------------------------------------------------------ the program around us
function onNative(m) {
  if (m.t === 'state') {
    const first = !S.native.app.version;
    Object.assign(S.native, m);
    if (m.accountServer) S.server.override = m.accountServer;
    if (S.helloDone) { S.helloDone(); S.helloDone = null; }
    if (first && S.session) autoUpdates();
    if (m.autoplay) { const id = m.autoplay; S.native.autoplay = null; setTimeout(() => playOrUpdate(id), 300); }
    render();
  } else if (m.t === 'progress') {
    if (S.active && S.active.id !== m.id && m.phase !== 'failed') return;   // a stale message from an earlier job
    if (!S.active && m.phase !== 'failed') return;
    S.active = Object.assign(S.active || {}, m, { heard: Date.now() });
    trackSpeed(m);
    if (m.phase === 'done' || m.phase === 'failed' || m.phase === 'cancelled') finishJob(m);
    else { renderProgress(); }
  } else if (m.t === 'running') {
    S.native.running = m.id; S.runStart = Date.now(); render();
  } else if (m.t === 'stopped') {
    const p = playData(), rec = p[m.id] || { secs: 0 };
    rec.secs += Math.max(0, m.seconds || Math.round((Date.now() - (S.runStart || Date.now())) / 1000));
    rec.last = new Date().toISOString(); p[m.id] = rec; store.set('rd_play_' + user(), p);
    S.native.running = null; render(); reportPlaytime();
  } else if (m.t === 'folder') {
    S.native.library = m.library || S.native.library; render(); if ($('#modal').dataset.kind === 'install') openInstall(S.byId[$('#modal').dataset.id]);
  } else if (m.t === 'opened' && m.url) {
    window.open(m.url, '_blank');
  } else if (m.t === 'ui-updated') {
    toast(null, 'Launcher updated', 'The new version loads next time you open the launcher.');
  } else if (m.t === 'app-status') {
    const was = S.appStatus && S.appStatus.state;
    S.appStatus = m;
    if (m.state === 'updated' && was !== 'updated') toast(null, 'Launcher updated', 'Restart the launcher to use the new version.');
    if (S.appCheckAsked && m.state === 'current') { toast(null, 'You\'re up to date', 'This is the newest Ruff Dogs Launcher.'); S.appCheckAsked = false; }
    if (S.appCheckAsked && m.state === 'failed') { toast(null, 'Couldn\'t check for updates', 'Check your internet connection and try again.'); S.appCheckAsked = false; }
    renderAppBar(); if (S.route.page === 'settings') render();
  } else if (m.t === 'error') {
    toast(null, 'Something went wrong', m.message || '');
  }
}

// ------------------------------------------------------------------------------------------------ downloads
function enqueue(id, kind) {
  const g = S.byId[id]; if (!g) return;
  if (S.active && S.active.id === id) return;
  if (S.queue.some((q) => q.id === id)) return;
  own(id);
  S.queue.push({ id, kind: kind || (installed(id) ? 'update' : 'install'), shortcut: S.settings.shortcut && !installed(id) });
  pump(); render();
}
function pump() {
  if (S.active || !S.queue.length) return;
  const job = S.queue.shift(), g = S.byId[job.id];
  S.active = { id: g.id, kind: job.kind, phase: 'starting', done: 0, total: g.files, bytes: 0, totalBytes: g.size, started: Date.now() };
  S.speed = { t: Date.now(), b: 0, v: 0 };
  Native.send({
    t: 'install', id: g.id, title: g.title, commit: g.commit, shortcut: !!job.shortcut,
    manifest: new URL('manifests/' + g.id + '.json', S.base || location.href).href,
    icon: g.art && g.art.ico ? new URL(g.art.ico, S.base || location.href).href : '',
  });
}
function trackSpeed(m) {
  const now = Date.now(), sp = S.speed || (S.speed = { t: now, b: 0, v: 0 });
  if (now - sp.t > 700) { const v = ((m.bytes || 0) - sp.b) / ((now - sp.t) / 1000); sp.v = sp.v ? sp.v * 0.6 + v * 0.4 : v; sp.t = now; sp.b = m.bytes || 0; }
}
function finishJob(m) {
  const g = S.byId[m.id], kind = S.active && S.active.kind;
  if (m.installed) S.native.installed[m.id] = m.installed;
  S.finished.unshift({ id: m.id, kind, phase: m.phase, error: m.error, when: Date.now() });
  S.active = null;
  if (g && m.phase === 'done' && S.settings.notify) toast(g, kind === 'update' ? 'Updated' : 'Ready to play', kind === 'update' ? (g.news || 'Now on the newest version.') : `${g.title} is installed.`);
  if (g && m.phase === 'failed') toast(g, 'Download failed', m.error || 'Check your internet connection and try again.');
  if (S.afterUpdate === m.id && m.phase === 'done') { S.afterUpdate = null; play(m.id); }
  pump(); render();
}
// a download that goes quiet for two minutes is called off, so it can't hold up everything queued behind it
setInterval(() => {
  const a = S.active;
  if (a && Date.now() - (a.heard || a.started) > 120000) {
    Native.send({ t: 'cancel', id: a.id });
    finishJob({ id: a.id, phase: 'failed', error: 'The download stopped responding. Check your internet connection and try again.' });
  }
}, 10000);
function autoUpdates() {
  if (!S.settings.autoUpdate) return;
  for (const g of S.games) if (needsUpdate(g)) enqueue(g.id, 'update');
}
function cancelJob(id) {
  if (S.active && S.active.id === id) Native.send({ t: 'cancel', id });
  S.queue = S.queue.filter((q) => q.id !== id); render();
}

// ------------------------------------------------------------------------------------------------ play
function play(id) {
  const g = S.byId[id] || { id, title: id };
  if (S.native.running) { toast(S.byId[S.native.running], 'Already playing', 'Close the game that is running first.'); return; }
  own(id);
  Native.send({ t: 'play', id, title: g.title });
  if (Native.kind === 'dev') { S.native.running = null; }
}
function playOrUpdate(id) {
  const g = S.byId[id];
  if (g && needsUpdate(g)) { S.afterUpdate = id; enqueue(id, 'update'); go('library', id); return; }
  if (installed(id)) play(id);
  else go('library', id);
}

// ------------------------------------------------------------------------------------------------ accounts (Ruff Dogs account = the same account OVERDRIVE uses online)
async function findServer() {
  S.server.status = 'checking'; renderServerDot();
  let url = S.server.override || S.config.accountServer || '';
  if (!url && S.config.accountLookup) {
    const repo = S.config.accountLookup;
    for (const u of [`https://api.github.com/repos/${repo}/contents/server.json`, `https://raw.githubusercontent.com/${repo}/main/server.json`]) {
      try {
        const j = await fetch(u, { headers: u.includes('api.github') ? { Accept: 'application/vnd.github.raw' } : {}, cache: 'no-store' }).then((r) => r.ok ? r.json() : Promise.reject());
        if (j.server) { url = j.server; break; }
      } catch {}
    }
  }
  S.server.url = (url || '').replace(/\/+$/, '');
  if (!S.server.url) { S.server.status = 'down'; renderServerDot(); return; }
  try {
    const r = await fetch(S.server.url + '/api/me', { headers: S.session && S.session.token ? { Authorization: 'Bearer ' + S.session.token } : {}, signal: AbortSignal.timeout(7000) });
    S.server.status = 'up';
    if (S.session && S.session.token) {
      if (r.status === 401 && (!S.session.server || S.session.server === S.server.url)) { toast(null, 'Signed out', 'Please sign in to your Ruff Dogs account again.'); signOut(); return; }
      if (r.ok) { const j = await r.json(); S.session.profile = j.profile; S.session.server = S.server.url; saveSession(); render(); }
    }
  } catch { S.server.status = 'down'; }
  renderServerDot(); renderChip();
}
async function api(path, body) {
  if (!S.server.url) throw new Error('The Ruff Dogs servers are asleep right now. You can still play offline.');
  let r;
  try {
    r = await fetch(S.server.url + path, {
      method: body ? 'POST' : 'GET', body: body ? JSON.stringify(body) : undefined,
      headers: Object.assign({ 'Content-Type': 'application/json' }, S.session && S.session.token ? { Authorization: 'Bearer ' + S.session.token } : {}),
      signal: AbortSignal.timeout(10000),
    });
  } catch { throw new Error('Could not reach the Ruff Dogs servers. They run on the studio Mac, so it may be asleep.'); }
  const j = await r.json().catch(() => ({}));
  if (!r.ok) throw new Error(j.error || 'The server said no (' + r.status + ').');
  return j;
}
function saveSession() {
  const s = { name: S.session.name, token: S.session.token, server: S.session.server, profile: S.session.profile, offline: S.session.offline };
  if (S.session.remember === false) sessionStorage.setItem('rd_session', JSON.stringify(s)); else store.set('rd_session', s);
}
function signOut() {
  if (S.session && S.session.token && S.server.url) fetch(S.server.url + '/api/logout', { method: 'POST', body: '{}', headers: { Authorization: 'Bearer ' + S.session.token } }).catch(() => {});
  S.session = null; store.del('rd_session'); sessionStorage.removeItem('rd_session');
  Native.send({ t: 'account', name: '', token: '', server: '' });
  $('#top').hidden = true; $('#view').hidden = true; showLogin();
}
function enter() {
  $('#login').hidden = true; $('#top').hidden = false; $('#view').hidden = false;
  if (S.session && S.session.token) Native.send({ t: 'account', name: S.session.name, token: S.session.token, server: S.session.server || S.server.url, profile: S.session.profile || null });
  if (S.native.app.version) autoUpdates();
  const start = location.hash.slice(1).split('/');
  go(start[0] || (owned().size ? 'library' : 'store'), start[1] || null);
}
function showLogin(tab = 'signin') {
  const L = $('#login'); L.hidden = false;
  const reg = tab === 'register';
  L.innerHTML = `<div class="login-card">
    <img class="wm" src="brand/logo-wide.png" alt="Ruff Dogs"><div class="sub">${reg ? 'Make your Ruff Dogs account. It works in the launcher and online in OVERDRIVE.' : 'Sign in to your Ruff Dogs account'}</div>
    <div class="ltabs"><button class="${reg ? '' : 'on'}" data-tab="signin">SIGN IN</button><button class="${reg ? 'on' : ''}" data-tab="register">CREATE ACCOUNT</button></div>
    <form autocomplete="off">
      <div class="field"><label>Username</label><input name="u" maxlength="16" spellcheck="false" autofocus>${reg ? '<div class="hint">3 to 16 letters, numbers or _. Everyone sees this name, so don\'t use your real name.</div>' : ''}</div>
      <div class="field"><label>Password</label><input name="p" type="password" maxlength="64">${reg ? '<div class="hint">At least 6 characters. Don\'t reuse a password from another site.</div>' : ''}</div>
      ${reg ? '<div class="field"><label>Password again</label><input name="p2" type="password" maxlength="64"></div>' : ''}
      <label class="check"><input type="checkbox" name="r" checked> Keep me signed in on this computer</label>
      <div class="err"></div>
      <button class="btn play" type="submit">${reg ? 'CREATE ACCOUNT' : 'SIGN IN'}</button>
    </form>
    <div class="login-foot"><span class="server-dot"></span><span class="server-text">Looking for the Ruff Dogs servers…</span><br><a data-offline>Play offline instead</a></div>
  </div>`;
  L.querySelectorAll('[data-tab]').forEach((b) => b.onclick = () => showLogin(b.dataset.tab));
  L.querySelector('[data-offline]').onclick = () => { S.session = { name: 'Guest', offline: true, profile: null }; saveSession(); enter(); };
  const f = L.querySelector('form');
  f.onsubmit = async (e) => {
    e.preventDefault();
    const u = f.u.value.trim(), p = f.p.value, err = L.querySelector('.err'), btn = f.querySelector('button');
    err.textContent = '';
    if (!/^[A-Za-z0-9_]{3,16}$/.test(u)) { err.textContent = 'Username: 3 to 16 letters, numbers or _'; return; }
    if (reg && p.length < 6) { err.textContent = 'Password needs at least 6 characters'; return; }
    if (reg && p !== f.p2.value) { err.textContent = "The passwords don't match"; return; }
    btn.disabled = true; btn.textContent = reg ? 'CREATING…' : 'SIGNING IN…';
    try {
      if (!S.server.url) await findServer();
      const j = await api(reg ? '/api/register' : '/api/login', { username: u, password: p });
      S.session = { name: j.profile.name, token: j.token, server: S.server.url, profile: j.profile, remember: f.r.checked };
      saveSession(); S.server.status = 'up';
      enter();
      if (reg) toast(null, `Welcome, ${j.profile.name}!`, 'Your Ruff Dogs account is ready. Grab a game from the store.');
    } catch (x) { err.textContent = x.message; btn.disabled = false; btn.textContent = reg ? 'CREATE ACCOUNT' : 'SIGN IN'; }
  };
  renderServerDot();
}
function renderServerDot() {
  const d = $('.server-dot'), t = $('.server-text');
  if (d) { d.className = 'server-dot ' + (S.server.status === 'up' ? 'up' : S.server.status === 'down' ? 'down' : ''); }
  if (t) t.textContent = S.server.status === 'up' ? 'Ruff Dogs servers are online' : S.server.status === 'down' ? 'Ruff Dogs servers are asleep. Signing in needs them.' : 'Looking for the Ruff Dogs servers…';
}
// Default picture: the first letter of the name on colors picked at random from the name (same name, same colors).
function letterAvatar(name) {
  let h = 2166136261;
  for (const ch of name.toLowerCase()) h = Math.imul(h ^ ch.codePointAt(0), 16777619) >>> 0;
  const hue = h % 360, hue2 = (hue + 40 + (h >> 9) % 80) % 360, ang = (h >> 17) % 360;
  const letter = esc(([...name.trim()][0] || '?').toUpperCase());
  const svg = `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256"><defs><linearGradient id="g" gradientTransform="rotate(${ang} .5 .5)"><stop offset="0" stop-color="hsl(${hue} 70% 52%)"/><stop offset="1" stop-color="hsl(${hue2} 75% 30%)"/></linearGradient></defs><rect width="256" height="256" fill="url(#g)"/><circle cx="${40 + h % 60}" cy="${30 + (h >> 5) % 50}" r="${90 + (h >> 11) % 60}" fill="#fff" opacity=".08"/><text x="128" y="128" dy=".35em" text-anchor="middle" font-family="Bahnschrift, Segoe UI, Arial, sans-serif" font-weight="700" font-size="140" fill="#fff" style="paint-order:stroke" stroke="#0003" stroke-width="6">${letter}</text></svg>`;
  return 'data:image/svg+xml;charset=utf-8,' + encodeURIComponent(svg);
}
const avatarUrl = () => {
  const a = S.session && S.session.profile && S.session.profile.avatar;
  return a ? (/^https?:/.test(a) ? a : (S.session.server || S.server.url) + a) : letterAvatar(S.session && S.session.name || 'Guest');
};

// ------------------------------------------------------------------------------------------------ routing + rendering
function go(page, id = null) {
  if (!['store', 'game', 'library', 'community', 'downloads', 'profile', 'settings'].includes(page)) page = 'store';
  S.route = { page, id };
  const gr = $('#gresults'), gs = $('#gsearch'); if (gr) gr.hidden = true; if (gs) { gs.value = ''; gs.blur(); }
  history.replaceState(null, '', '#' + page + (id ? '/' + id : ''));
  render(true);
}
document.addEventListener('click', (e) => {
  const a = e.target.closest('[data-go]');
  if (a) { e.preventDefault(); go(a.dataset.go, a.dataset.id || null); }
});
function render(scrollTop) {
  if (!S.session || $('#view').hidden) return;
  clearInterval(S.featureTimer);
  document.querySelectorAll('#top nav a').forEach((a) => a.classList.toggle('on', a.dataset.go === S.route.page || (a.dataset.go === 'store' && S.route.page === 'game')));
  renderChip(); renderProgress();
  const v = $('#view'), keep = v.querySelector('.lib-list') ? v.querySelector('.lib-list').scrollTop : 0;
  if (S.route.page === 'community' && !S.comm.loaded && !S.comm.loading) loadCommunity();
  const pages = { store: pageStore, game: pageGame, library: pageLibrary, community: pageCommunity, downloads: pageDownloads, profile: pageProfile, settings: pageSettings };
  v.innerHTML = pages[S.route.page]();
  if (scrollTop) v.scrollTop = 0;
  if (v.querySelector('.lib-list')) v.querySelector('.lib-list').scrollTop = keep;
  wire(v);
}
function renderChip() {
  const c = $('#me-chip'); if (!c || !S.session) return;
  c.querySelector('img').src = avatarUrl();
  c.querySelector('span').textContent = S.session.name;
  c.classList.toggle('offline', !S.session.token || S.server.status !== 'up');
  $('#nav-name').textContent = S.session.name.toUpperCase();
}
function renderProgress() {
  const n = (S.active ? 1 : 0) + S.queue.length, b = $('#dl-badge'), mini = $('#dl-mini');
  if (b) { b.hidden = !n; b.textContent = n; }
  if (mini) {
    mini.hidden = !S.active;
    if (S.active) {
      const g = S.byId[S.active.id] || { title: S.active.id }, pct = pctOf(S.active);
      mini.innerHTML = `<div>${S.active.kind === 'update' ? 'Updating' : 'Installing'} ${esc(g.title)} · ${pct}%</div><div class="bar"><i style="width:${pct}%"></i></div>`;
      mini.onclick = () => go('downloads');
    }
  }
  document.querySelectorAll('[data-prog]').forEach((el) => { el.outerHTML = progressBlock(); });
}
const pctOf = (a) => a.totalBytes ? Math.min(100, Math.floor(100 * (a.bytes || 0) / a.totalBytes)) : a.total ? Math.floor(100 * (a.done || 0) / a.total) : 0;
function progressBlock() {
  const a = S.active; if (!a) return '';
  const pct = pctOf(a), sp = S.speed && S.speed.v > 0 ? fmtSize(S.speed.v) + '/s' : '';
  const label = { starting: 'Starting…', manifest: 'Reading file list…', downloading: `${fmtSize(a.bytes || 0)} of ${fmtSize(a.totalBytes || 0)} · ${a.done || 0} of ${a.total || 0} files`, finishing: 'Finishing up…' }[a.phase] || a.phase;
  return `<div class="prog" data-prog><div class="line"><span>${label}</span><span>${sp ? sp + ' · ' : ''}${pct}%</span></div><div class="bar"><i style="width:${pct}%"></i></div></div>`;
}

// the one big button every game has
function actionButton(g, big) {
  const cls = big ? ' big' : '';
  if (S.active && S.active.id === g.id) return `<button class="btn busy${cls}" data-act="downloads">${S.active.kind === 'update' ? 'UPDATING' : 'INSTALLING'} ${pctOf(S.active)}%</button>`;
  if (S.queue.some((q) => q.id === g.id)) return `<button class="btn busy${cls}" data-act="downloads">QUEUED</button>`;
  if (S.native.running === g.id) return `<button class="btn stop${cls}" data-act="stop" data-id="${g.id}">${ICON.stop} STOP</button>`;
  if (needsUpdate(g)) return `<button class="btn update${cls}" data-act="update" data-id="${g.id}">${ICON.down} UPDATE</button>`;
  if (installed(g.id)) return `<button class="btn play${cls}" data-act="play" data-id="${g.id}">${ICON.play} PLAY</button>`;
  return `<button class="btn install${cls}" data-act="install" data-id="${g.id}">${ICON.down} INSTALL</button>`;
}
function wire(root) {
  root.querySelectorAll('[data-act]').forEach((b) => b.onclick = (e) => {
    e.stopPropagation();
    const id = b.dataset.id, act = b.dataset.act;
    if (act === 'install') openInstall(S.byId[id]);
    else if (act === 'update') enqueue(id, 'update');
    else if (act === 'play') play(id);
    else if (act === 'stop') Native.send({ t: 'stop', id });
    else if (act === 'downloads') go('downloads');
    else if (act === 'cancel') cancelJob(id);
    else if (act === 'manage') openManage(S.byId[id]);
    else if (act === 'feature') showFeature(+b.dataset.i);
    else if (act === 'allnotes') { S.allNotes = id; render(); }
    else if (act === 'appcheck') { S.appCheckAsked = true; Native.send({ t: 'appcheck' }); }
    else if (act === 'restart') Native.send({ t: 'restart' });
    else if (act === 'commgame') { S.comm.game = b.dataset.v || null; S.comm.pick = null; S.comm.shot = null; render(true); }
    else if (act === 'commshot') { S.comm.shot = b.dataset.v || null; render(); }
    else if (act === 'commup') { S.comm.pick.up = b.dataset.v === '1'; render(); }
    else if (act === 'committag') { const t = b.dataset.v, p = S.comm.pick; p.tags = p.tags.includes(t) ? p.tags.filter((x) => x !== t) : p.tags.length < 3 ? [...p.tags, t] : p.tags; render(); }
    else if (act === 'commsave') {
      const p = S.comm.pick;
      api('/api/community/rate', { game: p.id, up: p.up, tags: p.tags }).then((j) => { S.comm.data = j; S.comm.pick = null; toast(S.byId[p.id], 'Thanks for rating!', 'Your rating helps other players pick a game.'); render(); })
        .catch((e) => toast(null, 'Couldn\'t save your rating', e.message));
    }
    else if (act === 'filter') { S.storeFilter = b.dataset.v; render(); }
    else if (act === 'libfilter') { S.libFilter = b.dataset.v; render(); }
    else if (act === 'folder') Native.send({ t: 'folder' });
    else if (act === 'setting') { S.settings[b.dataset.k] = !S.settings[b.dataset.k]; store.set('rd_settings', S.settings); render(); }
    else if (act === 'signout') signOut();
    else if (act === 'signin') { S.session = null; store.del('rd_session'); $('#top').hidden = true; $('#view').hidden = true; showLogin(); }
    else if (act === 'avatar') pickAvatar();
    else if (act === 'recheck') { loadCatalog().then(() => { autoUpdates(); render(); toast(null, 'Store refreshed', `${S.games.length} games.`); }); }
    else if (act === 'copy') { navigator.clipboard && navigator.clipboard.writeText(b.dataset.v); toast(null, 'Copied', b.dataset.v); }
  });
  root.querySelectorAll('[data-game]').forEach((c) => c.onclick = () => go(c.dataset.page || 'game', c.dataset.game));
  const ss = root.querySelector('#store-search');
  if (ss) ss.oninput = () => { S.storeSearch = ss.value; const pos = ss.selectionStart; render(); const n = $('#store-search'); n.focus(); n.setSelectionRange(pos, pos); };
  const ls = root.querySelector('#lib-search');
  if (ls) ls.oninput = () => { S.libSearch = ls.value; const pos = ls.selectionStart; render(); const n = $('#lib-search'); n.focus(); n.setSelectionRange(pos, pos); };
  if (root.querySelector('.feature')) { S.featureTimer = setInterval(() => showFeature((S.featureI + 1) % S.featureN), 7000); }
}

// ------------------------------------------------------------------------------------------------ store
function pageStore() {
  if (!S.games.length) return `<div class="empty"><img src="brand/dog.png" alt=""><div>Couldn't load the store. Check your internet connection.</div><button class="btn ghost small" data-act="recheck">TRY AGAIN</button></div>`;
  const feat = [...S.games].sort((a, b) => new Date(b.updated) - new Date(a.updated)).slice(0, 5);
  S.featureN = feat.length; S.featureI = S.featureI % S.featureN || 0;
  const genres = ['All', 'PC', 'VR', ...new Set(S.games.flatMap((g) => g.genres || []))];
  const q = S.storeSearch.trim().toLowerCase();
  const list = S.games.filter((g) => (S.storeFilter === 'All' || (g.modes || []).includes(S.storeFilter) || (g.genres || []).includes(S.storeFilter)) && (!q || (g.title + ' ' + g.tagline + ' ' + (g.genres || []).join(' ')).toLowerCase().includes(q)));
  const own = owned();
  return `<div class="page">
    <div class="feature">
      ${feat.map((g, i) => { const shots = ['shot1', 'shot2', 'shot3', 'header'].map((k) => (g.art || {})[k]).filter(Boolean).slice(0, 4); return `<div class="slide" data-slide="${i}" style="opacity:${i === S.featureI ? 1 : 0};pointer-events:${i === S.featureI ? 'auto' : 'none'}">
        <div class="f-main" data-game="${g.id}">${art(g, 'wide')}</div>
        <div class="f-side" data-game="${g.id}">
          ${g.art && g.art.logo ? `<img class="logo" src="${artUrl(g.art.logo)}" alt="${esc(g.title)}">` : `<h2>${esc(g.title)}</h2>`}
          <p>${esc(g.tagline)}</p>
          ${shots.length ? `<div class="f-shots">${shots.map((u) => `<img src="${artUrl(u)}" alt="">`).join('')}</div>` : ''}
          <div class="f-tags">${(g.genres || []).slice(0, 4).map((t) => `<span>${esc(t)}</span>`).join('')}${(g.modes || []).includes('VR') ? '<span>VR</span>' : ''}</div>
          <div class="f-buy"><span class="f-when">${i === 0 ? 'Updated ' + fmtAgo(g.updated).toLowerCase() : 'Free to play'}</span>${actionButton(g)}</div>
        </div></div>`; }).join('')}
      <button class="f-arrow l" data-act="feature" data-i="${(S.featureI + S.featureN - 1) % S.featureN}" aria-label="Previous">&#8249;</button>
      <button class="f-arrow r" data-act="feature" data-i="${(S.featureI + 1) % S.featureN}" aria-label="Next">&#8250;</button>
    </div>
    <div class="dots">${feat.map((g, i) => `<i class="${i === S.featureI ? 'on' : ''}" data-act="feature" data-i="${i}"></i>`).join('')}</div>
    <div class="section-title"><span>Recently updated</span></div>
    <div class="rows">${[...S.games].sort((a, b) => new Date(b.updated) - new Date(a.updated)).slice(0, 6).map((g) => `
      <div class="row-card" data-game="${g.id}">${art(g, 'header')}<div><div class="t">${esc(g.title)}</div><div class="d">${fmtAgo(g.updated)} · ${esc(g.news)}</div></div></div>`).join('')}</div>
    <div class="section-title"><span>All games · ${S.games.length}</span><span class="aside">Every game is free</span></div>
    <div class="filters" style="margin-bottom:16px">${genres.map((x) => `<span class="chip ${S.storeFilter === x ? 'on' : ''}" data-act="filter" data-v="${esc(x)}">${esc(x)}</span>`).join('')}
      <input class="search" id="store-search" placeholder="Search the store" value="${esc(S.storeSearch)}"></div>
    <div class="grid">${list.map((g) => `
      <div class="cap" data-game="${g.id}"><div style="position:relative">${badges(g)}${art(g, 'cover')}</div>
        <div class="meta"><div class="name">${esc(g.title)}</div><div class="sub"><span>${esc((g.genres || [])[0] || '')}</span>${installed(g.id) ? '<span class="owned">INSTALLED</span>' : own.has(g.id) ? '<span class="owned">IN LIBRARY</span>' : '<span class="free">FREE</span>'}</div></div></div>`).join('') || '<div style="color:var(--dim)">No games match that.</div>'}</div>
  </div>`;
}
function showFeature(i) {
  S.featureI = i;
  document.querySelectorAll('[data-slide]').forEach((s) => { s.style.opacity = +s.dataset.slide === i ? 1 : 0; s.style.pointerEvents = +s.dataset.slide === i ? 'auto' : 'none'; });
  document.querySelectorAll('.dots i').forEach((d, k) => d.classList.toggle('on', k === i));
  const n = S.featureN, l = $('.f-arrow.l'), r = $('.f-arrow.r');
  if (l) l.dataset.i = (i + n - 1) % n; if (r) r.dataset.i = (i + 1) % n;
}

function detailsBox(g) {
  const p = playData()[g.id], i = installed(g.id);
  return `<div class="side">
    <div class="kv"><span>Price</span><span class="free">FREE</span></div>
    <div class="kv"><span>Developer</span><span>Ruff Dogs Studios</span></div>
    <div class="kv"><span>Genre</span><span>${esc((g.genres || []).join(', '))}</span></div>
    <div class="kv"><span>Plays on</span><span>${(g.modes || []).map((m) => m === 'VR' ? 'Meta Quest (VR)' : 'PC').join(', ')}</span></div>
    <div class="kv"><span>In the store since</span><span>${fmtDate(g.added)}</span></div>
    <div class="kv"><span>Last update</span><span>${fmtDate(g.updated)}</span></div>
    <div class="kv"><span>Download size</span><span>${fmtSize(g.size)}</span></div>
    ${i ? `<div class="kv"><span>Installed in</span><span style="word-break:break-all">${esc(i.dir || '')}</span></div>` : ''}
    ${p ? `<div class="kv"><span>You've played</span><span>${fmtTime(p.secs)}</span></div>` : ''}
    <div class="rating"><b>${esc((g.rating || 'Everyone').split(' ')[0][0])}</b><span>${esc(g.rating || 'Everyone')}</span></div>
  </div>`;
}
function notesBlock(g, limit, ready) {
  const notes = g.notes && g.notes.length ? g.notes : [{ date: g.updated, title: g.news, items: [] }];
  const one = (n, i) => `<div class="news ${i === 0 && ready ? 'ready' : ''}">
    <div class="when">${i === 0 ? (ready ? 'Update ready · ' : 'Latest · ') : ''}${fmtDate(n.date)}</div>
    <div class="what">${esc(n.title === 'Update' ? 'Fixes and changes' : n.title)}</div>
    ${n.items && n.items.length ? `<ul>${n.items.map((t) => `<li>${esc(t)}</li>`).join('')}</ul>` : ''}</div>`;
  const more = notes.length > limit ? `<a class="news-more" data-act="allnotes" data-id="${g.id}">Show older updates (${notes.length - limit})</a>`
    : limit > 2 && notes.length > 1 ? `<a class="news-more" data-act="allnotes" data-id="">Show less</a>` : '';
  return notes.slice(0, limit).map(one).join('') + more;
}
function vrBox(g) {
  if (!(g.modes || []).includes('VR')) return '';
  return `<div class="vr-box"><b>Play in VR on your Quest:</b> open this in the Quest browser and press Enter VR. On PC it plays with mouse and keyboard.<code>${esc(g.web)}</code>
    <button class="btn ghost small" style="margin-top:8px" data-act="copy" data-v="${esc(g.web)}">COPY LINK</button></div>`;
}
function pageGame() {
  const g = S.byId[S.route.id];
  if (!g) return `<div class="empty">That game isn't in the store any more. <a data-go="store">Back to the store</a></div>`;
  return `<div class="gp-hero">${art(g, 'wide')}</div>
    <div class="gp-head">${art(g, 'cover')}<div style="flex:1;min-width:0">
      <h1>${esc(g.title)}</h1><div class="tag">${esc(g.tagline)}</div>
      <div class="gp-actions">${actionButton(g, true)}${installed(g.id) || owned().has(g.id) ? `<button class="btn ghost" data-go="library" data-id="${g.id}">IN YOUR LIBRARY</button>` : ''}<span class="free" style="margin-left:6px">FREE</span></div>
    </div></div>
    <div class="gp-body"><div>
      ${S.active && S.active.id === g.id ? progressBlock() + '<div style="height:18px"></div>' : ''}
      <div class="section-title" style="margin-top:0"><span>What's new</span></div>
      ${notesBlock(g, S.allNotes === g.id ? 6 : 2)}
      <div class="section-title"><span>About this game</span></div>
      <p>${esc(g.about)}</p>
      ${g.early ? '<p style="color:var(--gold2)">Early access: this game is still being made and will change a lot.</p>' : ''}
      ${vrBox(g)}
    </div>${detailsBox(g)}</div>`;
}

// ------------------------------------------------------------------------------------------------ library
function pageLibrary() {
  const own = owned();
  for (const id of Object.keys(S.native.installed || {})) own.add(id);
  const q = S.libSearch.trim().toLowerCase();
  const games = S.games.filter((g) => own.has(g.id) && (!q || g.title.toLowerCase().includes(q)) && (S.libFilter !== 'installed' || installed(g.id)) && (S.libFilter !== 'vr' || (g.modes || []).includes('VR')));
  if (!own.size) return `<div class="empty"><img src="brand/dog.png" alt=""><div style="font:700 24px var(--display);letter-spacing:.08em;color:var(--text)">YOUR LIBRARY IS EMPTY</div><div>Every Ruff Dogs game is free. Grab some from the store.</div><button class="btn play" data-go="store">GO TO THE STORE</button></div>`;
  const sel = S.byId[S.route.id] && own.has(S.route.id) ? S.byId[S.route.id] : games[0] || S.games.find((g) => own.has(g.id));
  if (sel && S.route.id !== sel.id) { S.route.id = sel.id; history.replaceState(null, '', '#library/' + sel.id); }
  const item = (g) => {
    const st = S.native.running === g.id ? '<span class="st run">PLAYING</span>' : S.active && S.active.id === g.id ? `<span class="st dl">${pctOf(S.active)}%</span>` : needsUpdate(g) ? '<span class="st upd">UPDATE</span>' : '';
    return `<div class="lib-item ${installed(g.id) ? 'inst' : ''} ${sel && sel.id === g.id ? 'on' : ''}" data-game="${g.id}" data-page="library">${art(g, 'icon', 'ic')}<span class="nm">${esc(g.title)}</span>${st}</div>`;
  };
  const inst = games.filter((g) => installed(g.id)), not = games.filter((g) => !installed(g.id));
  const side = `<aside class="lib-side"><div class="head"><input class="search" id="lib-search" placeholder="Search your games" value="${esc(S.libSearch)}">
      <div class="tabs">${[['all', 'All'], ['installed', 'Installed'], ['vr', 'VR']].map(([k, l]) => `<span class="chip ${S.libFilter === k ? 'on' : ''}" data-act="libfilter" data-v="${k}">${l}</span>`).join('')}</div></div>
    <div class="lib-list">${inst.length ? `<div class="lib-group">Installed · ${inst.length}</div>${inst.map(item).join('')}` : ''}${not.length ? `<div class="lib-group">Not installed · ${not.length}</div>${not.map(item).join('')}` : ''}</div></aside>`;
  if (!sel) return `<div class="lib">${side}<div class="lib-main"></div></div>`;
  const g = sel, p = playData()[g.id] || {}, i = installed(g.id);
  return `<div class="lib">${side}<div class="lib-main">
    <div class="gp-hero">${art(g, 'wide')}<div class="lib-title">${g.art && g.art.logo ? `<img class="logo" src="${artUrl(g.art.logo)}" alt="${esc(g.title)}">` : `<h1>${esc(g.title)}</h1>`}</div></div>
    <div class="lib-bar">${actionButton(g, true)}
      ${S.active && S.active.id === g.id ? `<div style="flex:1">${progressBlock()}</div>` : `<div class="stats">
        <div class="stat"><div class="k">Last played</div><div class="v">${S.native.running === g.id ? 'Playing now' : fmtAgo(p.last)}</div></div>
        <div class="stat"><div class="k">Play time</div><div class="v">${fmtTime(p.secs)}</div></div>
        <div class="stat"><div class="k">${i ? 'Size on disk' : 'Download'}</div><div class="v">${fmtSize(i && i.size || g.size)}</div></div></div>`}
      <div class="more"><button class="btn ghost small" data-go="game" data-id="${g.id}">STORE PAGE</button>${i ? `<button class="icon-btn" data-act="manage" data-id="${g.id}" title="Manage">${ICON.dots}</button>` : ''}</div>
    </div>
    <div class="lib-content"><div>
      <div class="section-title" style="margin-top:0"><span>What's new</span></div>
      ${notesBlock(g, S.allNotes === g.id ? 6 : 1, needsUpdate(g))}
      <div class="section-title"><span>About</span></div><p style="color:#cfcbc3;line-height:1.65">${esc(g.about)}</p>${vrBox(g)}
    </div>${detailsBox(g)}</div>
  </div></div>`;
}

// ------------------------------------------------------------------------------------------------ community
// News from every game, player ratings with tags, most played and screenshots. On purpose there is no chat, no comments
// and no messages: players can only pick thumbs and tags from a fixed list, so nobody can write anything to anyone.
S.comm = { game: null, loaded: false, loading: false, data: null, error: '', pick: null, shot: null };
async function loadCommunity() {
  S.comm.loading = true;
  try { S.comm.data = await api('/api/community'); S.comm.error = ''; }
  catch (e) { S.comm.error = e.message; }
  S.comm.loading = false; S.comm.loaded = true;
  reportPlaytime();
  if (S.route.page === 'community') render();
}
// sends each game's total play time once it has grown (the server keeps the biggest number it has seen)
async function reportPlaytime() {
  if (!S.session || !S.session.token || !S.server.url) return;
  const plays = playData(), sent = store.get('rd_playsent_' + user(), {});
  for (const [id, p] of Object.entries(plays)) {
    const secs = Math.round(p.secs || 0);
    if (secs > 60 && secs > (sent[id] || 0)) {
      try { await api('/api/community/playtime', { game: id, secs }); sent[id] = secs; } catch { return; }
    }
  }
  store.set('rd_playsent_' + user(), sent);
}
function verdict(c) {
  const n = (c.up || 0) + (c.down || 0);
  if (!n) return { label: 'Not rated yet', cls: 'none', pct: 0, n };
  const pct = Math.round(100 * c.up / n);
  return { label: pct >= 80 ? 'Players love it' : pct >= 60 ? 'Mostly positive' : pct >= 40 ? 'Mixed' : 'Mostly negative', cls: pct >= 60 ? 'good' : pct >= 40 ? 'mixed' : 'bad', pct, n };
}
const commOf = (id) => (S.comm.data && S.comm.data.games[id]) || { up: 0, down: 0, tags: {}, players: 0, hours: 0 };
const THUMB = (down) => `<svg viewBox="0 0 24 24" width="20" height="20" style="${down ? 'transform:scaleY(-1)' : ''}"><path fill="currentColor" d="M2 21h3V9H2v12Zm20-10.5c0-1.1-.9-2-2-2h-6.3l1-4.6.03-.32c0-.41-.17-.79-.44-1.06L13.2 1.5 6.6 8.1c-.37.36-.6.86-.6 1.4v9.5c0 1.1.9 2 2 2h9c.83 0 1.54-.5 1.84-1.22l3.02-7.05c.09-.23.14-.47.14-.73v-1.5Z"/></svg>`;
function newsCards(list, showGame) {
  if (!list.length) return '<div class="comm-empty">No news yet.</div>';
  return list.map(({ g, n }) => `<div class="news-card" data-game="${g.id}">
    ${showGame ? art(g, 'header', 'thumb') : ''}
    <div class="body"><div class="meta">${showGame ? `<b>${esc(g.title)}</b> · ` : ''}${fmtDate(n.date)}</div>
      <div class="t">${esc(n.title === 'Update' ? 'Fixes and changes' : n.title)}</div>
      ${n.items && n.items.length ? `<ul>${n.items.slice(0, showGame ? 3 : 8).map((t) => `<li>${esc(t)}</li>`).join('')}</ul>` : ''}</div></div>`).join('');
}
function ratingBar(c) {
  const v = verdict(c);
  return `<div class="rbar ${v.cls}"><i style="width:${v.n ? v.pct : 0}%"></i></div><div class="rlabel ${v.cls}">${v.label}${v.n ? ` · ${v.pct}% of ${v.n} ${v.n === 1 ? 'player' : 'players'}` : ''}</div>`;
}
function pageCommunity() {
  const games = S.games, sel = S.comm.game && S.byId[S.comm.game];
  const chips = `<div class="comm-chips"><span class="chip ${!sel ? 'on' : ''}" data-act="commgame" data-v="">All games</span>${games.map((g) => `<span class="chip ic ${sel && sel.id === g.id ? 'on' : ''}" data-act="commgame" data-v="${g.id}">${art(g, 'icon', 'mini')}${esc(g.title)}</span>`).join('')}</div>`;
  const status = S.comm.loading && !S.comm.data ? '<div class="comm-note">Loading ratings…</div>'
    : S.comm.error ? `<div class="comm-note">${esc(S.comm.error)} News and screenshots still work.</div>` : '';
  const safety = '<div class="comm-safe">There\'s no chat or comments here, just ratings and tags, so the community stays friendly for everyone.</div>';
  if (!sel) {
    const news = games.flatMap((g) => (g.notes || []).map((n) => ({ g, n }))).sort((a, b) => String(b.n.date).localeCompare(String(a.n.date))).slice(0, 14);
    const rated = [...games].sort((a, b) => { const va = verdict(commOf(a.id)), vb = verdict(commOf(b.id)); return (vb.n ? vb.pct + vb.n / 1000 : -1) - (va.n ? va.pct + va.n / 1000 : -1); });
    const played = [...games].sort((a, b) => commOf(b.id).hours - commOf(a.id).hours || commOf(b.id).players - commOf(a.id).players);
    return `<div class="comm"><h1 class="page-title">Community</h1><div class="comm-sub">News, ratings and screenshots from every Ruff Dogs game.</div>${chips}${status}
      <div class="comm-grid"><div><div class="section-title" style="margin-top:0"><span>Latest news</span></div>${newsCards(news, true)}</div>
      <div><div class="section-title" style="margin-top:0"><span>Player ratings</span></div>
        <div class="comm-box">${rated.map((g) => `<div class="rrow" data-act="commgame" data-v="${g.id}">${art(g, 'icon', 'ic')}<div class="rr"><div class="nm">${esc(g.title)}</div>${ratingBar(commOf(g.id))}</div></div>`).join('')}</div>
        <div class="section-title"><span>Most played</span></div>
        <div class="comm-box">${played.slice(0, 6).map((g, i) => { const c = commOf(g.id); return `<div class="prow" data-act="commgame" data-v="${g.id}"><span class="rank">${i + 1}</span>${art(g, 'icon', 'ic')}<div class="nm">${esc(g.title)}</div><div class="hrs">${c.hours ? c.hours.toLocaleString() + ' h' : '–'}<small>${c.players} ${c.players === 1 ? 'player' : 'players'}</small></div></div>`; }).join('')}</div>
        ${safety}</div></div></div>`;
  }
  const g = sel, c = commOf(g.id), v = verdict(c), tags = (S.comm.data && S.comm.data.tags) || [];
  const mine = (S.comm.data && S.comm.data.mine && S.comm.data.mine[g.id]) || { up: null, tags: [] };
  const pick = S.comm.pick && S.comm.pick.id === g.id ? S.comm.pick : { id: g.id, up: mine.up, tags: [...(mine.tags || [])] };
  S.comm.pick = pick;
  const canRate = S.session && S.session.token && S.comm.data && (installed(g.id) || (playData()[g.id] || {}).secs > 0 || owned().has(g.id));
  const topTags = Object.entries(c.tags || {}).sort((a, b) => b[1] - a[1]).slice(0, 6);
  const shots = Object.keys(g.art || {}).filter((k) => /^shot\d$/.test(k)).sort().map((k) => g.art[k]).concat((g.art || {}).hero ? [g.art.hero] : []);
  const yours = !S.session || !S.session.token ? '<div class="comm-note">Sign in to rate games.</div>'
    : !S.comm.data ? '' : !canRate ? '<div class="comm-note">Add this game to your library to rate it.</div>'
    : `<div class="yours"><div class="k">Your rating</div>
        <div class="thumbs"><button class="thumb ${pick.up === true ? 'on up' : ''}" data-act="commup" data-v="1">${THUMB(false)} Thumbs up</button><button class="thumb ${pick.up === false ? 'on down' : ''}" data-act="commup" data-v="0">${THUMB(true)} Thumbs down</button></div>
        <div class="k" style="margin-top:12px">Pick up to 3 tags</div>
        <div class="tagpick">${tags.map((t) => `<span class="chip ${pick.tags.includes(t) ? 'on' : ''}" data-act="committag" data-v="${esc(t)}">${esc(t)}</span>`).join('')}</div>
        <button class="btn play small" style="margin-top:14px" data-act="commsave" ${pick.up === null || pick.up === undefined ? 'disabled' : ''}>SAVE RATING</button></div>`;
  return `<div class="comm"><h1 class="page-title">Community</h1>${chips}${status}
    <div class="gp-hero comm-hero">${art(g, 'wide')}<div class="lib-title">${g.art && g.art.logo ? `<img class="logo" src="${artUrl(g.art.logo)}" alt="${esc(g.title)}">` : `<h1>${esc(g.title)}</h1>`}</div></div>
    <div class="comm-grid"><div>
      ${shots.length ? `<div class="section-title" style="margin-top:0"><span>Screenshots</span></div><div class="shots">${shots.map((u, i) => `<div class="shot" data-act="commshot" data-v="${esc(u)}"><img src="${artUrl(u)}" alt="Screenshot ${i + 1}"></div>`).join('')}</div>` : ''}
      <div class="section-title"><span>News</span></div>${newsCards((g.notes || []).map((n) => ({ g, n })), false)}
    </div><div>
      <div class="section-title" style="margin-top:0"><span>Player ratings</span></div>
      <div class="comm-box"><div class="big ${v.cls}">${v.n ? v.pct + '%' : '–'}</div>${ratingBar(c)}
        <div class="ud"><span>${THUMB(false)} ${c.up || 0}</span><span>${THUMB(true)} ${c.down || 0}</span></div>
        ${topTags.length ? `<div class="k">Players say it's</div><div class="tagpick">${topTags.map(([t, n]) => `<span class="chip static">${esc(t)} <b>${n}</b></span>`).join('')}</div>` : ''}
        <div class="stats2"><div><b>${c.players}</b><span>${c.players === 1 ? 'player' : 'players'}</span></div><div><b>${c.hours.toLocaleString()}</b><span>hours played</span></div></div>
        ${yours}</div>
      <button class="btn ghost small" style="margin-top:14px;width:100%" data-go="game" data-id="${g.id}">STORE PAGE</button>
      ${safety}</div></div>
    ${S.comm.shot ? `<div class="lightbox" data-act="commshot" data-v=""><img src="${artUrl(S.comm.shot)}" alt=""></div>` : ''}</div>`;
}

// ------------------------------------------------------------------------------------------------ downloads
function pageDownloads() {
  const a = S.active, ga = a && S.byId[a.id];
  return `<div class="page"><h1 class="page-title">Downloads</h1>
    ${ga ? `<div class="section-title" style="margin-top:0"><span>${a.kind === 'update' ? 'Updating now' : 'Installing now'}</span></div>
      <div class="dl-card">${art(ga, 'header')}<div><div class="t">${esc(ga.title)}</div><div class="s">${a.done || 0} of ${a.total || ga.files} files</div>${progressBlock()}</div>
      <button class="btn ghost small" data-act="cancel" data-id="${ga.id}">CANCEL</button></div>` : '<div class="dl-card" style="grid-template-columns:1fr;color:var(--dim)">Nothing is downloading. Updates install by themselves when the launcher starts.</div>'}
    ${S.queue.length ? `<div class="section-title"><span>Up next · ${S.queue.length}</span></div>${S.queue.map((q) => { const g = S.byId[q.id]; return `<div class="dl-card">${art(g, 'header')}<div><div class="t">${esc(g.title)}</div><div class="s">${q.kind === 'update' ? 'Update' : 'Install'} · ${fmtSize(g.size)}</div></div><button class="btn ghost small" data-act="cancel" data-id="${g.id}">REMOVE</button></div>`; }).join('')}` : ''}
    ${S.finished.length ? `<div class="section-title"><span>Finished</span></div>${S.finished.slice(0, 12).map((f) => { const g = S.byId[f.id] || { title: f.id, id: f.id }; return `<div class="dl-card done">${art(g, 'header')}<div><div class="t">${esc(g.title)}</div><div class="s">${f.phase === 'done' ? (f.kind === 'update' ? 'Updated' : 'Installed') : f.phase === 'cancelled' ? 'Cancelled' : 'Failed: ' + esc(f.error || '')} · ${fmtAgo(f.when)}</div></div>${f.phase === 'done' && installed(f.id) ? actionButton(g) : ''}</div>`; }).join('')}` : ''}
  </div>`;
}

// ------------------------------------------------------------------------------------------------ profile
function pageProfile() {
  const s = S.session, pr = s.profile || {}, plays = playData(), own = owned();
  const total = Object.values(plays).reduce((t, p) => t + (p.secs || 0), 0);
  const recent = Object.entries(plays).filter(([id]) => S.byId[id]).sort((a, b) => new Date(b[1].last) - new Date(a[1].last)).slice(0, 6);
  const title = pr.title || (s.offline ? 'OFFLINE' : 'PLAYER');
  return `<div class="page"><div class="prof-head">
      <div class="avatar-big"><img src="${avatarUrl()}" alt="">${s.token ? '<button data-act="avatar">CHANGE</button>' : ''}</div>
      <div style="flex:1"><h1>${esc(s.name)}</h1><span class="title-tag ${pr.role === 'dev' ? 'dev' : ''}">${esc(title)}</span>
        <div style="color:var(--dim);font-size:13px;margin-top:8px">${s.token ? `Ruff Dogs account${pr.joined ? ' · joined ' + fmtDate(pr.joined) : ''} · ${S.server.status === 'up' ? '<span style="color:var(--green)">Online</span>' : 'Offline'}` : 'Playing offline. Your games and play time stay on this computer.'}</div>
        <div class="prof-stats"><div><div class="v">${own.size}</div><div class="k">Games</div></div><div><div class="v">${Object.keys(S.native.installed || {}).length}</div><div class="k">Installed</div></div><div><div class="v">${(total / 3600).toFixed(1)}</div><div class="k">Hours played</div></div></div>
      </div>
      <div style="display:grid;gap:8px">${s.token ? '<button class="btn ghost small" data-act="signout">SIGN OUT</button>' : '<button class="btn play small" data-act="signin">SIGN IN</button>'}</div>
    </div>
    <div class="section-title"><span>Recently played</span></div>
    <div class="side" style="padding:0 6px">${recent.length ? recent.map(([id, p]) => { const g = S.byId[id]; return `<div class="recent" data-game="${id}" data-page="library" style="cursor:pointer">${art(g, 'header')}<div style="flex:1"><div class="t">${esc(g.title)}</div><div class="d">${fmtTime(p.secs)} played · last played ${fmtAgo(p.last).toLowerCase()}</div></div>${installed(id) ? actionButton(g) : ''}</div>`; }).join('') : '<div style="padding:18px;color:var(--dim)">Nothing yet. Go play something!</div>'}</div>
  </div>`;
}
function pickAvatar() {
  const inp = document.createElement('input'); inp.type = 'file'; inp.accept = 'image/png,image/jpeg,image/webp';
  inp.onchange = () => {
    const f = inp.files[0]; if (!f) return;
    const img = new Image();
    img.onload = async () => {
      const c = document.createElement('canvas'); c.width = c.height = 256;
      const s = Math.min(img.width, img.height);
      c.getContext('2d').drawImage(img, (img.width - s) / 2, (img.height - s) / 2, s, s, 0, 0, 256, 256);
      try {
        const j = await api('/api/avatar', { image: c.toDataURL('image/jpeg', 0.88) });
        S.session.profile = Object.assign({}, S.session.profile, { avatar: j.avatar }); saveSession(); render();
        toast(null, 'Picture updated', 'Everyone sees it in OVERDRIVE too.');
      } catch (x) { toast(null, "Couldn't change your picture", x.message); }
      URL.revokeObjectURL(img.src);
    };
    img.src = URL.createObjectURL(f);
  };
  inp.click();
}

// ------------------------------------------------------------------------------------------------ settings
function pageSettings() {
  const L = S.native.library || {}, a = S.native.app || {};
  const used = L.total ? Math.round(100 * (L.total - L.free) / L.total) : 0;
  const tog = (k, label, d) => `<div class="set"><div class="l"><div>${label}</div><div class="d">${d}</div></div><div class="toggle ${S.settings[k] ? 'on' : ''}" data-act="setting" data-k="${k}"></div></div>`;
  return `<div class="page" style="max-width:860px"><h1 class="page-title">Settings</h1>
    <div class="set-group"><h3>Library</h3>
      <div class="set"><div class="l"><div>Install games to</div><div class="path space">${esc(L.dir || '…')}</div>
        ${L.total ? `<div class="space"><div class="bar"><i style="width:${used}%"></i></div><div class="d" style="margin-top:5px">${fmtSize(L.free)} free of ${fmtSize(L.total)}</div></div>` : ''}
        <div class="d">New games go here. Games you already installed stay where they are.</div></div>
        <button class="btn ghost small" data-act="folder">CHANGE</button></div>
      ${tog('autoUpdate', 'Update games automatically', 'When the launcher starts, it downloads new versions of your installed games.')}
      ${tog('shortcut', 'Desktop shortcuts', 'Put a shortcut on the desktop when a game finishes installing.')}
      ${tog('notify', 'Pop-ups', 'Show a little message when a download finishes.')}
    </div>
    <div class="set-group"><h3>Account</h3>
      <div class="set"><div class="l"><div>${esc(S.session.name)}</div><div class="d">${S.session.token ? 'Ruff Dogs account. Same login as OVERDRIVE online.' : 'Playing offline as a guest.'}</div></div>
        ${S.session.token ? '<button class="btn ghost small" data-act="signout">SIGN OUT</button>' : '<button class="btn play small" data-act="signin">SIGN IN</button>'}</div>
      <div class="set"><div class="l"><div><span class="server-dot ${S.server.status === 'up' ? 'up' : 'down'}"></span>Ruff Dogs servers</div><div class="d">${S.server.status === 'up' ? 'Online' : 'Asleep. Accounts and online play need the studio Mac to be on.'}</div></div></div>
    </div>
    <div class="set-group"><h3>About</h3>
      <div class="set"><div class="l"><div>Ruff Dogs Launcher ${esc(a.version || '')}${a.build ? ` (build ${a.build})` : ''}</div><div class="d">${appStatusText()}</div></div>
        ${S.appStatus && S.appStatus.state === 'updated' ? '<button class="btn play small" data-act="restart">RESTART NOW</button>' : `<button class="btn ghost small" data-act="appcheck" ${S.appStatus && /checking|downloading/.test(S.appStatus.state) ? 'disabled' : ''}>CHECK FOR UPDATES</button>`}</div>
      <div class="set"><div class="l"><div>Store list</div><div class="d">From ${fmtAgo(S.catalogWhen).toLowerCase()} · ${S.games.length} games · No real money, ever.</div></div>
        <button class="btn ghost small" data-act="recheck">REFRESH STORE</button></div>
    </div>
  </div>`;
}

// ------------------------------------------------------------------------------------------------ launcher updates
// The program checks for a new launcher every time it starts (and when you press CHECK FOR UPDATES). A new one is
// downloaded and put in place straight away; it runs from the next start, or right now with RESTART.
function appStatusText() {
  const st = S.appStatus || {};
  if (Native.kind !== 'windows') return 'Updates come from the Windows program.';
  return st.state === 'checking' ? 'Checking for a new version…'
    : st.state === 'downloading' ? `Downloading build ${st.build}…`
    : st.state === 'updated' ? `Build ${st.build} is ready. Restart the launcher to use it.`
    : st.state === 'failed' ? 'Couldn\'t check for updates. It tries again next time you open the launcher.'
    : st.state === 'current' ? 'Up to date. It checks for a new version every time it starts.'
    : 'It checks for a new version every time it starts.';
}
function renderAppBar() {
  let bar = $('#appbar');
  const show = S.appStatus && S.appStatus.state === 'updated';
  if (!show) { if (bar) bar.remove(); return; }
  if (!bar) { bar = document.createElement('div'); bar.id = 'appbar'; document.body.append(bar); }
  bar.innerHTML = `<b>The launcher updated.</b><span>Restart it to use the new version.</span><button class="btn play small" data-act="restart">RESTART</button><button class="x" title="Later" aria-label="Later">&times;</button>`;
  bar.querySelector('[data-act=restart]').onclick = () => Native.send({ t: 'restart' });
  bar.querySelector('.x').onclick = () => bar.remove();
}

// ------------------------------------------------------------------------------------------------ modals + toasts
function modal(kind, id, html) {
  const m = $('#modal'); m.hidden = false; m.dataset.kind = kind; m.dataset.id = id || '';
  m.innerHTML = `<div class="mbox">${html}</div>`;
  m.onclick = (e) => { if (e.target === m || e.target.closest('[data-close]')) closeModal(); };
  wire(m);
  return m;
}
const closeModal = () => { const m = $('#modal'); m.hidden = true; m.innerHTML = ''; m.dataset.kind = ''; };
function openInstall(g) {
  if (!g) return;
  const L = S.native.library || {}, room = !L.free || L.free > g.size * 1.2;
  const m = modal('install', g.id, `<h3>Install ${esc(g.title)}</h3><p>${esc(g.tagline)}</p>
    <div class="kv"><span>Download size</span><span>${fmtSize(g.size)}</span></div>
    <div class="kv"><span>Install to</span><span class="path" style="max-width:270px">${esc((L.dir || '') + (L.sep || '\\') + g.title.replace(/[^A-Za-z0-9 '\-]/g, ''))}</span></div>
    <div class="kv"><span>Free space</span><span style="color:${room ? 'inherit' : 'var(--red)'}">${L.free ? fmtSize(L.free) : '…'}</span></div>
    <label class="check" style="margin-top:14px"><input type="checkbox" id="m-short" ${S.settings.shortcut ? 'checked' : ''}> Make a desktop shortcut</label>
    <div class="acts"><button class="btn ghost small" data-act="folder">CHANGE FOLDER</button><span style="flex:1"></span><button class="btn ghost small" data-close>CANCEL</button><button class="btn install small" id="m-go" ${room ? '' : 'disabled'}>${ICON.down} INSTALL</button></div>`);
  m.querySelector('#m-go').onclick = () => {
    const sc = m.querySelector('#m-short').checked;
    closeModal(); own(g.id);
    S.queue.push({ id: g.id, kind: 'install', shortcut: sc }); pump(); render();
  };
}
function openManage(g) {
  const i = installed(g.id); if (!i) return;
  const m = modal('manage', g.id, `<h3>${esc(g.title)}</h3><p>Installed in <span class="path">${esc(i.dir)}</span></p>
    <div style="display:grid;gap:8px">
      <button class="btn ghost small" id="mg-files">BROWSE LOCAL FILES</button>
      <button class="btn ghost small" id="mg-short">MAKE A DESKTOP SHORTCUT</button>
      <button class="btn ghost small" id="mg-verify">CHECK GAME FILES</button>
      <button class="btn stop small" id="mg-un">UNINSTALL</button>
    </div><div class="acts"><button class="btn ghost small" data-close>CLOSE</button></div>`);
  m.querySelector('#mg-files').onclick = () => { Native.send({ t: 'reveal', id: g.id }); closeModal(); };
  m.querySelector('#mg-short').onclick = () => { Native.send({ t: 'shortcut', id: g.id, title: g.title }); closeModal(); toast(g, 'Shortcut made', 'Look on your desktop.'); };
  m.querySelector('#mg-verify').onclick = () => { closeModal(); S.queue.push({ id: g.id, kind: 'update' }); pump(); render(); };
  m.querySelector('#mg-un').onclick = () => {
    const c = modal('uninstall', g.id, `<h3>Uninstall ${esc(g.title)}?</h3><p>This deletes the game's files from this computer. It stays in your library and you can install it again any time. Your saves are kept.</p>
      <div class="acts"><button class="btn ghost small" data-close>KEEP IT</button><button class="btn stop small" id="un-go">UNINSTALL</button></div>`);
    c.querySelector('#un-go').onclick = () => { Native.send({ t: 'uninstall', id: g.id }); delete S.native.installed[g.id]; closeModal(); render(); toast(g, 'Uninstalled', g.title + ' was removed.'); };
  };
}
function toast(g, title, text) {
  const t = document.createElement('div'); t.className = 'toast';
  t.innerHTML = `${g ? art(g, 'icon') : '<div class="art"><img src="brand/dog.png" alt=""></div>'}<div><b>${esc(title)}</b><span>${esc(text)}</span></div>`;
  $('#toasts').appendChild(t);
  setTimeout(() => { t.style.transition = 'opacity .4s'; t.style.opacity = 0; setTimeout(() => t.remove(), 450); }, 5000);
}
document.addEventListener('keydown', (e) => { if (e.key === 'Escape') closeModal(); });

// ------------------------------------------------------------------------------------------------ search (top bar)
// Finds games by name first, then by genre, tagline and what changed in their updates. "/" or Ctrl+K jumps to it.
function searchGames(q) {
  q = q.trim().toLowerCase(); if (!q) return [];
  const score = (g) => {
    const t = g.title.toLowerCase();
    if (t.startsWith(q)) return 100; if (t.includes(q)) return 80;
    if ((g.genres || []).concat(g.modes || []).some((x) => x.toLowerCase().startsWith(q))) return 60;
    if ((g.tagline + ' ' + g.about).toLowerCase().includes(q)) return 40;
    if ((g.notes || []).some((n) => (n.title + ' ' + (n.items || []).join(' ')).toLowerCase().includes(q))) return 20;
    return 0;
  };
  return S.games.map((g) => [score(g), g]).filter(([s]) => s).sort((a, b) => b[0] - a[0]).map(([, g]) => g);
}
(() => {
  const box = $('#gsearch'), res = $('#gresults'); let sel = 0, hits = [];
  const draw = () => {
    hits = searchGames(box.value).slice(0, 7);
    if (!box.value.trim()) { res.hidden = true; return; }
    res.hidden = false;
    res.innerHTML = hits.length ? hits.map((g, i) => `<div class="gr ${i === sel ? 'on' : ''}" data-i="${i}">${art(g, 'icon', 'ic')}<div class="tx"><b>${esc(g.title)}</b><span>${esc(g.tagline)}</span></div><em>${installed(g.id) ? 'Installed' : owned().has(g.id) ? 'In library' : 'Free'}</em></div>`).join('')
      + `<div class="gr all" data-i="-1">See all results in the store</div>` : `<div class="gr none">No games match "${esc(box.value.trim())}"</div>`;
    res.querySelectorAll('.gr[data-i]').forEach((r) => r.onmousedown = (e) => { e.preventDefault(); pick(+r.dataset.i); });
  };
  const pick = (i) => {
    const g = hits[i];
    if (g) go('game', g.id); else { S.storeSearch = box.value; S.storeFilter = 'All'; go('store'); setTimeout(() => { const s = $('#store-search'); if (s) s.scrollIntoView({ block: 'center' }); }, 30); }
    box.value = ''; res.hidden = true; box.blur();
  };
  box.oninput = () => { sel = 0; draw(); };
  box.onfocus = draw;
  box.onblur = () => setTimeout(() => { res.hidden = true; }, 120);
  box.onkeydown = (e) => {
    if (e.key === 'ArrowDown') { sel = Math.min(sel + 1, hits.length - 1); draw(); e.preventDefault(); }
    else if (e.key === 'ArrowUp') { sel = Math.max(sel - 1, 0); draw(); e.preventDefault(); }
    else if (e.key === 'Enter') pick(hits.length ? sel : -1);
    else if (e.key === 'Escape') { box.value = ''; res.hidden = true; box.blur(); }
  };
  document.addEventListener('keydown', (e) => {
    const typing = /INPUT|TEXTAREA/.test(document.activeElement.tagName);
    if ((e.key === '/' && !typing) || (e.key.toLowerCase() === 'k' && (e.ctrlKey || e.metaKey))) { e.preventDefault(); box.focus(); box.select(); }
  });
})();
document.addEventListener('contextmenu', (e) => { if (!e.target.closest('input')) e.preventDefault(); });

boot();
