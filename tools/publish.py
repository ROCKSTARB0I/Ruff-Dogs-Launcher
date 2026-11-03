#!/usr/bin/env python3
"""Publishes the Ruff Dogs Launcher to GitHub in one commit. Every installed launcher picks it up on its next start:
new games in the store, game updates, and changes to the launcher's own page.

  python3 tools/sync.py        when a game changed on GitHub (or games.json changed)
  python3 tools/publish.py     then this

Uploads only files that changed. Never uploads the dev library, tokens, or anything outside the folders listed below.
Uses the same GitHub token as the server manager (server-manager/github-token.txt).
"""
import base64, json, os, re, sys, urllib.error, urllib.request
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import uimanifest

ROOT = uimanifest.ROOT
REPO, BRANCH = 'ROCKSTARB0I/Ruff-Dogs-Launcher', 'main'
TOKEN_FILE = os.environ.get('RD_TOKEN_FILE') or os.path.join(os.path.dirname(ROOT), 'server-manager', 'github-token.txt')
SOURCES = ['games.json', 'notes.json', 'README.md', 'native/setup.c', 'native/setup.rc',
           'native/setup-art/side.png', 'native/setup-art/wordmark.png', 'native/setup-art/dog.png', '.gitignore', 'native/ruffdogs.c', 'native/ruffdogs.rc', 'native/ruffdogs.manifest',
           'native/icon.ico', 'tools/sync.py', 'tools/pack.py', 'tools/publish.py', 'tools/uimanifest.py', 'dev/devserver.py']
MANAGED = ('ui/',)                                  # files gone locally get deleted from the repo here

def gh(method, path, body=None):
    req = urllib.request.Request(f'https://api.github.com/repos/{REPO}{path}', method=method,
                                 data=json.dumps(body).encode() if body is not None else None,
                                 headers={'Authorization': f'Bearer {TOKEN}', 'Accept': 'application/vnd.github+json',
                                          'User-Agent': 'ruffdogs-publish', 'Content-Type': 'application/json'})
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            return json.load(r)
    except urllib.error.HTTPError as e:
        if e.code in (404, 409) and path.startswith('/git/ref/'):
            return None
        sys.exit(f'GitHub said {e.code} for {method} {path}: {e.read()[:300].decode("utf-8", "replace")}')

BUILD = os.environ.get('RD_BUILD') or '/Volumes/QUICKSILVER/ruffdogs-build'

def raw_api(method, url, data=None, ctype='application/json'):
    req = urllib.request.Request(url, method=method, data=data, headers={'Authorization': f'Bearer {TOKEN}', 'Accept': 'application/vnd.github+json',
                                                                       'User-Agent': 'ruffdogs-publish', 'Content-Type': ctype})
    try:
        with urllib.request.urlopen(req, timeout=1800) as r: return r.status, json.load(r)
    except urllib.error.HTTPError as e:
        return e.code, None

def engine_release(ver):
    """The engine Setup installs, as a GitHub release asset (too big for the repo itself). Installed launchers download it
    from here when a new launcher build needs a newer engine. Uploaded once per engine version."""
    name = f'engine-{ver}.tar.gz'
    url = f'https://github.com/{REPO}/releases/download/engine-{ver}/{name}'
    code, rel = raw_api('GET', f'https://api.github.com/repos/{REPO}/releases/tags/engine-{ver}')
    if code == 404:
        code, rel = raw_api('POST', f'https://api.github.com/repos/{REPO}/releases', json.dumps({
            'tag_name': f'engine-{ver}', 'target_commitish': BRANCH, 'name': f'Game engine {ver}',
            'body': 'The Chromium (CEF) runtime the Ruff Dogs Launcher and its games run on. The launcher and Setup install it; you never need to download this by hand.'}).encode())
    if not rel: print(f'  (could not make the engine release: GitHub said {code})'); return url
    if any(a['name'] == name for a in rel.get('assets', [])): return url
    path = os.path.join(BUILD, name)
    if not os.path.isfile(path): print(f'  (engine archive {path} missing: run tools/pack.py first)'); return url
    print(f'  up   engine {ver} release ({os.path.getsize(path) / 1048576:.0f} MB, takes a few minutes)')
    up = rel['upload_url'].split('{')[0] + '?name=' + name
    code, _ = raw_api('POST', up, open(path, 'rb').read(), 'application/gzip')
    if code not in (200, 201): print(f'  (engine upload failed: GitHub said {code})')
    return url

