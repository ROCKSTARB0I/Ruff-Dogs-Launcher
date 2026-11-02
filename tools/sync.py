#!/usr/bin/env python3
"""Builds the store catalog for the Ruff Dogs Launcher from games.json and each game's GitHub repo.

For every game it asks GitHub for the newest commit and its file list, and writes
  ui/catalog.json               the store: titles, art, sizes, what changed, newest commit per game
  ui/manifests/<id>.json        every file of that game (path -> git blob sha) and where to download it
It also copies each game's art from its local folder into ui/art/<id>/ and makes a Windows icon (game.ico).

Installed launchers read the catalog on start, so after a game is pushed to GitHub:
  python3 tools/sync.py         then   python3 tools/publish.py
Public repos only need no token. Unchanged games reuse their old manifest (saves GitHub's 60-calls-an-hour limit).
"""
import json, os, shutil, struct, subprocess, sys, time, urllib.error, urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ATLAS = os.path.dirname(ROOT)
UI = os.path.join(ROOT, 'ui')
SKIP = ('.git', '.github/', 'README.md', '.DS_Store')

def get(url, accept='application/vnd.github+json'):
    req = urllib.request.Request(url, headers={'Accept': accept, 'User-Agent': 'ruff-dogs-launcher-sync'})
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.load(r)

def art_for(g):
    """Copies the best art we have for a game into ui/art/<id>/ and returns {kind: relative path}."""
    src = os.path.join(ATLAS, g.get('folder', ''))
    out = os.path.join(UI, 'art', g['id']); os.makedirs(out, exist_ok=True)
    found = {}
    choices = {
        'cover': ['../overdrive-steam/library/library_capsule.jpg'],                 # tall 2:3 box art
        'header': ['../overdrive-steam/library/library_header.jpg', 'cover.jpg'],     # wide share card
        'hero': ['../overdrive-steam/library/library_hero.jpg', 'launcher/hero.jpg'],
        'logo': ['../overdrive-steam/library/library_logo.png'],
        'icon': ['../overdrive-steam/library/icon_256.png', 'icon-512.png', 'icon-192.png', 'apple-touch-icon.png'],
    }
    if g['id'] != 'overdrive':
        choices = {k: [p for p in v if not p.startswith('../overdrive-steam')] for k, v in choices.items()}
        choices['hero'] = []
    for kind, paths in choices.items():
        for p in paths:
            f = os.path.normpath(os.path.join(src, p))
            if os.path.isfile(f):
                ext = os.path.splitext(f)[1].lower()
                dst = os.path.join(out, kind + ext)
                if not os.path.isfile(dst) or os.path.getmtime(dst) < os.path.getmtime(f): shutil.copyfile(f, dst)
                found[kind] = f'art/{g["id"]}/{kind}{ext}'; break
    # hand-made art dropped straight into ui/art/<id>/ always wins
    for f in sorted(os.listdir(out)):
        kind, ext = os.path.splitext(f)
        if kind in ('cover', 'header', 'hero', 'logo', 'icon', 'shot1', 'shot2', 'shot3', 'shot4') and ext in ('.jpg', '.png', '.webp'):
            found.setdefault(kind, f'art/{g["id"]}/{f}')
    if 'icon' in found: make_ico(os.path.join(UI, found['icon']), os.path.join(out, 'game.ico')); found['ico'] = f'art/{g["id"]}/game.ico'
    return found

def make_ico(png, ico):
    """A Windows .ico holding one 256px PNG (Windows Vista and later read PNG icons)."""
    if os.path.isfile(ico) and os.path.getmtime(ico) > os.path.getmtime(png): return
    tmp = ico + '.png'
    subprocess.run(['sips', '-s', 'format', 'png', '-z', '256', '256', png, '--out', tmp], check=True, capture_output=True)
    data = open(tmp, 'rb').read(); os.remove(tmp)
    with open(ico, 'wb') as f:
        f.write(struct.pack('<HHH', 0, 1, 1) + struct.pack('<BBBBHHII', 0, 0, 0, 0, 1, 32, len(data), 22) + data)

def main():
    spec = json.load(open(os.path.join(ROOT, 'games.json')))
    owner = spec['owner']
    os.makedirs(os.path.join(UI, 'manifests'), exist_ok=True)
    try: old = {g['id']: g for g in json.load(open(os.path.join(UI, 'catalog.json')))['games']}
    except (OSError, ValueError, KeyError): old = {}
    games = []
    for g in spec['games']:
        repo, branch = g['repo'], g.get('branch', 'main')
        mpath = os.path.join(UI, 'manifests', g['id'] + '.json')
        try:
            c = get(f'https://api.github.com/repos/{owner}/{repo}/commits/{branch}')
        except urllib.error.HTTPError as e:
            if g['id'] in old and os.path.isfile(mpath):
                print(f'  {g["title"]}: GitHub said {e.code}, keeping the last catalog entry'); games.append({**old[g['id']], **{k: v for k, v in g.items() if k not in ('exclude', 'folder')}}); continue
            print(f'  {g["title"]}: GitHub said {e.code}, left out'); continue
        sha = c['sha']
        prev = old.get(g['id'])
        if prev and prev.get('commit') == sha and os.path.isfile(mpath):
            man = json.load(open(mpath))
        else:
            tree = get(f'https://api.github.com/repos/{owner}/{repo}/git/trees/{sha}?recursive=1')
            ex = tuple(g.get('exclude', []))
            files, sizes = {}, {}
            for e in tree['tree']:
                p = e['path']
                if e['type'] != 'blob' or p.startswith(SKIP) or os.path.basename(p) in SKIP or (ex and p.startswith(ex)): continue
                files[p] = e['sha']; sizes[p] = e.get('size', 0)
            if 'index.html' not in files: print(f'  {g["title"]}: no index.html in the repo, left out'); continue
            man = {'id': g['id'], 'commit': sha, 'base': f'https://raw.githubusercontent.com/{owner}/{repo}/{sha}/',
                   'files': files, 'sizes': sizes}
            json.dump(man, open(mpath, 'w'), indent=0, separators=(',', ':'))
        info = c['commit']
        entry = {k: v for k, v in g.items() if k not in ('exclude', 'folder')}
        entry.update({
            'commit': sha, 'updated': info['committer']['date'], 'news': info['message'].split('\n')[0][:140],
            'size': sum(man['sizes'].values()), 'files': len(man['files']),
            'web': f'https://{owner.lower()}.github.io/{repo}/', 'art': art_for(g),
            'added': (prev or {}).get('added') or time.strftime('%Y-%m-%d'),
        })
        games.append(entry)
        print(f'  {g["title"]:<18} {sha[:7]}  {len(man["files"]):>4} files  {entry["size"] / 1048576:6.1f} MB  {entry["news"][:50]}')
    cat = {'version': int(time.time()), 'generated': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()), 'games': games}
    json.dump(cat, open(os.path.join(UI, 'catalog.json'), 'w'), indent=1)
    for f in os.listdir(os.path.join(UI, 'manifests')):     # games removed from the store
        if f[:-5] not in {g['id'] for g in games}: os.remove(os.path.join(UI, 'manifests', f))
    print(f'catalog: {len(games)} games')

if __name__ == '__main__':
    main()
