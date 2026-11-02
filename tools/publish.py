#!/usr/bin/env python3
"""Publishes the Ruff Dogs Launcher to GitHub in one commit. Every installed launcher picks it up on its next start:
new games in the store, game updates, and changes to the launcher's own page.

  python3 tools/sync.py        when a game changed on GitHub (or games.json changed)
  python3 tools/publish.py     then this

Uploads only files that changed. Never uploads the dev library, tokens, or anything outside the folders listed below.
Uses the same GitHub token as the server manager (server-manager/github-token.txt).
"""
import base64, json, os, sys, urllib.error, urllib.request
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import uimanifest

ROOT = uimanifest.ROOT
REPO, BRANCH = 'ROCKSTARB0I/Ruff-Dogs-Launcher', 'main'
TOKEN_FILE = os.environ.get('RD_TOKEN_FILE') or os.path.join(os.path.dirname(ROOT), 'server-manager', 'github-token.txt')
SOURCES = ['games.json', 'README.md', '.gitignore', 'native/ruffdogs.c', 'native/ruffdogs.rc', 'native/ruffdogs.manifest',
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

def main():
    global TOKEN
    try: TOKEN = open(TOKEN_FILE).read().strip()
    except OSError: sys.exit(f'No GitHub token at {TOKEN_FILE}. See server-manager/README.md ("One-time setup").')
    man = uimanifest.update()
    want = ['ui/' + r for r in man['files']] + ['ui/version.json'] + [s for s in SOURCES if os.path.isfile(os.path.join(ROOT, s))]

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
