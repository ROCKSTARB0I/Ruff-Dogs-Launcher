"""ui/version.json: every launcher page file with its git hash. Installed launchers compare it with their own copy
and download only what changed. The version number goes up only when a file actually changed."""
import hashlib, json, os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
UI = os.path.join(ROOT, 'ui')

def git_hash(data): return hashlib.sha1(b'blob %d\0' % len(data) + data).hexdigest()

def ui_files():
    out = {}
    for d, dirs, files in os.walk(UI):
        dirs[:] = sorted(x for x in dirs if not x.startswith('.'))
        for f in sorted(files):
            if f.startswith('.') or f == 'version.json': continue
            p = os.path.join(d, f)
            out[os.path.relpath(p, UI).replace(os.sep, '/')] = git_hash(open(p, 'rb').read())
    return out

def update():
    path = os.path.join(UI, 'version.json')
    try: old = json.load(open(path))
    except (OSError, ValueError): old = {'version': 0, 'files': {}}
    files = ui_files()
    if files == old.get('files'): return old
    man = {'version': old.get('version', 0) + 1, 'files': files}
    json.dump(man, open(path, 'w'), indent=0, separators=(',', ':'))
    return man
