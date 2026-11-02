#!/usr/bin/env python3
"""Runs the Ruff Dogs Launcher on the Mac for testing, standing in for RuffDogsLauncher.exe.

Serves the launcher page (ui/) and answers its messages the way the .exe does: installs really download every game
file from GitHub (checked against its git hash) into a library folder, updates only fetch what changed, and PLAY
opens the installed copy (served at /play/<id>/) in a new tab.

  python3 dev/devserver.py [port]          default 9850
  RD_LIBRARY=/some/folder                  where games install (default: ./dev/library)
  RD_ACCOUNT_SERVER=http://localhost:9851  account server to use (a throwaway copy of the OVERDRIVE server)
"""
import hashlib, json, os, queue, re, shutil, sys, threading, time, urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import unquote, urlparse

PORT = int(os.environ.get('PORT') or (sys.argv[1] if len(sys.argv) > 1 else 9850))
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
UI = os.path.join(ROOT, 'ui')
LIB = os.environ.get('RD_LIBRARY') or os.path.join(ROOT, 'dev', 'library')
os.makedirs(LIB, exist_ok=True)
clients, lock = [], threading.RLock()
cancel = set()

def emit(msg):
    with lock:
        for q in clients: q.put(msg)

def blob_sha(data): return hashlib.sha1(b'blob %d\0' % len(data) + data).hexdigest()
def safe_rel(p): return p and not p.startswith('/') and '..' not in p.split('/') and '\\' not in p and ':' not in p
def folder_name(title): return re.sub(r'[^A-Za-z0-9 \'\-]', '', title).strip() or 'Game'

def installed():
    out = {}
    for d in os.listdir(LIB):
        f = os.path.join(LIB, d, 'ruffdogs-game.json')
        if os.path.isfile(f):
            try: j = json.load(open(f))
            except ValueError: continue
            out[j['id']] = {'commit': j['commit'], 'dir': os.path.join(LIB, d), 'size': j.get('size', 0), 'installedAt': j.get('installedAt')}
    return out

def state():
    du = shutil.disk_usage(LIB)
    return {'t': 'state', 'app': {'version': 'dev', 'platform': 'mac-dev'}, 'installed': installed(), 'running': None,
            'library': {'dir': LIB, 'free': du.free, 'total': du.total, 'sep': '/'},
            'accountServer': os.environ.get('RD_ACCOUNT_SERVER', '')}

def fetch(url):
    if url.startswith(f'http://localhost:{PORT}/') or url.startswith(f'http://127.0.0.1:{PORT}/'):
        with open(os.path.join(UI, unquote(urlparse(url).path).lstrip('/')), 'rb') as f: return f.read()
    req = urllib.request.Request(url, headers={'User-Agent': 'RuffDogsLauncher'})
    with urllib.request.urlopen(req, timeout=120) as r: return r.read()

def install(m):
    gid = m['id']; cancel.discard(gid)
    def prog(phase, **k): emit({'t': 'progress', 'id': gid, 'phase': phase, **k})
    try:
        prog('manifest')
        man = json.loads(fetch(m['manifest']))
        files, sizes, base = man['files'], man['sizes'], man['base']
        have = installed().get(gid)
        dest = have['dir'] if have else os.path.join(LIB, folder_name(m['title']))
        old = {}
        if have:
            try: old = json.load(open(os.path.join(dest, 'ruffdogs-game.json'))).get('files', {})
            except (OSError, ValueError): pass
        stage = os.path.join(dest, '.rd-download')
        shutil.rmtree(stage, ignore_errors=True); os.makedirs(stage)
        total_bytes, done_bytes, todo = sum(sizes.values()), 0, []
        for rel, sha in files.items():
            if not safe_rel(rel): continue
            p = os.path.join(dest, 'game', rel)
            if old.get(rel) == sha and os.path.isfile(p) or (os.path.isfile(p) and blob_sha(open(p, 'rb').read()) == sha):
                done_bytes += sizes.get(rel, 0); continue
            todo.append(rel)
        prog('downloading', done=len(files) - len(todo), total=len(files), bytes=done_bytes, totalBytes=total_bytes)
        count = [len(files) - len(todo), done_bytes]
        def one(rel):                         # four downloads at once, like the .exe
            if gid in cancel: raise InterruptedError
            data = fetch(base + '/'.join(urllib.request.quote(x) for x in rel.split('/')))
            if blob_sha(data) != files[rel]: raise ValueError(f'{rel} came down damaged')
            p = os.path.join(stage, rel); os.makedirs(os.path.dirname(p), exist_ok=True)
            open(p, 'wb').write(data)
            with lock:
                count[0] += 1; count[1] += len(data)
                prog('downloading', done=count[0], total=len(files), bytes=count[1], totalBytes=total_bytes)
        from concurrent.futures import ThreadPoolExecutor
        with ThreadPoolExecutor(4) as pool: list(pool.map(one, todo))
        prog('finishing', done=len(files), total=len(files), bytes=total_bytes, totalBytes=total_bytes)
        for rel in todo:
            p = os.path.join(dest, 'game', rel); os.makedirs(os.path.dirname(p), exist_ok=True)
            os.replace(os.path.join(stage, rel), p)
        for rel in old:                       # files the new version doesn't have any more
            if rel not in files and safe_rel(rel):
                try: os.remove(os.path.join(dest, 'game', rel))
                except OSError: pass
        shutil.rmtree(stage, ignore_errors=True)
        rec = {'id': gid, 'title': m['title'], 'commit': man['commit'], 'size': total_bytes, 'files': files,
               'installedAt': time.strftime('%Y-%m-%dT%H:%M:%S')}
        json.dump(rec, open(os.path.join(dest, 'ruffdogs-game.json'), 'w'))
        prog('done', installed={'commit': man['commit'], 'dir': dest, 'size': total_bytes, 'installedAt': rec['installedAt']})
    except InterruptedError:
        shutil.rmtree(os.path.join(LIB, folder_name(m['title']), '.rd-download'), ignore_errors=True)
        prog('cancelled')
    except Exception as e:
        prog('failed', error=str(e)[:200])

