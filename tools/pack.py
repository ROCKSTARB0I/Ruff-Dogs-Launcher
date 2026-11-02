#!/usr/bin/env python3
"""Builds RuffDogsLauncher.exe: compiles native/ruffdogs.c with Zig and glues the launcher page (ui/) onto the end.

  python3 tools/pack.py      ->  <build>/dist/RuffDogsLauncher.exe  and  RuffDogsLauncher.zip

Big tools live on the external drive (RD_BUILD, default /Volumes/QUICKSILVER/ruffdogs-build). The compiler and the
WebView2 SDK are shared with OVERDRIVE's build (/Volumes/QUICKSILVER/overdrive-build).
"""
import os, struct, subprocess, sys, zipfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import uimanifest

ROOT = uimanifest.ROOT
NATIVE = os.path.join(ROOT, 'native')
BUILD = os.environ.get('RD_BUILD') or '/Volumes/QUICKSILVER/ruffdogs-build'
TOOLS = os.environ.get('RD_TOOLS') or '/Volumes/QUICKSILVER/overdrive-build'
SDK = os.path.join(TOOLS, 'webview2-sdk', 'pkg', 'build', 'native')

def compile_exe(out):
    libs = ['user32', 'gdi32', 'shell32', 'shlwapi', 'ole32', 'advapi32', 'winhttp', 'bcrypt', 'dwmapi', 'uuid']
    cmd = [os.path.join(TOOLS, 'zig', 'zig'), 'build-exe', '-target', 'x86_64-windows-gnu', '-O', 'ReleaseSmall', '-fstrip',
           '--subsystem', 'windows', '-lc', '-I', os.path.join(TOOLS, 'shim'), '-I', os.path.join(SDK, 'include'),
           'ruffdogs.c', 'ruffdogs.rc', *[f'-l{l}' for l in libs], '--name', 'RuffDogsLauncher', f'-femit-bin={out}']
    subprocess.run(cmd, cwd=NATIVE, check=True)

def build_setup(launcher, dist):
    """RuffDogsSetup.exe: the install wizard (native/setup.c) with the launcher riding on the end.
    [setup program][launcher bytes][b'RDSETUP1'][u64 offset][u64 length]"""
    bare = os.path.join(BUILD, 'RuffDogsSetup-bare.exe')
    libs = ['user32', 'gdi32', 'shell32', 'shlwapi', 'ole32', 'advapi32', 'gdiplus', 'dwmapi', 'uuid']
    subprocess.run([os.path.join(TOOLS, 'zig', 'zig'), 'build-exe', '-target', 'x86_64-windows-gnu', '-O', 'ReleaseSmall', '-fstrip',
                    '--subsystem', 'windows', '-lc', 'setup.c', 'setup.rc', *[f'-l{l}' for l in libs], '--name', 'RuffDogsSetup',
                    f'-femit-bin={bare}'], cwd=NATIVE, check=True)
    data = open(launcher, 'rb').read()
    out = os.path.join(dist, 'RuffDogsSetup.exe')
    with open(out, 'wb') as f:
        f.write(open(bare, 'rb').read()); off = f.tell(); f.write(data)
        f.write(b'RDSETUP1' + struct.pack('<QQ', off, len(data)))
    z = os.path.join(dist, 'RuffDogsSetup.zip')
    with zipfile.ZipFile(z, 'w', zipfile.ZIP_DEFLATED) as zf: zf.write(out, 'RuffDogsSetup.exe')
    return out, z

def main():
    man = uimanifest.update()
    files = {rel: open(os.path.join(uimanifest.UI, rel), 'rb').read() for rel in man['files']}
    files['version.json'] = open(os.path.join(uimanifest.UI, 'version.json'), 'rb').read()
    files['_launcher/config.json'] = files['config.json']
    files['_native/WebView2Loader.dll'] = open(os.path.join(SDK, 'x64', 'WebView2Loader.dll'), 'rb').read()

    dist = os.path.join(BUILD, 'dist'); os.makedirs(dist, exist_ok=True)
    exe = os.path.join(dist, 'RuffDogsLauncher.exe')
    compile_exe(exe)
    size = os.path.getsize(exe)
    # [program][file bytes...][index][b'RDPAK001'][u64 index offset]
    with open(exe, 'ab') as f:
        entries = []
        for rel in sorted(files):
            entries.append((rel, f.tell(), len(files[rel]))); f.write(files[rel])
        idx = f.tell()
        f.write(struct.pack('<I', len(entries)))
        for rel, off, n in entries:
            b = rel.encode(); f.write(struct.pack('<H', len(b)) + b + struct.pack('<QQ', off, n))
        f.write(b'RDPAK001' + struct.pack('<Q', idx))
    z = os.path.join(dist, 'RuffDogsLauncher.zip')
    with zipfile.ZipFile(z, 'w', zipfile.ZIP_DEFLATED) as zf: zf.write(exe, 'RuffDogsLauncher.exe')
    setup, sz = build_setup(exe, dist)
    mb = lambda n: f'{n / 1048576:.1f} MB'
    print(f'built {setup} ({mb(os.path.getsize(setup))}), {sz} ({mb(os.path.getsize(sz))})  <- give people this one')
    print(f'page version {man["version"]}: program {size // 1024} KB + {len(files)} packed files')
    print(f'built {exe} ({mb(os.path.getsize(exe))}), {z} ({mb(os.path.getsize(z))})')

if __name__ == '__main__':
    main()
