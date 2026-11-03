#!/usr/bin/env python3
"""Builds RuffDogsLauncher.exe (native/ruffdogs.c + cefhost.h, on our bundled Chromium) and RuffDogsSetup.exe (Setup with
the launcher and the engine inside).

  python3 tools/pack.py      ->  <build>/dist/RuffDogsLauncher.exe  and  RuffDogsLauncher.zip

Big tools live on the external drive (RD_BUILD, default /Volumes/QUICKSILVER/ruffdogs-build). The compiler and the
Zig compiler is shared with OVERDRIVE's build (/Volumes/QUICKSILVER/overdrive-build); CEF lives in <build>/cef.
"""
import os, struct, subprocess, sys, zipfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import uimanifest

ROOT = uimanifest.ROOT
NATIVE = os.path.join(ROOT, 'native')
BUILD = os.environ.get('RD_BUILD') or '/Volumes/QUICKSILVER/ruffdogs-build'
TOOLS = os.environ.get('RD_TOOLS') or '/Volumes/QUICKSILVER/overdrive-build'
CEF = os.environ.get('RD_CEF') or os.path.join(BUILD, 'cef', 'cef150')        # unpacked CEF "minimal" build for windows64
ENGINE_VER = '150.0.23'                                                       # must match ENGINE_VER in native/cefhost.h

def compile_exe(out):
    libs = ['user32', 'gdi32', 'shell32', 'shlwapi', 'ole32', 'advapi32', 'winhttp', 'bcrypt', 'dwmapi', 'uuid']
    cmd = [os.path.join(TOOLS, 'zig', 'zig'), 'build-exe', '-target', 'x86_64-windows-gnu', '-O', 'ReleaseSmall', '-fstrip',
           '--subsystem', 'windows', '-lc', '-I', CEF,
           'ruffdogs.c', 'ruffdogs.rc', *[f'-l{l}' for l in libs], '--name', 'RuffDogsLauncher', f'-femit-bin={out}']
    subprocess.run(cmd, cwd=NATIVE, check=True)

def build_engine():
    """The engine Setup installs: CEF's runtime files, trimmed to English, as a .tar.gz Windows' tar.exe can unpack.
    Built once per CEF version and reused."""
    out = os.path.join(BUILD, f'engine-{ENGINE_VER}.tar.gz')
    if os.path.isfile(out): return out
    import tarfile
    rel, res = os.path.join(CEF, 'Release'), os.path.join(CEF, 'Resources')
    with tarfile.open(out + '.tmp', 'w:gz', compresslevel=9) as t:
        for f in sorted(os.listdir(rel)):
            if f.endswith(('.lib', '.pdb')) or f.startswith('bootstrap') or f in ('dxcompiler.dll', 'dxil.dll'): continue   # WebGPU only; the games use WebGL
            t.add(os.path.join(rel, f), f)
        for f in sorted(os.listdir(res)):
            if f == 'locales': continue
            t.add(os.path.join(res, f), f)
        t.add(os.path.join(res, 'locales', 'en-US.pak'), 'locales/en-US.pak')
    os.replace(out + '.tmp', out)
    return out

def build_setup(launcher, dist):
    """RuffDogsSetup.exe: the install wizard (native/setup.c) with the launcher and the engine riding on the end.
    [setup program][launcher][engine.tar.gz][b'RDSETUP2'][u64 exe off, len][u64 engine off, len][engine version, 32 bytes]"""
    bare = os.path.join(BUILD, 'RuffDogsSetup-bare.exe')
    libs = ['user32', 'gdi32', 'shell32', 'shlwapi', 'ole32', 'advapi32', 'gdiplus', 'dwmapi', 'uuid']
    subprocess.run([os.path.join(TOOLS, 'zig', 'zig'), 'build-exe', '-target', 'x86_64-windows-gnu', '-O', 'ReleaseSmall', '-fstrip',
                    '--subsystem', 'windows', '-lc', 'setup.c', 'setup.rc', *[f'-l{l}' for l in libs], '--name', 'RuffDogsSetup',
                    f'-femit-bin={bare}'], cwd=NATIVE, check=True)
    engine = build_engine()
    out = os.path.join(dist, 'RuffDogsSetup.exe')
    with open(out, 'wb') as f:
        f.write(open(bare, 'rb').read())
        exe_off = f.tell(); data = open(launcher, 'rb').read(); f.write(data)
        eng_off = f.tell()
        with open(engine, 'rb') as e:
            while True:
                chunk = e.read(1 << 22)
                if not chunk: break
                f.write(chunk)
        eng_len = f.tell() - eng_off
        f.write(b'RDSETUP2' + struct.pack('<QQQQ', exe_off, len(data), eng_off, eng_len) + ENGINE_VER.encode().ljust(32, b'\0'))
    old = os.path.join(dist, 'RuffDogsSetup.zip')            # setups go out as a plain .exe, never zipped
    if os.path.isfile(old): os.remove(old)
    return out

def main():
    man = uimanifest.update()
    files = {rel: open(os.path.join(uimanifest.UI, rel), 'rb').read() for rel in man['files']}
    files['version.json'] = open(os.path.join(uimanifest.UI, 'version.json'), 'rb').read()
    files['_launcher/config.json'] = files['config.json']

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
    setup = build_setup(exe, dist)
    mb = lambda n: f'{n / 1048576:.1f} MB'
    print(f'built {setup} ({mb(os.path.getsize(setup))})  <- give people this one')
    print(f'page version {man["version"]}: program {size // 1024} KB + {len(files)} packed files')
    print(f'built {exe} ({mb(os.path.getsize(exe))}), {z} ({mb(os.path.getsize(z))})')

if __name__ == '__main__':
    main()