def handle(m):
    t = m.get('t')
    if t == 'hello': emit(state())
    elif t == 'install': threading.Thread(target=install, args=(m,), daemon=True).start()
    elif t == 'cancel': cancel.add(m.get('id'))
    elif t == 'uninstall':
        have = installed().get(m.get('id'))
        if have and os.path.dirname(have['dir']) == LIB: shutil.rmtree(have['dir'], ignore_errors=True)
        emit(state())
    elif t == 'play':
        if m.get('id') in installed(): emit({'t': 'opened', 'url': f'/play/{m["id"]}/'})
    elif t == 'folder': emit({'t': 'folder', 'library': state()['library']})
    elif t in ('reveal', 'shortcut', 'account', 'stop'): pass      # Windows-only jobs

class H(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    def log_message(self, *a): pass
    def send(self, code, data, ctype='text/plain'):
        self.send_response(code); self.send_header('Content-Type', ctype); self.send_header('Content-Length', str(len(data)))
        self.send_header('Cache-Control', 'no-store'); self.end_headers(); self.wfile.write(data)
    def do_POST(self):
        n = int(self.headers.get('Content-Length') or 0)
        try: handle(json.loads(self.rfile.read(n)))
        except ValueError: pass
        self.send(204, b'')
    def do_GET(self):
        path = unquote(urlparse(self.path).path)
        if path == '/native/events':
            self.send_response(200); self.send_header('Content-Type', 'text/event-stream'); self.send_header('Cache-Control', 'no-store'); self.end_headers()
            q = queue.Queue()
            with lock: clients.append(q)
            try:
                while True:
                    try: msg = q.get(timeout=15); self.wfile.write(f'data: {json.dumps(msg)}\n\n'.encode())
                    except queue.Empty: self.wfile.write(b': ping\n\n')
                    self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError): pass
            finally:
                with lock: clients.remove(q)
            return
        root, rel = UI, path.lstrip('/')
        mp = re.match(r'^/play/([a-z0-9-]+)/(.*)$', path)
        if mp:
            have = installed().get(mp.group(1))
            if not have: return self.send(404, b'not installed')
            root, rel = os.path.join(have['dir'], 'game'), mp.group(2)
        f = os.path.abspath(os.path.join(root, rel))
        if os.path.isdir(f): f = os.path.join(f, 'index.html')
        if not f.startswith(os.path.abspath(root)) or not os.path.isfile(f): return self.send(404, b'not found')
        import mimetypes
        ctype = {'.js': 'text/javascript', '.mjs': 'text/javascript', '.json': 'application/json', '.m4a': 'audio/mp4'}.get(os.path.splitext(f)[1]) or mimetypes.guess_type(f)[0] or 'application/octet-stream'
        self.send(200, open(f, 'rb').read(), ctype)

ThreadingHTTPServer.daemon_threads = True
print(f'Ruff Dogs Launcher (dev) on http://localhost:{PORT}  library: {LIB}', flush=True)
ThreadingHTTPServer(('127.0.0.1', PORT), H).serve_forever()
