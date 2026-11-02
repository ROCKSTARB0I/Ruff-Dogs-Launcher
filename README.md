# Ruff Dogs Launcher

The Ruff Dogs Studios game launcher for Windows: a store, a library, downloads and Ruff Dogs accounts. Every game is free. No real money, ever.

- **Store**: every Ruff Dogs game, with what changed in its latest update.
- **Library**: your games. Install, play, update, uninstall, play time.
- **Installs like Steam**: each game gets its own folder on your drive (default `C:\Ruff Dogs Games\<Game>`) with its own `.exe`, plus desktop and Start menu shortcuts. You can pick another drive in Settings.
- **Auto-updates**: when the launcher starts, it updates your installed games. It downloads only the files that changed, and checks each one.
- **Accounts**: one Ruff Dogs account, the same one OVERDRIVE uses online. You can also play offline as a guest.

## How it works

- `games.json` is the store list. `tools/sync.py` reads each game's GitHub repo and writes `ui/catalog.json` and `ui/manifests/<id>.json` (every file and its git hash).
- `ui/` is the launcher page. The `.exe` carries a copy and updates it from this repo (`ui/version.json`).
- `native/ruffdogs.c` is the Windows program (C + WebView2, built with Zig). The same program runs each installed game when a `ruffdogs-game.json` sits beside it.

## Releasing

```
python3 tools/sync.py       # after any game is pushed to GitHub
python3 tools/publish.py    # sends the new store list to every launcher
python3 tools/pack.py       # only when native/ruffdogs.c changes: builds a new RuffDogsLauncher.exe
```

Test on a Mac with `python3 dev/devserver.py` (http://localhost:9850).