def stage_app():
    """app/RuffDogsLauncher.exe + app/launcher.json: installed launchers update themselves to a higher APP_BUILD."""
    exe = os.path.join(BUILD, 'dist', 'RuffDogsLauncher.exe')
    if not os.path.isfile(exe): return []
    src = open(os.path.join(ROOT, 'native', 'ruffdogs.c')).read()
    build = int(re.search(r'#define APP_BUILD (\d+)', src).group(1)); eng = re.search(r'#define ENGINE_VER "([^"]+)"', src).group(1)
    data = open(exe, 'rb').read(); h = uimanifest.git_hash(data)
    code, cur = raw_api('GET', f'https://api.github.com/repos/{REPO}/contents/app/launcher.json?ref={BRANCH}')
    if cur and cur.get('content'):
        cur = json.loads(base64.b64decode(cur['content']))
        if cur.get('build', 0) >= build and cur.get('hash') != h:
            print(f'  (the launcher .exe changed but APP_BUILD is still {build}: bump it in native/ruffdogs.c to send it out; not sending it now)')
            return []
        if cur.get('build') == build and cur.get('hash') == h: return ['app/RuffDogsLauncher.exe', 'app/launcher.json']
    os.makedirs(os.path.join(ROOT, 'app'), exist_ok=True)
    open(os.path.join(ROOT, 'app', 'RuffDogsLauncher.exe'), 'wb').write(data)
    info = {'build': build, 'hash': h, 'size': len(data), 'engine': eng, 'engineUrl': engine_release(eng)}
    json.dump(info, open(os.path.join(ROOT, 'app', 'launcher.json'), 'w'), indent=1)
    print(f'  launcher build {build} ready to send ({len(data) / 1048576:.1f} MB)')
    return ['app/RuffDogsLauncher.exe', 'app/launcher.json']

def main():
    global TOKEN
    try: TOKEN = open(TOKEN_FILE).read().strip()
    except OSError: sys.exit(f'No GitHub token at {TOKEN_FILE}. See server-manager/README.md ("One-time setup").')
    man = uimanifest.update()
    want = ['ui/' + r for r in man['files']] + ['ui/version.json'] + [s for s in SOURCES if os.path.isfile(os.path.join(ROOT, s))]
    want += stage_app()
    for d, _, fs in os.walk(os.path.join(ROOT, 'art')):        # store art made for the launcher
        want += [os.path.relpath(os.path.join(d, f), ROOT).replace(os.sep, '/') for f in fs if not f.startswith('.')]

    ref = gh('GET', f'/git/ref/heads/{BRANCH}')
    if ref is None:                                   # brand-new empty repo: the first file has to go in through the contents API
        gh('PUT', '/contents/README.md', {'message': 'Start', 'content': base64.b64encode(open(os.path.join(ROOT, 'README.md'), 'rb').read()).decode(), 'branch': BRANCH})
        ref = gh('GET', f'/git/ref/heads/{BRANCH}')
    head = ref['object']['sha']
    base_tree = gh('GET', f'/git/commits/{head}')['tree']['sha']
    remote = {e['path']: e['sha'] for e in gh('GET', f'/git/trees/{base_tree}?recursive=1')['tree'] if e['type'] == 'blob'}

    tree, sent = [], 0
    for rel in want:
        data = open(os.path.join(ROOT, rel), 'rb').read()
        if remote.get(rel) == uimanifest.git_hash(data): continue
        b = gh('POST', '/git/blobs', {'content': base64.b64encode(data).decode(), 'encoding': 'base64'})
        tree.append({'path': rel, 'mode': '100644', 'type': 'blob', 'sha': b['sha']}); sent += len(data)
        print('  up  ', rel)
    gone = [p for p in remote if p.startswith(MANAGED) and not os.path.isfile(os.path.join(ROOT, p))]
    for p in gone:
        tree.append({'path': p, 'mode': '100644', 'type': 'blob', 'sha': None}); print('  del ', p)
    if not tree:
        print('GitHub already has everything.'); return
    t = gh('POST', '/git/trees', {'base_tree': base_tree, 'tree': tree})
    c = gh('POST', '/git/commits', {'message': f"Launcher page version {man['version']}", 'tree': t['sha'], 'parents': [head]})
    gh('PATCH', f'/git/refs/heads/{BRANCH}', {'sha': c['sha']})
    print(f"published page version {man['version']}: {len(tree) - len(gone)} file(s) up ({sent / 1048576:.1f} MB), {len(gone)} removed -> {c['html_url']}")

if __name__ == '__main__':
    main()
