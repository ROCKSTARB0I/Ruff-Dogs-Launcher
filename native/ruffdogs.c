// RuffDogsLauncher.exe: the Ruff Dogs Studios game launcher, written from scratch in C. No Electron, no framework.
//
// One program, two jobs:
//   LAUNCHER  (normal start) the store/library window. The page (ui/) is packed onto the end of this .exe and kept
//             up to date from the Ruff-Dogs-Launcher GitHub repo. It installs games like Steam does: each game gets
//             its own folder on the drive (default C:\Ruff Dogs Games\<Title>) with <Title>.exe, the game files in
//             game\, and desktop + Start menu shortcuts. Installs and updates download only the files that changed,
//             four at a time, each checked against its git hash before it is used.
//   GAME      (when a ruffdogs-game.json sits beside the .exe) runs that game in its own window. Every installed
//             <Title>.exe is a copy of this program with the game's icon written into it.
//
// Both run pages on our own bundled Chromium (CEF, see cefhost.h), installed once by RuffDogsSetup.exe and shared by
// the launcher and every game. No WebView2, no Edge.
//
// Build: python3 tools/pack.py   (compiles this with Zig and appends the launcher page)
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <dwmapi.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define APP_VERSION "1.1.0"
#define ENGINE_VER "150.0.23"          // the CEF build this program runs on (see cefhost.h); Setup installs it
#define LHOST L"https://launcher.ruffdogs/"
#define LHOST_A "https://launcher.ruffdogs/"
#define WM_JSON (WM_APP + 1)
#define WM_STARTWEB (WM_APP + 2)
#define WM_GAMEEXIT (WM_APP + 3)
#define WM_PICKFOLDER (WM_APP + 4)
#define WM_STATE (WM_APP + 5)
#define BIG (MAX_PATH * 2)

static HWND g_win;
static int g_isFs, g_gameMode, g_devtools;
static WINDOWPLACEMENT g_place = { sizeof(WINDOWPLACEMENT) };
static WCHAR g_self[BIG], g_data[BIG], g_uiDir[BIG], g_uiNext[BIG], g_lib[BIG];
static char g_repo[200] = "ROCKSTARB0I/Ruff-Dogs-Launcher", g_branch[64] = "main", g_uiPath[64] = "ui/", g_accountServer[300];
static char g_autoplay[64];
static volatile LONG g_useUpdate;
// game mode
static char g_gameId[64], g_gameTitle[128];
static WCHAR g_gameDir[BIG];

// ================================================================================================ small helpers
static WCHAR *widen(const char *s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
  WCHAR *w = malloc(n * sizeof(WCHAR)); MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n); return w;
}
static char *narrow(const WCHAR *w) {
  int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
  char *s = malloc(n); WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL); return s;
}
static int exists(const WCHAR *p) { return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES; }
// dir + "\" + rel (rel uses forward slashes)
static void join(WCHAR *out, const WCHAR *dir, const char *rel) {
  WCHAR *w = widen(rel);
  for (WCHAR *c = w; *c; c++) if (*c == L'/') *c = L'\\';
  _snwprintf(out, BIG, L"%ls\\%ls", dir, w); out[BIG - 1] = 0;
  free(w);
}
static int safe_rel(const char *rel) {
  if (!rel[0] || rel[0] == '/' || strchr(rel, '\\') || strchr(rel, ':')) return 0;
  for (const char *p = rel; (p = strstr(p, "..")); p += 2)
    if ((p == rel || p[-1] == '/') && (p[2] == '/' || !p[2])) return 0;
  return 1;
}
static char *read_file(const WCHAR *p, DWORD *len) {
  HANDLE f = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
  if (f == INVALID_HANDLE_VALUE) return NULL;
  DWORD n = GetFileSize(f, NULL), got = 0; char *b = malloc(n + 1);
  if (!ReadFile(f, b, n, &got, NULL) || got != n) { free(b); b = NULL; } else b[n] = 0;
  CloseHandle(f); if (len) *len = n; return b;
}
static void make_parent(const WCHAR *p) { WCHAR dir[BIG]; wcsncpy(dir, p, BIG - 1); dir[BIG - 1] = 0; PathRemoveFileSpecW(dir); SHCreateDirectoryExW(NULL, dir, NULL); }
static int write_file(const WCHAR *p, const void *b, DWORD n) {
  make_parent(p);
  HANDLE f = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
  if (f == INVALID_HANDLE_VALUE) return 0;
  DWORD put = 0; BOOL ok = WriteFile(f, b, n, &put, NULL) && put == n; CloseHandle(f); return ok;
}
static void remove_tree(const WCHAR *dir) {
  WCHAR from[BIG + 2] = { 0 }; wcsncpy(from, dir, BIG - 1);   // double-null terminated for SHFileOperation
  SHFILEOPSTRUCTW op = { 0 }; op.wFunc = FO_DELETE; op.pFrom = from; op.fFlags = FOF_NO_UI;
  if (exists(dir)) SHFileOperationW(&op);
}
// "Jason's Revenge" -> a name Windows accepts for a folder or .exe
static void safe_name(const char *title, WCHAR *out, int cap) {
  WCHAR *w = widen(title); int n = 0;
  for (WCHAR *c = w; *c && n < cap - 1; c++) if (!wcschr(L"<>:\"/\\|?*", *c) && *c >= 32) out[n++] = *c;
  while (n && (out[n - 1] == L' ' || out[n - 1] == L'.')) n--;
  out[n] = 0; if (!n) wcscpy(out, L"Game");
  free(w);
}

// ------------------------------------------------------------------------------------------------ growable string
typedef struct { char *s; size_t n, cap; } Buf;
static void b_add(Buf *b, const char *fmt, ...) {
  va_list a; va_start(a, fmt); int k = vsnprintf(NULL, 0, fmt, a); va_end(a);
  if (b->n + k + 1 > b->cap) { b->cap = (b->n + k + 1) * 2; b->s = realloc(b->s, b->cap); }
  va_start(a, fmt); vsnprintf(b->s + b->n, k + 1, fmt, a); va_end(a); b->n += k;
}
static void b_str(Buf *b, const char *s) {           // a JSON string literal
  b_add(b, "\"");
  for (; s && *s; s++) {
    unsigned char c = (unsigned char)*s;
    if (c == '"' || c == '\\') b_add(b, "\\%c", c);
    else if (c < 32) b_add(b, "\\u%04x", c);
    else b_add(b, "%c", c);
  }
  b_add(b, "\"");
}
static void b_wstr(Buf *b, const WCHAR *w) { char *s = narrow(w); b_str(b, s); free(s); }

// ------------------------------------------------------------------------------------------------ tiny JSON readers (our own small files and messages)
static const char *jstring(const char *p, char *out, int cap) {
  if (*p != '"') return NULL;
  int n = 0;
  for (p++; *p && *p != '"'; p++) {
    char c = *p;
    if (c == '\\' && p[1]) {
      p++; c = *p;
      if (c == 'n') c = '\n'; else if (c == 't') c = '\t'; else if (c == 'r') c = '\r';
      else if (c == 'u' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2]) && isxdigit((unsigned char)p[3]) && isxdigit((unsigned char)p[4])) {
        char h[5] = { p[1], p[2], p[3], p[4], 0 }; unsigned u = (unsigned)strtoul(h, NULL, 16); p += 4;
        char tmp[4]; int k = 0;
        if (u < 0x80) tmp[k++] = (char)u;
        else if (u < 0x800) { tmp[k++] = (char)(0xC0 | (u >> 6)); tmp[k++] = (char)(0x80 | (u & 63)); }
        else { tmp[k++] = (char)(0xE0 | (u >> 12)); tmp[k++] = (char)(0x80 | ((u >> 6) & 63)); tmp[k++] = (char)(0x80 | (u & 63)); }
        for (int i = 0; i < k && n < cap - 1; i++) out[n++] = tmp[i];
        continue;
      }
    }
    if (n < cap - 1) out[n++] = c;
  }
  out[n] = 0; return *p ? p + 1 : NULL;
}
static const char *json_key(const char *j, const char *key) {
  char k[80]; snprintf(k, sizeof k, "\"%s\"", key);
  const char *p = strstr(j, k); if (!p) return NULL;
  p = strchr(p + strlen(k), ':'); if (!p) return NULL;
  while (*++p == ' ' || *p == '\t' || *p == '\r' || *p == '\n');
  return p;
}
static int json_str(const char *j, const char *key, char *out, int cap) {
  out[0] = 0; const char *p = j ? json_key(j, key) : NULL;
  return p && jstring(p, out, cap) != NULL;
}
static long long json_num(const char *j, const char *key, long long def) {
  const char *p = j ? json_key(j, key) : NULL; if (!p) return def;
  if (!strncmp(p, "true", 4)) return 1;
  if (!strncmp(p, "false", 5)) return 0;
  return _atoi64(p);
}
// the raw text of an object/array/value, e.g. "profile": {...}
static char *json_raw(const char *j, const char *key) {
  const char *p = j ? json_key(j, key) : NULL; if (!p) return NULL;
  const char *e = p; int depth = 0, str = 0;
  for (; *e; e++) {
    if (str) { if (*e == '\\' && e[1]) e++; else if (*e == '"') str = 0; continue; }
    if (*e == '"') str = 1;
    else if (*e == '{' || *e == '[') depth++;
    else if (*e == '}' || *e == ']') { if (!depth) break; if (!--depth) { e++; break; } }
    else if (*e == ',' && !depth) break;
  }
  size_t n = e - p; char *r = malloc(n + 1); memcpy(r, p, n); r[n] = 0; return r;
}

// a manifest: {"files": {"path": "githash", ...}, "sizes": {"path": bytes, ...}, "base": "...", "commit": "...", "version": N}
typedef struct { char *path; char hash[41]; long long size; } FEnt;
typedef struct { FEnt *e; int n; long long total; char base[600], commit[64]; long long version; } Manifest;
static void manifest_free(Manifest *m) { for (int i = 0; i < m->n; i++) free(m->e[i].path); free(m->e); memset(m, 0, sizeof *m); }
static int manifest_parse(const char *j, Manifest *m) {
  memset(m, 0, sizeof *m);
  if (!j) return 0;
  json_str(j, "base", m->base, sizeof m->base); json_str(j, "commit", m->commit, sizeof m->commit);
  m->version = json_num(j, "version", 0);
  const char *p = json_key(j, "files"); if (!p || *p != '{') return 0;
  p++;
  int cap = 256; m->e = malloc(cap * sizeof(FEnt));
  char path[1024], hash[64];
  for (;;) {
    while (*p && strchr(" \t\r\n,", *p)) p++;
    if (*p != '"') break;
    if (!(p = jstring(p, path, sizeof path))) return 0;
    while (*p && strchr(" \t\r\n:", *p)) p++;
    if (!(p = jstring(p, hash, sizeof hash))) return 0;
    if (m->n == cap) { cap *= 2; m->e = realloc(m->e, cap * sizeof(FEnt)); }
    m->e[m->n].path = _strdup(path); strncpy(m->e[m->n].hash, hash, 40); m->e[m->n].hash[40] = 0; m->e[m->n].size = 0; m->n++;
  }
  const char *s = json_key(j, "sizes");
  if (s && *s == '{') {
    s++;
    for (;;) {
      while (*s && strchr(" \t\r\n,", *s)) s++;
      if (*s != '"') break;
      if (!(s = jstring(s, path, sizeof path))) break;
      while (*s && strchr(" \t\r\n:", *s)) s++;
      long long v = _atoi64(s);
      while (*s && (isdigit((unsigned char)*s) || *s == '-')) s++;
      for (int i = 0; i < m->n; i++) if (!strcmp(m->e[i].path, path)) { m->e[i].size = v; break; }
    }
  }
  for (int i = 0; i < m->n; i++) m->total += m->e[i].size;
  return 1;
}
static const char *manifest_hash(const Manifest *m, const char *rel) {
  for (int i = 0; i < m->n; i++) if (!strcmp(m->e[i].path, rel)) return m->e[i].hash;
  return NULL;
}

// git's hash of a file: sha1("blob <size>\0" + bytes). The same hash GitHub shows, so every download can be checked.
static void git_hash(const void *data, DWORD n, char out[41]) {
  BCRYPT_ALG_HANDLE alg; BCRYPT_HASH_HANDLE h; UCHAR d[20]; char head[32];
  int hn = snprintf(head, sizeof head, "blob %lu", (unsigned long)n) + 1;
  out[0] = 0;
  if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, NULL, 0)) return;
  if (!BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0)) {
    BCryptHashData(h, (PUCHAR)head, hn, 0); BCryptHashData(h, (PUCHAR)data, n, 0);
    BCryptFinishHash(h, d, 20, 0); BCryptDestroyHash(h);
    for (int i = 0; i < 20; i++) sprintf(out + i * 2, "%02x", d[i]);
  }
  BCryptCloseAlgorithmProvider(alg, 0);
}
static int file_matches(const WCHAR *p, const char *want) {
  DWORD n; char *b = read_file(p, &n), h[41]; if (!b) return 0;
  git_hash(b, n, h); free(b); return !strcmp(h, want);
}

// ================================================================================================ the pack (launcher page glued onto the .exe)
// [file bytes...][index][8-byte magic "RDPAK001"][u64 index offset]; index = u32 count, then u16 len, path, u64 off, u64 size
typedef struct { char *path; unsigned long long off, size; } PakEntry;
static HANDLE g_pakFile = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_pakLock;
static PakEntry *g_pak; static int g_pakN;
static int read_at(unsigned long long off, void *buf, unsigned long long n) {
  EnterCriticalSection(&g_pakLock);
  LARGE_INTEGER li; li.QuadPart = off;
  int ok = SetFilePointerEx(g_pakFile, li, NULL, FILE_BEGIN);
  for (unsigned long long done = 0; ok && done < n;) {
    DWORD want = (DWORD)((n - done) > (1u << 30) ? (1u << 30) : (n - done)), got = 0;
    ok = ReadFile(g_pakFile, (char *)buf + done, want, &got, NULL) && got == want; done += got;
  }
  LeaveCriticalSection(&g_pakLock);
  return ok;
}
static int pak_open(void) {
  g_pakFile = CreateFileW(g_self, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, NULL);
  if (g_pakFile == INVALID_HANDLE_VALUE) return 0;
  LARGE_INTEGER sz; GetFileSizeEx(g_pakFile, &sz);
  unsigned char foot[16]; unsigned long long idx;
  if (sz.QuadPart < 16 || !read_at(sz.QuadPart - 16, foot, 16) || memcmp(foot, "RDPAK001", 8)) return 0;
  memcpy(&idx, foot + 8, 8);
  unsigned long long ilen = sz.QuadPart - 16 - idx;
  unsigned char *b = malloc(ilen), *p = b;
  if (!read_at(idx, b, ilen)) return 0;
  memcpy(&g_pakN, p, 4); p += 4;
  g_pak = calloc(g_pakN, sizeof(PakEntry));
  for (int i = 0; i < g_pakN; i++) {
    unsigned short n; memcpy(&n, p, 2); p += 2;
    g_pak[i].path = malloc(n + 1); memcpy(g_pak[i].path, p, n); g_pak[i].path[n] = 0; p += n;
    memcpy(&g_pak[i].off, p, 8); p += 8; memcpy(&g_pak[i].size, p, 8); p += 8;
  }
  free(b);
  return 1;
}
static PakEntry *pak_find(const char *rel) {
  for (int i = 0; i < g_pakN; i++) if (!strcmp(g_pak[i].path, rel)) return &g_pak[i];
  return NULL;
}
static char *pak_read(const PakEntry *e) {
  char *b = malloc(e->size + 1);
  if (!read_at(e->off, b, e->size)) { free(b); return NULL; }
  b[e->size] = 0; return b;
}
// a launcher page file: from the downloaded update if there is one, else from the pack
static IStream *open_asset(const char *rel) {
  if (g_useUpdate) {
    WCHAR p[BIG]; join(p, g_uiDir, rel);
    IStream *s;
    if (exists(p) && SUCCEEDED(SHCreateStreamOnFileEx(p, STGM_READ | STGM_SHARE_DENY_NONE, 0, FALSE, NULL, &s))) return s;
  }
  PakEntry *e = pak_find(rel); if (!e) return NULL;
  char *b = pak_read(e); if (!b) return NULL;
  IStream *s = SHCreateMemStream((const BYTE *)b, (UINT)e->size);
  free(b); return s;
}
static const char *mime(const char *rel) {
  const char *x = strrchr(rel, '.'); if (!x) return "application/octet-stream";
  static const char *T[][2] = {
    { ".html", "text/html; charset=utf-8" }, { ".js", "text/javascript; charset=utf-8" }, { ".css", "text/css; charset=utf-8" },
    { ".json", "application/json" }, { ".svg", "image/svg+xml" }, { ".png", "image/png" }, { ".jpg", "image/jpeg" },
    { ".jpeg", "image/jpeg" }, { ".webp", "image/webp" }, { ".ico", "image/x-icon" }, { ".m4a", "audio/mp4" },
    { ".mp4", "video/mp4" }, { ".woff2", "font/woff2" },
  };
  for (size_t i = 0; i < sizeof T / sizeof *T; i++) if (!_stricmp(x, T[i][0])) return T[i][1];
  return "application/octet-stream";
}
static int url_to_rel(const WCHAR *uri, char *rel, int cap) {
  char *u = narrow(uri); int ok = 0;
  if (!_strnicmp(u, LHOST_A, strlen(LHOST_A))) {
    const char *p = u + strlen(LHOST_A); int n = 0;
    for (; *p && *p != '?' && *p != '#' && n < cap - 12; p++) {
      if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) { char h[3] = { p[1], p[2], 0 }; rel[n++] = (char)strtol(h, NULL, 16); p += 2; }
      else rel[n++] = *p;
    }
    rel[n] = 0;
    if (!n || rel[n - 1] == '/') strcat(rel, "index.html");
    ok = safe_rel(rel);
  }
  free(u); return ok;
}

// ================================================================================================ talking to the page
// Worker threads hand finished JSON to the window thread, which passes it to the page.
static void post_json(Buf *b) { if (g_win) PostMessageW(g_win, WM_JSON, 0, (LPARAM)b->s); else free(b->s); b->s = NULL; }
static void post_simple(const char *fmt, ...) {
  Buf b = { 0 }; va_list a; va_start(a, fmt); char tmp[2048]; vsnprintf(tmp, sizeof tmp, fmt, a); va_end(a);
  b_add(&b, "%s", tmp); post_json(&b);
}

// ================================================================================================ HTTP (WinHTTP)
static HINTERNET http_session(void) {
  HINTERNET s = WinHttpOpen(L"RuffDogsLauncher/" APP_VERSION, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!s) s = WinHttpOpen(L"RuffDogsLauncher/" APP_VERSION, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (s) { DWORD p = WINHTTP_PROTOCOL_FLAG_HTTP2; WinHttpSetOption(s, WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL, &p, sizeof p); }
  return s;
}
static int http_get(HINTERNET s, const char *url, int timeout, char **out, DWORD *len, const WCHAR *headers) {
  WCHAR *w = widen(url), host[256], path[4096], extra[1024];
  URL_COMPONENTS uc = { sizeof uc };
  uc.lpszHostName = host; uc.dwHostNameLength = 256; uc.lpszUrlPath = path; uc.dwUrlPathLength = 4096;
  uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = 1024;
  int ok = 0; HINTERNET c = NULL, r = NULL;
  *out = NULL; *len = 0;
  if (!s || !WinHttpCrackUrl(w, 0, 0, &uc)) goto done;
  wcsncat(path, extra, 4095 - wcslen(path));
  WinHttpSetTimeouts(s, timeout, timeout, timeout, timeout);
  c = WinHttpConnect(s, host, uc.nPort, 0); if (!c) goto done;
  r = WinHttpOpenRequest(c, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
  if (!r || !WinHttpSendRequest(r, headers ? headers : WINHTTP_NO_ADDITIONAL_HEADERS, headers ? (DWORD)-1L : 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(r, NULL)) goto done;
  DWORD code = 0, cl = sizeof code;
  WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &cl, WINHTTP_NO_HEADER_INDEX);
  if (code != 200) goto done;
  DWORD cap = 1 << 16, n = 0, got; char *b = malloc(cap);
  for (;;) {
    if (n + 65536 + 1 > cap) { cap *= 2; b = realloc(b, cap); }
    if (!WinHttpReadData(r, b + n, 65536, &got)) { free(b); goto done; }
    if (!got) break;
    n += got;
  }
  b[n] = 0; *out = b; *len = n; ok = 1;
done:
  if (r) WinHttpCloseHandle(r);
  if (c) WinHttpCloseHandle(c);
  free(w); return ok;
}
// "art/my game/a.png" -> "art/my%20game/a.png"
static void url_path(char *out, int cap, const char *base, const char *rel) {
  int k = snprintf(out, cap, "%s", base);
  for (const unsigned char *c = (const unsigned char *)rel; *c && k < cap - 4; c++)
    k += (isalnum(*c) || strchr("-._~/", *c)) ? snprintf(out + k, cap - k, "%c", *c) : snprintf(out + k, cap - k, "%%%02X", *c);
}
// GitHub's raw file server caches files for 5 minutes, so read from the newest commit's (never stale) address instead
static void fresh_base(HINTERNET s, const char *repo, const char *branch, const char *sub, char *base, int cap) {
  snprintf(base, cap, "https://raw.githubusercontent.com/%s/%s/%s", repo, branch, sub);
  char url[512], *sha; DWORD n;
  snprintf(url, sizeof url, "https://api.github.com/repos/%s/commits/%s", repo, branch);
  if (http_get(s, url, 8000, &sha, &n, L"Accept: application/vnd.github.sha\r\n")) {
    if (n >= 40 && strspn(sha, "0123456789abcdef") >= 40) { sha[40] = 0; snprintf(base, cap, "https://raw.githubusercontent.com/%s/%s/%s", repo, sha, sub); }
    free(sha);
  }
}

// ================================================================================================ launcher self-update (the page)
static DWORD WINAPI ui_update_thread(void *arg) {
  (void)arg;
  Manifest shipped, have, remote, *cur = &shipped;
  PakEntry *pv = pak_find("version.json");
  char *t = pv ? pak_read(pv) : NULL; manifest_parse(t, &shipped); free(t);
  WCHAR vp[BIG]; join(vp, g_uiDir, "version.json");
  t = read_file(vp, NULL); manifest_parse(t, &have); free(t);
  if (have.version > shipped.version) { cur = &have; g_useUpdate = 1; }

  HINTERNET s = http_session();
  char base[700], url[1200], *body; DWORD blen;
  fresh_base(s, g_repo, g_branch, g_uiPath, base, sizeof base);
  snprintf(url, sizeof url, "%sversion.json", base);
  if (!http_get(s, url, 8000, &body, &blen, NULL)) goto out;
  if (!manifest_parse(body, &remote) || !remote.n || remote.version <= cur->version) { free(body); goto out; }

  remove_tree(g_uiNext);
  int ok = 1;
  for (int i = 0; i < remote.n && ok; i++) {
    const char *rel = remote.e[i].path, *want = remote.e[i].hash;
    if (!safe_rel(rel)) continue;
    WCHAR dst[BIG]; join(dst, g_uiNext, rel);
    char *data = NULL, h[41]; DWORD n = 0;
    const char *old = manifest_hash(cur, rel);
    if (old && !strcmp(old, want)) {                     // unchanged: reuse the copy we already have
      if (g_useUpdate) { WCHAR src[BIG]; join(src, g_uiDir, rel); data = read_file(src, &n); }
      if (!data) { PakEntry *e = pak_find(rel); if (e && (data = pak_read(e))) n = (DWORD)e->size; }
      if (data) { git_hash(data, n, h); if (strcmp(h, want)) { free(data); data = NULL; } }
    }
    if (!data) {
      url_path(url, sizeof url, base, rel);
      if (!http_get(s, url, 60000, &data, &n, NULL)) { ok = 0; break; }
      git_hash(data, n, h);
      if (strcmp(h, want)) { free(data); ok = 0; break; }
    }
    ok = write_file(dst, data, n); free(data);
  }
  if (ok) { WCHAR mv[BIG]; join(mv, g_uiNext, "version.json"); ok = write_file(mv, body, blen); }
  free(body);
  if (!ok) { remove_tree(g_uiNext); goto out; }
  InterlockedExchange(&g_useUpdate, 0);
  remove_tree(g_uiDir);
  if (MoveFileExW(g_uiNext, g_uiDir, 0)) { InterlockedExchange(&g_useUpdate, 1); post_simple("{\"t\":\"ui-updated\"}"); }
  else remove_tree(g_uiNext);
out:
  if (s) WinHttpCloseHandle(s);
  return 0;
}

// ================================================================================================ the game library
// installs.txt lists where each game lives ("id<TAB>folder" per line); each folder holds ruffdogs-game.json
typedef struct { char id[64]; WCHAR dir[BIG]; } Inst;
static Inst g_inst[256]; static int g_instN;
static CRITICAL_SECTION g_instLock;
static void inst_save(void) {
  Buf b = { 0 };
  for (int i = 0; i < g_instN; i++) { char *d = narrow(g_inst[i].dir); b_add(&b, "%s\t%s\n", g_inst[i].id, d); free(d); }
  WCHAR p[BIG]; _snwprintf(p, BIG, L"%ls\\installs.txt", g_data);
  write_file(p, b.s ? b.s : "", (DWORD)b.n); free(b.s);
}
static void inst_load(void) {
  WCHAR p[BIG]; _snwprintf(p, BIG, L"%ls\\installs.txt", g_data);
  char *t = read_file(p, NULL); g_instN = 0; if (!t) return;
  for (char *line = strtok(t, "\r\n"); line && g_instN < 256; line = strtok(NULL, "\r\n")) {
    char *tab = strchr(line, '\t'); if (!tab) continue; *tab = 0;
    Inst *in = &g_inst[g_instN]; strncpy(in->id, line, 63); in->id[63] = 0;
    WCHAR *w = widen(tab + 1); wcsncpy(in->dir, w, BIG - 1); in->dir[BIG - 1] = 0; free(w);
    WCHAR rec[BIG]; _snwprintf(rec, BIG, L"%ls\\ruffdogs-game.json", in->dir);
    if (exists(rec)) g_instN++;                          // a folder someone deleted by hand is simply not installed any more
  }
  free(t);
}
static Inst *inst_find(const char *id) { for (int i = 0; i < g_instN; i++) if (!strcmp(g_inst[i].id, id)) return &g_inst[i]; return NULL; }
static void inst_set(const char *id, const WCHAR *dir) {
  EnterCriticalSection(&g_instLock);
  Inst *in = inst_find(id);
  if (!in && g_instN < 256) { in = &g_inst[g_instN++]; strncpy(in->id, id, 63); in->id[63] = 0; }
  if (in) wcsncpy(in->dir, dir, BIG - 1);
  inst_save(); LeaveCriticalSection(&g_instLock);
}
static void inst_del(const char *id) {
  EnterCriticalSection(&g_instLock);
  for (int i = 0; i < g_instN; i++) if (!strcmp(g_inst[i].id, id)) { g_inst[i] = g_inst[--g_instN]; break; }
  inst_save(); LeaveCriticalSection(&g_instLock);
}
static void installed_json(Buf *b, const Inst *in) {
  WCHAR rec[BIG]; _snwprintf(rec, BIG, L"%ls\\ruffdogs-game.json", in->dir);
  char *j = read_file(rec, NULL), commit[64] = "", when[40] = "";
  json_str(j, "commit", commit, sizeof commit); json_str(j, "installedAt", when, sizeof when);
  char eng[32] = ""; json_str(j, "engine", eng, sizeof eng);
  if (strcmp(eng, ENGINE_VER)) commit[0] = 0;           // its .exe is from an older launcher: show it as needing an update, which rebuilds the .exe
  b_add(b, "{\"commit\":"); b_str(b, commit); b_add(b, ",\"dir\":"); b_wstr(b, in->dir);
  b_add(b, ",\"size\":%lld,\"installedAt\":", json_num(j, "size", 0)); b_str(b, when); b_add(b, "}");
  free(j);
}

static void settings_save(void) {
  WCHAR p[BIG]; _snwprintf(p, BIG, L"%ls\\settings.txt", g_data);
  char *l = narrow(g_lib); Buf b = { 0 }; b_add(&b, "library=%s\n", l); free(l);
  write_file(p, b.s, (DWORD)b.n); free(b.s);
}
static void settings_load(void) {
  WCHAR p[BIG]; _snwprintf(p, BIG, L"%ls\\settings.txt", g_data);
  char *t = read_file(p, NULL), *l = t ? strstr(t, "library=") : NULL;
  if (l) { l += 8; char *e = strpbrk(l, "\r\n"); if (e) *e = 0; WCHAR *w = widen(l); wcsncpy(g_lib, w, BIG - 1); free(w); }
  free(t);
  if (g_lib[0] && SHCreateDirectoryExW(NULL, g_lib, NULL) != ERROR_ACCESS_DENIED && exists(g_lib)) return;
  // like Steam: a plain folder at the top of the system drive, or inside the user's own folders if that's not allowed
  WCHAR drive[8] = L"C:"; GetEnvironmentVariableW(L"SystemDrive", drive, 8);
  _snwprintf(g_lib, BIG, L"%ls\\Ruff Dogs Games", drive);
  SHCreateDirectoryExW(NULL, g_lib, NULL);
  if (!exists(g_lib)) { _snwprintf(g_lib, BIG, L"%ls\\Games", g_data); SHCreateDirectoryExW(NULL, g_lib, NULL); }
}
static void library_json(Buf *b) {
  ULARGE_INTEGER fr = { 0 }, tot = { 0 };
  GetDiskFreeSpaceExW(g_lib, &fr, &tot, NULL);
  b_add(b, "{\"dir\":"); b_wstr(b, g_lib); b_add(b, ",\"free\":%llu,\"total\":%llu,\"sep\":\"\\\\\"}", fr.QuadPart, tot.QuadPart);
}

static HANDLE g_proc; static char g_runId[64]; static ULONGLONG g_runStart;
static void send_state(void) {
  Buf b = { 0 };
  b_add(&b, "{\"t\":\"state\",\"app\":{\"version\":\"" APP_VERSION "\",\"platform\":\"windows\"},\"installed\":{");
  EnterCriticalSection(&g_instLock);
  for (int i = 0; i < g_instN; i++) { if (i) b_add(&b, ","); b_str(&b, g_inst[i].id); b_add(&b, ":"); installed_json(&b, &g_inst[i]); }
  LeaveCriticalSection(&g_instLock);
  b_add(&b, "},\"running\":"); if (g_proc) b_str(&b, g_runId); else b_add(&b, "null");
  b_add(&b, ",\"library\":"); library_json(&b);
  b_add(&b, ",\"autoplay\":"); if (g_autoplay[0]) b_str(&b, g_autoplay); else b_add(&b, "null");
  b_add(&b, ",\"accountServer\":"); b_str(&b, g_accountServer);
  b_add(&b, "}");
  g_autoplay[0] = 0;
  post_json(&b);
}

// ------------------------------------------------------------------------------------------------ shortcuts + icons
static int make_link(const WCHAR *lnk, const WCHAR *target, const WCHAR *args, const WCHAR *icon, const WCHAR *workdir, const WCHAR *desc) {
  IShellLinkW *sl; IPersistFile *pf; int ok = 0;
  if (FAILED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&sl))) return 0;
  IShellLinkW_SetPath(sl, target);
  if (args) IShellLinkW_SetArguments(sl, args);
  if (icon) IShellLinkW_SetIconLocation(sl, icon, 0);
  if (workdir) IShellLinkW_SetWorkingDirectory(sl, workdir);
  if (desc) IShellLinkW_SetDescription(sl, desc);
  make_parent(lnk);
  if (SUCCEEDED(IShellLinkW_QueryInterface(sl, &IID_IPersistFile, (void **)&pf))) { ok = SUCCEEDED(IPersistFile_Save(pf, lnk, TRUE)); IPersistFile_Release(pf); }
  IShellLinkW_Release(sl); return ok;
}
static void link_paths(const WCHAR *name, WCHAR *desk, WCHAR *menu) {
  WCHAR d[MAX_PATH];
  if (desk) { SHGetFolderPathW(NULL, CSIDL_DESKTOPDIRECTORY, NULL, 0, d); _snwprintf(desk, BIG, L"%ls\\%ls.lnk", d, name); }
  if (menu) { SHGetFolderPathW(NULL, CSIDL_PROGRAMS, NULL, 0, d); _snwprintf(menu, BIG, L"%ls\\Ruff Dogs\\%ls.lnk", d, name); }
}
// a game's shortcuts point at the game's own .exe in its folder ("Open file location" lands there); the game checks
// for an update itself when it starts and hands over to the launcher if there is one (see game_needs_update)
static void game_shortcuts(const char *id, const WCHAR *name, const WCHAR *dir, int desktop) {
  (void)id;
  WCHAR exe[BIG], desk[BIG], menu[BIG];
  _snwprintf(exe, BIG, L"%ls\\%ls.exe", dir, name);
  link_paths(name, desk, menu);
  make_link(menu, exe, NULL, exe, dir, name);
  if (desktop) make_link(desk, exe, NULL, exe, dir, name);
}
// shortcuts made by older launchers pointed at the launcher itself: re-point any that exist at the game's own .exe
static void refresh_shortcuts(void) {
  for (int i = 0; i < g_instN; i++) {
    WCHAR rec[BIG]; _snwprintf(rec, BIG, L"%ls\\ruffdogs-game.json", g_inst[i].dir);
    char *j = read_file(rec, NULL), exe[128] = ""; json_str(j, "exe", exe, sizeof exe); free(j);
    if (!exe[0]) continue;
    WCHAR *w = widen(exe), desk[BIG], menu[BIG], target[BIG];
    link_paths(w, desk, menu); _snwprintf(target, BIG, L"%ls\\%ls.exe", g_inst[i].dir, w);
    if (exists(menu)) make_link(menu, target, NULL, target, g_inst[i].dir, w);
    if (exists(desk)) make_link(desk, target, NULL, target, g_inst[i].dir, w);
    free(w);
  }
}
// writes a .ico file into an .exe as its icon (resource group 1, which Explorer and the taskbar show)
static int set_exe_icon(const WCHAR *exe, const unsigned char *ico, DWORD n) {
  if (n < 6 || ico[2] != 1 || ico[3] != 0) return 0;
  int count = ico[4] | (ico[5] << 8);
  if (!count || n < 6 + 16u * count) return 0;
  unsigned char *grp = calloc(1, 6 + 14 * count);
  memcpy(grp, ico, 6);
  HANDLE u = BeginUpdateResourceW(exe, FALSE); if (!u) { free(grp); return 0; }
  WORD lang = MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL);
  for (int i = 0; i < count; i++) {
    const unsigned char *e = ico + 6 + 16 * i;
    DWORD bytes, off; memcpy(&bytes, e + 8, 4); memcpy(&off, e + 12, 4);
    if (off + bytes > n) { EndUpdateResourceW(u, TRUE); free(grp); return 0; }
    UpdateResourceW(u, RT_ICON, MAKEINTRESOURCEW(i + 1), lang, (void *)(ico + off), bytes);
    unsigned char *g = grp + 6 + 14 * i;
    memcpy(g, e, 12);                                    // width, height, colours, reserved, planes, bit count, bytes
    g[12] = (unsigned char)(i + 1); g[13] = 0;            // resource id
  }
  UpdateResourceW(u, RT_GROUP_ICON, MAKEINTRESOURCEW(1), lang, grp, 6 + 14 * count);
  free(grp);
  return EndUpdateResourceW(u, FALSE);
}

// ------------------------------------------------------------------------------------------------ install / update
typedef struct { char id[64], title[128], commit[64], manifest[700], icon[700]; int shortcut; } Job;
static volatile LONG g_jobBusy, g_cancel;
typedef struct {
  Manifest *m; int *todo; LONG ntodo; volatile LONG next, fail, files; volatile LONG64 bytes; LONG64 total;
  WCHAR stage[BIG]; char id[64], err[300]; ULONGLONG lastPost; int baseFiles;
} Dl;
static void progress(Dl *d, const char *phase, int force) {
  ULONGLONG now = GetTickCount64();
  if (!force && now - d->lastPost < 120) return;
  d->lastPost = now;
  Buf b = { 0 };
  b_add(&b, "{\"t\":\"progress\",\"id\":"); b_str(&b, d->id);
  b_add(&b, ",\"phase\":\"%s\",\"done\":%ld,\"total\":%d,\"bytes\":%lld,\"totalBytes\":%lld}", phase, (long)d->files, d->m ? d->m->n : 0, (long long)d->bytes, (long long)d->total);
  post_json(&b);
}
static DWORD WINAPI dl_worker(void *arg) {
  Dl *d = arg; HINTERNET s = http_session(); char url[1500];
  for (;;) {
    LONG i = InterlockedIncrement(&d->next) - 1;
    if (i >= d->ntodo || d->fail || g_cancel) break;
    FEnt *e = &d->m->e[d->todo[i]];
    char *data = NULL, h[41]; DWORD n = 0; int ok = 0;
    url_path(url, sizeof url, d->m->base, e->path);
    for (int attempt = 0; attempt < 3 && !ok && !g_cancel; attempt++) {
      if (http_get(s, url, 60000, &data, &n, NULL)) { git_hash(data, n, h); ok = !strcmp(h, e->hash); if (!ok) { free(data); data = NULL; } }
      if (!ok) Sleep(600 * (attempt + 1));
    }
    if (!ok) { if (!InterlockedExchange(&d->fail, 1)) snprintf(d->err, sizeof d->err, "Couldn't download %s", e->path); break; }
    WCHAR dst[BIG]; join(dst, d->stage, e->path);
    if (!write_file(dst, data, n)) { free(data); if (!InterlockedExchange(&d->fail, 1)) snprintf(d->err, sizeof d->err, "Couldn't save %s. Is the drive full?", e->path); break; }
    free(data);
    InterlockedIncrement(&d->files); InterlockedAdd64(&d->bytes, n);
    progress(d, "downloading", 0);
  }
  if (s) WinHttpCloseHandle(s);
  return 0;
}
static void job_result(const char *id, const char *phase, const char *err, const WCHAR *dir) {
  Buf b = { 0 };
  b_add(&b, "{\"t\":\"progress\",\"id\":"); b_str(&b, id); b_add(&b, ",\"phase\":\"%s\"", phase);
  if (err) { b_add(&b, ",\"error\":"); b_str(&b, err); }
  if (dir) { Inst tmp = { 0 }; wcsncpy(tmp.dir, dir, BIG - 1); b_add(&b, ",\"installed\":"); installed_json(&b, &tmp); }
  b_add(&b, "}"); post_json(&b);
}
// like http_get, but https://launcher.ruffdogs/... comes from the launcher's own page files
static int fetch_any(HINTERNET s, const char *url, int timeout, char **out, DWORD *len) {
  if (!_strnicmp(url, LHOST_A, strlen(LHOST_A))) {
    char rel[1024]; WCHAR *w = widen(url); int ok = url_to_rel(w, rel, sizeof rel); free(w);
    *out = NULL; *len = 0; if (!ok) return 0;
    if (g_useUpdate) { WCHAR p[BIG]; join(p, g_uiDir, rel); if ((*out = read_file(p, len))) return 1; }
    PakEntry *e = pak_find(rel); if (!e || !(*out = pak_read(e))) return 0;
    *len = (DWORD)e->size; return 1;
  }
  return http_get(s, url, timeout, out, len, NULL);
}
static DWORD WINAPI install_thread(void *arg) {
  Job *job = arg; Dl d = { 0 }; Manifest m = { 0 }, old = { 0 };
  char *body = NULL; DWORD blen = 0; const char *err = NULL;
  CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
  strcpy(d.id, job->id); d.m = &m;
  progress(&d, "manifest", 1);
  HINTERNET s = http_session();
  if (!fetch_any(s, job->manifest, 20000, &body, &blen) || !manifest_parse(body, &m) || !m.n || !m.base[0]) { err = "Couldn't get the game's file list. Check your internet connection."; goto fail; }

  WCHAR name[128], dest[BIG], gameDir[BIG], rec[BIG];
  safe_name(job->title, name, 128);
  Inst *have = inst_find(job->id);
  if (have) wcscpy(dest, have->dir); else _snwprintf(dest, BIG, L"%ls\\%ls", g_lib, name);
  _snwprintf(gameDir, BIG, L"%ls\\game", dest);
  _snwprintf(d.stage, BIG, L"%ls\\.download", dest);
  _snwprintf(rec, BIG, L"%ls\\ruffdogs-files.json", dest);
  char *ot = read_file(rec, NULL); manifest_parse(ot, &old); free(ot);
  remove_tree(d.stage);
  if (SHCreateDirectoryExW(NULL, d.stage, NULL) != ERROR_SUCCESS && !exists(d.stage)) { err = "Couldn't make the game folder. Try another library folder in Settings."; goto fail; }

  // only download files that are missing or changed
  d.todo = malloc(sizeof(int) * m.n); d.total = m.total;
  for (int i = 0; i < m.n; i++) {
    if (!safe_rel(m.e[i].path)) continue;
    WCHAR p[BIG]; join(p, gameDir, m.e[i].path);
    const char *was = manifest_hash(&old, m.e[i].path);
    int same = exists(p) && ((was && !strcmp(was, m.e[i].hash)) || file_matches(p, m.e[i].hash));
    if (same) { d.files++; d.bytes += m.e[i].size; } else d.todo[d.ntodo++] = i;
  }
  progress(&d, "downloading", 1);
  HANDLE th[4]; int nth = d.ntodo < 4 ? d.ntodo : 4;
  for (int i = 0; i < nth; i++) th[i] = CreateThread(NULL, 0, dl_worker, &d, 0, NULL);
  if (nth) WaitForMultipleObjects(nth, th, TRUE, INFINITE);
  for (int i = 0; i < nth; i++) CloseHandle(th[i]);
  if (g_cancel) { remove_tree(d.stage); job_result(job->id, "cancelled", NULL, NULL); goto done; }
  if (d.fail) { err = d.err; remove_tree(d.stage); goto fail; }

  // everything arrived and checked out: move it into place, then drop files the new version doesn't have
  progress(&d, "finishing", 1);
  for (int k = 0; k < d.ntodo; k++) {
    const char *rel = m.e[d.todo[k]].path; WCHAR from[BIG], to[BIG];
    join(from, d.stage, rel); join(to, gameDir, rel); make_parent(to);
    if (!MoveFileExW(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) { err = "A game file is in use. Close the game and try again."; remove_tree(d.stage); goto fail; }
  }
  for (int i = 0; i < old.n; i++)
    if (!manifest_hash(&m, old.e[i].path) && safe_rel(old.e[i].path)) { WCHAR p[BIG]; join(p, gameDir, old.e[i].path); DeleteFileW(p); }
  remove_tree(d.stage);

  // the game's own .exe: a copy of this program with the game's icon
  WCHAR exe[BIG], tmp[BIG];
  _snwprintf(exe, BIG, L"%ls\\%ls.exe", dest, name); _snwprintf(tmp, BIG, L"%ls\\.exe-new", dest);
  if (CopyFileW(g_self, tmp, FALSE)) {
    char *ico; DWORD in;
    if (job->icon[0] && fetch_any(s, job->icon, 20000, &ico, &in)) { set_exe_icon(tmp, (unsigned char *)ico, in); free(ico); }
    if (!MoveFileExW(tmp, exe, MOVEFILE_REPLACE_EXISTING)) DeleteFileW(tmp);   // still running? keep the old one
  }
  SYSTEMTIME st; GetSystemTime(&st);
  Buf r = { 0 };
  b_add(&r, "{\"id\":"); b_str(&r, job->id); b_add(&r, ",\"title\":"); b_str(&r, job->title);
  b_add(&r, ",\"engine\":\"" ENGINE_VER "\",\"exe\":"); b_wstr(&r, name); b_add(&r, ",\"commit\":"); b_str(&r, m.commit[0] ? m.commit : job->commit);
  b_add(&r, ",\"size\":%lld,\"installedAt\":\"%04d-%02d-%02dT%02d:%02d:%02dZ\"}", m.total, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
  WCHAR gj[BIG]; _snwprintf(gj, BIG, L"%ls\\ruffdogs-game.json", dest);
  write_file(gj, r.s, (DWORD)r.n); free(r.s);
  write_file(rec, body, blen);
  SetFileAttributesW(rec, FILE_ATTRIBUTE_HIDDEN);
  inst_set(job->id, dest);
  game_shortcuts(job->id, name, dest, job->shortcut);
  job_result(job->id, "done", NULL, dest);
  goto done;
fail:
  job_result(job->id, "failed", err, NULL);
done:
  if (s) WinHttpCloseHandle(s);
  free(body); free(d.todo); manifest_free(&m); manifest_free(&old); free(job);
  InterlockedExchange(&g_jobBusy, 0);
  CoUninitialize();
  return 0;
}

static void uninstall(const char *id) {
  Inst *in = inst_find(id); if (!in) { send_state(); return; }
  if (g_proc && !strcmp(g_runId, id)) { post_simple("{\"t\":\"error\",\"message\":\"Close the game before uninstalling it.\"}"); return; }
  WCHAR dir[BIG], rec[BIG]; wcscpy(dir, in->dir);
  _snwprintf(rec, BIG, L"%ls\\ruffdogs-game.json", dir);
  char *j = read_file(rec, NULL), exe[128] = "";
  json_str(j, "exe", exe, sizeof exe); free(j);
  if (exists(rec)) remove_tree(dir);                    // only ever a folder we made (it has our ruffdogs-game.json)
  if (exe[0]) { WCHAR *w = widen(exe), desk[BIG], menu[BIG]; link_paths(w, desk, menu); DeleteFileW(desk); DeleteFileW(menu); free(w); }
  inst_del(id);
  send_state();
}

// ------------------------------------------------------------------------------------------------ playing
static DWORD WINAPI watch_thread(void *arg) {
  HANDLE p = arg; WaitForSingleObject(p, INFINITE);
  PostMessageW(g_win, WM_GAMEEXIT, 0, 0); return 0;
}
static void play(const char *id) {
  if (g_proc) return;
  Inst *in = inst_find(id); if (!in) return;
  WCHAR rec[BIG]; _snwprintf(rec, BIG, L"%ls\\ruffdogs-game.json", in->dir);
  char *j = read_file(rec, NULL), exe[128] = ""; json_str(j, "exe", exe, sizeof exe); free(j);
  WCHAR *w = widen(exe), path[BIG], cmd[BIG + 32];
  _snwprintf(path, BIG, L"%ls\\%ls.exe", in->dir, w); free(w);
  _snwprintf(cmd, BIG + 32, L"\"%ls\" --from-launcher", path);
  STARTUPINFOW si = { sizeof si }; PROCESS_INFORMATION pi;
  if (!exists(path) || !CreateProcessW(path, cmd, NULL, NULL, FALSE, 0, NULL, in->dir, &si, &pi)) {
    post_simple("{\"t\":\"error\",\"message\":\"The game's .exe is missing. Use Manage > Check game files to repair it.\"}"); return;
  }
  CloseHandle(pi.hThread);
  g_proc = pi.hProcess; strncpy(g_runId, id, 63); g_runStart = GetTickCount64();
  Buf b = { 0 }; b_add(&b, "{\"t\":\"running\",\"id\":"); b_str(&b, id); b_add(&b, "}"); post_json(&b);
  CreateThread(NULL, 0, watch_thread, pi.hProcess, 0, NULL);
}
static BOOL CALLBACK close_windows_of(HWND h, LPARAM pid) {
  DWORD p; GetWindowThreadProcessId(h, &p);
  if (p == (DWORD)pid && IsWindowVisible(h)) PostMessageW(h, WM_CLOSE, 0, 0);
  return TRUE;
}
static DWORD WINAPI force_stop(void *arg) { HANDLE p = arg; if (WaitForSingleObject(p, 5000) == WAIT_TIMEOUT) TerminateProcess(p, 0); return 0; }
static void stop_game(void) {
  if (!g_proc) return;
  EnumWindows(close_windows_of, (LPARAM)GetProcessId(g_proc));
  CreateThread(NULL, 0, force_stop, g_proc, 0, NULL);
}

static void pick_folder(void) {
  IFileOpenDialog *d;
  if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&d))) return;
  DWORD o = 0; IFileOpenDialog_GetOptions(d, &o);
  IFileOpenDialog_SetOptions(d, o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
  IFileOpenDialog_SetTitle(d, L"Choose a drive or folder for your Ruff Dogs games");
  if (SUCCEEDED(IFileOpenDialog_Show(d, g_win))) {
    IShellItem *it;
    if (SUCCEEDED(IFileOpenDialog_GetResult(d, &it))) {
      PWSTR p = NULL;
      if (SUCCEEDED(IShellItem_GetDisplayName(it, SIGDN_FILESYSPATH, &p)) && p) {
        WCHAR dir[BIG]; wcsncpy(dir, p, BIG - 1); dir[BIG - 1] = 0; PathRemoveBackslashW(dir);
        // picking a whole drive (or any folder) makes a "Ruff Dogs Games" folder inside it, like a Steam library
        if (_wcsicmp(PathFindFileNameW(dir), L"Ruff Dogs Games")) wcsncat(dir, L"\\Ruff Dogs Games", BIG - wcslen(dir) - 1);
        SHCreateDirectoryExW(NULL, dir, NULL);
        if (exists(dir)) { wcscpy(g_lib, dir); settings_save(); }
        else post_simple("{\"t\":\"error\",\"message\":\"Windows won't let the launcher write there. Pick another folder.\"}");
        CoTaskMemFree(p);
      }
      IShellItem_Release(it);
    }
  }
  IFileOpenDialog_Release(d);
  Buf b = { 0 }; b_add(&b, "{\"t\":\"folder\",\"library\":"); library_json(&b); b_add(&b, "}"); post_json(&b);
}

// messages from the launcher page
static void on_page_message(const char *m) {
  char t[32], id[64]; json_str(m, "t", t, sizeof t); json_str(m, "id", id, sizeof id);
  if (!strcmp(t, "hello")) send_state();
  else if (!strcmp(t, "install")) {
    if (InterlockedCompareExchange(&g_jobBusy, 1, 0)) { post_simple("{\"t\":\"error\",\"message\":\"Another download is still running.\"}"); return; }
    Job *j = calloc(1, sizeof *j);
    strcpy(j->id, id); json_str(m, "title", j->title, sizeof j->title); json_str(m, "commit", j->commit, sizeof j->commit);
    json_str(m, "manifest", j->manifest, sizeof j->manifest); json_str(m, "icon", j->icon, sizeof j->icon);
    j->shortcut = (int)json_num(m, "shortcut", 0);
    if (!j->id[0] || !j->title[0] || strncmp(j->manifest, "https://", 8) || (j->icon[0] && strncmp(j->icon, "https://", 8))) { free(j); g_jobBusy = 0; return; }
    g_cancel = 0;
    CloseHandle(CreateThread(NULL, 0, install_thread, j, 0, NULL));
  }
  else if (!strcmp(t, "cancel")) g_cancel = 1;
  else if (!strcmp(t, "uninstall")) uninstall(id);
  else if (!strcmp(t, "play")) play(id);
  else if (!strcmp(t, "stop")) stop_game();
  else if (!strcmp(t, "folder")) PostMessageW(g_win, WM_PICKFOLDER, 0, 0);
  else if (!strcmp(t, "reveal")) { Inst *in = inst_find(id); if (in) ShellExecuteW(NULL, L"open", in->dir, NULL, NULL, SW_SHOWNORMAL); }
  else if (!strcmp(t, "shortcut")) {
    Inst *in = inst_find(id); if (!in) return;
    WCHAR rec[BIG]; _snwprintf(rec, BIG, L"%ls\\ruffdogs-game.json", in->dir);
    char *j = read_file(rec, NULL), exe[128] = ""; json_str(j, "exe", exe, sizeof exe); free(j);
    WCHAR *w = widen(exe); game_shortcuts(id, w, in->dir, 1); free(w);
  }
  else if (!strcmp(t, "account")) {
    // the games read this to sign you in (OVERDRIVE online uses the same account)
    WCHAR p[BIG]; _snwprintf(p, BIG, L"%ls\\account.json", g_data);
    write_file(p, m, (DWORD)strlen(m));
  }
  else if (!strcmp(t, "open")) {
    char url[1000]; json_str(m, "url", url, sizeof url);
    if (!strncmp(url, "https://", 8)) { WCHAR *w = widen(url); ShellExecuteW(NULL, L"open", w, NULL, NULL, SW_SHOWNORMAL); free(w); }
  }
}

// ================================================================================================ window + web view
static void set_fullscreen(int on) {
  if (on == g_isFs) return;
  if (on) {
    MONITORINFO mi = { sizeof mi };
    GetWindowPlacement(g_win, &g_place);
    GetMonitorInfoW(MonitorFromWindow(g_win, MONITOR_DEFAULTTONEAREST), &mi);
    SetWindowLongPtrW(g_win, GWL_STYLE, WS_POPUP | WS_VISIBLE);
    SetWindowPos(g_win, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                 mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
  } else {
    SetWindowLongPtrW(g_win, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
    SetWindowPlacement(g_win, &g_place);
    SetWindowPos(g_win, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
  }
  g_isFs = on;
}

#include "cefhost.h"

// what a game page gets before its own scripts run: who's playing and, for OVERDRIVE, the launcher's sign-in
// (unless the game already has its own login saved)
static char *game_boot_script(void) {
  Buf b = { 0 };
  b_add(&b, "window.ruffDogs=Object.freeze({launcher:true,game:"); b_str(&b, g_gameId); b_add(&b, ",platform:'PC'});");
  WCHAR p[BIG]; _snwprintf(p, BIG, L"%ls\\account.json", g_data);
  char *j = read_file(p, NULL);
  if (j && !strcmp(g_gameId, "overdrive")) {
    char token[128] = ""; json_str(j, "token", token, sizeof token);
    char *prof = json_raw(j, "profile");
    if (token[0] && strspn(token, "0123456789abcdefABCDEF") == strlen(token)) {
      b_add(&b, "try{if(!localStorage.getItem('overdrive_token')){localStorage.setItem('overdrive_token','%s');", token);
      if (prof && prof[0] == '{') { b_add(&b, "localStorage.setItem('overdrive_profile',"); b_str(&b, prof); b_add(&b, ");"); }
      b_add(&b, "}}catch(e){}");
    }
    free(prof);
  }
  free(j);
  return b.s;
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
  case WM_SIZE: resize_browser(); return 0;
  case WM_GETMINMAXINFO: { MINMAXINFO *mm = (MINMAXINFO *)lp; mm->ptMinTrackSize.x = 960; mm->ptMinTrackSize.y = 600; return 0; }
  case WM_JSON: { char *s = (char *)lp; send_to_page(s); free(s); return 0; }
  case WM_PAGEMSG: { char *s = (char *)lp; if (!g_gameMode && s) on_page_message(s); free(s); return 0; }
  case WM_STATE: send_state(); return 0;
  case WM_STARTWEB: create_browser(); return 0;
  case WM_PICKFOLDER: pick_folder(); return 0;
  case WM_GAMEEXIT: {
    if (!g_proc) return 0;
    long secs = (long)((GetTickCount64() - g_runStart) / 1000);
    CloseHandle(g_proc); g_proc = NULL;
    Buf b = { 0 }; b_add(&b, "{\"t\":\"stopped\",\"id\":"); b_str(&b, g_runId); b_add(&b, ",\"seconds\":%ld}", secs); post_json(&b);
    if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
    return 0;
  }
  case WM_COPYDATA: {                                   // a second launch (a desktop shortcut) asks us to start a game
    COPYDATASTRUCT *cd = (COPYDATASTRUCT *)lp;
    if (cd->dwData == 0x52440001 && cd->cbData < sizeof g_autoplay) { memcpy(g_autoplay, cd->lpData, cd->cbData); g_autoplay[cd->cbData] = 0; send_state(); }
    if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
    SetForegroundWindow(h);
    return TRUE;
  }
  case WM_PAINT: {                                     // shown for a moment before the page loads
    PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); RECT rc; GetClientRect(h, &rc);
    HBRUSH bg = CreateSolidBrush(RGB(10, 11, 14)); FillRect(dc, &rc, bg); DeleteObject(bg);
    HFONT f = CreateFontW(44, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Bahnschrift");
    HGDIOBJ old = SelectObject(dc, f); SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(255, 215, 122));
    WCHAR *t = g_gameMode ? widen(g_gameTitle) : NULL;
    DrawTextW(dc, t ? t : L"RUFF DOGS", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    free(t); SelectObject(dc, old); DeleteObject(f); EndPaint(h, &ps); return 0;
  }
  case WM_ERASEBKGND: return 1;
  case WM_CLOSE:                                        // close the page first; the engine quits once it's gone
    if (g_browser && !g_closing) { g_closing = 1; cef_browser_host_t *bh = g_browser->get_host(g_browser); bh->close_browser(bh, 1); RELEASE(bh); }
    DestroyWindow(h); return 0;
  case WM_DESTROY:
    if (!g_browser) p_cef_quit_message_loop();
    return 0;
  }
  return DefWindowProcW(h, msg, wp, lp);
}

static void load_config(void) {
  PakEntry *e = pak_find("_launcher/config.json");
  char *j = e ? pak_read(e) : NULL;
  WCHAR dir[BIG], beside[BIG];                         // a config.json next to the .exe wins (testing)
  wcscpy(dir, g_self); PathRemoveFileSpecW(dir);
  _snwprintf(beside, BIG, L"%ls\\config.json", dir);
  char *k = read_file(beside, NULL); if (k) { free(j); j = k; }
  if (!j) return;
  char v[300];
  if (json_str(j, "repo", v, sizeof v) && v[0]) strcpy(g_repo, v);
  if (json_str(j, "branch", v, sizeof v) && v[0]) strncpy(g_branch, v, sizeof g_branch - 1);
  if (json_str(j, "uiPath", v, sizeof v) && v[0]) strncpy(g_uiPath, v, sizeof g_uiPath - 1);
  json_str(j, "accountServer", g_accountServer, sizeof g_accountServer);
  g_devtools = (int)json_num(j, "devtools", 0);
  free(j);
}

// First start from Downloads (or anywhere): copy the launcher to %LOCALAPPDATA%\Programs\Ruff Dogs Launcher, add Start menu
// and desktop shortcuts, and run that copy. Later starts (and newer downloads of the launcher) update it in place.
static int self_install(PWSTR cmd) {
  WCHAR progs[MAX_PATH], dir[BIG], dst[BIG];
  DWORD n = sizeof dir;                                   // RuffDogsSetup.exe records the folder the player picked
  if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Ruff Dogs\\Launcher", L"InstallDir", RRF_RT_REG_SZ, NULL, dir, &n) != ERROR_SUCCESS || !dir[0]) {
    SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, progs);
    _snwprintf(dir, BIG, L"%ls\\Programs\\Ruff Dogs Launcher", progs);
  }
  _snwprintf(dst, BIG, L"%ls\\RuffDogsLauncher.exe", dir);
  if (!_wcsicmp(dst, g_self)) return 0;
  SHCreateDirectoryExW(NULL, dir, NULL);
  if (!CopyFileW(g_self, dst, FALSE)) return 0;        // can't install (or it's running): just run from here
  WCHAR desk[BIG], menu[BIG]; link_paths(L"Ruff Dogs Launcher", desk, menu);
  make_link(menu, dst, NULL, dst, dir, L"Ruff Dogs Launcher");
  make_link(desk, dst, NULL, dst, dir, L"Ruff Dogs Launcher");
  WCHAR line[BIG * 2]; _snwprintf(line, BIG * 2, L"\"%ls\" %ls", dst, PathGetArgsW(cmd));
  STARTUPINFOW si = { sizeof si }; PROCESS_INFORMATION pi;
  if (!CreateProcessW(dst, line, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi)) return 0;
  CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
  return 1;
}

// where the launcher is installed (Setup records it; the launcher's own first-run install uses the default)
static void launcher_path(WCHAR *exe) {
  WCHAR dir[BIG]; DWORD n = sizeof dir;
  if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Ruff Dogs\\Launcher", L"InstallDir", RRF_RT_REG_SZ, NULL, dir, &n) != ERROR_SUCCESS || !dir[0]) {
    WCHAR la[MAX_PATH]; SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, la);
    _snwprintf(dir, BIG, L"%ls\\Programs\\Ruff Dogs Launcher", la);
  }
  _snwprintf(exe, BIG, L"%ls\\RuffDogsLauncher.exe", dir);
}
// GAME mode started straight from its shortcut: if the store has a newer version of this game, let the launcher
// update it and start it (returns 1 when it handed over). No internet, or no launcher: just play what's installed.
static int game_needs_update(const char *haveCommit) {
  if (!haveCommit[0]) return 0;
  WCHAR lexe[BIG]; launcher_path(lexe);
  if (!exists(lexe)) return 0;
  HINTERNET s = http_session(); char base[700], url[800], *cat = NULL; DWORD n; int handed = 0;
  fresh_base(s, g_repo, g_branch, g_uiPath, base, sizeof base);
  snprintf(url, sizeof url, "%scatalog.json", base);
  if (http_get(s, url, 5000, &cat, &n, NULL)) {
    char key[96]; snprintf(key, sizeof key, "\"id\": \"%s\"", g_gameId);
    const char *at = strstr(cat, key); char want[64] = "";
    if (at) json_str(at, "commit", want, sizeof want);
    if (want[0] && strcmp(want, haveCommit)) {
      WCHAR line[BIG * 2], *wid = widen(g_gameId), dir[BIG];
      _snwprintf(line, BIG * 2, L"\"%ls\" --play %ls", lexe, wid); free(wid);
      wcscpy(dir, lexe); PathRemoveFileSpecW(dir);
      STARTUPINFOW si = { sizeof si }; PROCESS_INFORMATION pi;
      if (CreateProcessW(lexe, line, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); handed = 1; }
    }
    free(cat);
  }
  if (s) WinHttpCloseHandle(s);
  return handed;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show) {
  (void)prev;
  InitializeCriticalSection(&g_pakLock); InitializeCriticalSection(&g_instLock);
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
  GetModuleFileNameW(NULL, g_self, BIG);
  { int c = cef_start(inst); if (c >= 0) return c; }    // Chromium's helper processes are this same .exe
  WCHAR local[MAX_PATH]; SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, local);
  _snwprintf(g_data, BIG, L"%ls\\Ruff Dogs", local);
  SHCreateDirectoryExW(NULL, g_data, NULL);
  { WCHAR dt[BIG]; _snwprintf(dt, BIG, L"%ls\\devtools", g_data); g_devtools = exists(dt); }   // an empty "devtools" file turns on F12

  // GAME mode: this is <Title>.exe inside an installed game's folder
  WCHAR here[BIG], rec[BIG]; wcscpy(here, g_self); PathRemoveFileSpecW(here);
  _snwprintf(rec, BIG, L"%ls\\ruffdogs-game.json", here);
  char *gj = read_file(rec, NULL);
  if (gj) {
    g_gameMode = 1;
    char commit[64] = "";
    json_str(gj, "id", g_gameId, sizeof g_gameId); json_str(gj, "title", g_gameTitle, sizeof g_gameTitle); json_str(gj, "commit", commit, sizeof commit); free(gj);
    for (char *c = g_gameId; *c; c++) if (!(isalnum((unsigned char)*c) || *c == '-')) *c = '-';
    if (!g_gameId[0]) strcpy(g_gameId, "game");
    _snwprintf(g_gameDir, BIG, L"%ls\\game", here);
    if (!wcsstr(GetCommandLineW(), L"--from-launcher") && pak_open()) { load_config(); if (game_needs_update(commit)) return 0; }
  } else {
    int argc; LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i + 1 < argc; i++) if (!wcscmp(argv[i], L"--play")) { char *s = narrow(argv[i + 1]); strncpy(g_autoplay, s, 63); free(s); }
    LocalFree(argv);
    if (self_install(GetCommandLineW())) return 0;
    // only one launcher: a second start (like a desktop shortcut) hands its game to the first one and quits
    CreateMutexW(NULL, TRUE, L"RuffDogsLauncher.Single");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
      HWND other = FindWindowW(L"RuffDogsLauncher", NULL);
      if (other) { COPYDATASTRUCT cd = { 0x52440001, (DWORD)strlen(g_autoplay), g_autoplay }; SendMessageW(other, WM_COPYDATA, 0, (LPARAM)&cd); }
      return 0;
    }
    if (!pak_open()) { MessageBoxW(NULL, L"This copy of the Ruff Dogs Launcher is damaged. Please download it again.", L"Ruff Dogs", MB_ICONERROR); return 1; }
    load_config();
    _snwprintf(g_uiDir, BIG, L"%ls\\ui", g_data);
    _snwprintf(g_uiNext, BIG, L"%ls\\ui-next", g_data);
    settings_load();
    inst_load();
    refresh_shortcuts();
  }

  if (!cef_init(inst)) return 1;

  WNDCLASSEXW wc = { sizeof wc };
  wc.lpfnWndProc = wndproc; wc.hInstance = inst; wc.lpszClassName = g_gameMode ? L"RuffDogsGame" : L"RuffDogsLauncher";
  wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1)); wc.hIconSm = wc.hIcon;
  wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
  RegisterClassExW(&wc);

  RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
  int ww = wa.right - wa.left, wh = wa.bottom - wa.top;
  int w = ww * (g_gameMode ? 85 : 78) / 100, h = w * 9 / 16; if (h > wh * 85 / 100) { h = wh * 85 / 100; w = h * 16 / 9; }
  WCHAR *title = g_gameMode ? widen(g_gameTitle) : NULL;
  g_win = CreateWindowExW(0, wc.lpszClassName, title ? title : L"Ruff Dogs Launcher", WS_OVERLAPPEDWINDOW, wa.left + (ww - w) / 2, wa.top + (wh - h) / 2, w, h, NULL, NULL, inst, NULL);
  free(title);
  BOOL dark = TRUE; DwmSetWindowAttribute(g_win, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof dark);
  ShowWindow(g_win, show); UpdateWindow(g_win);

  if (!g_gameMode) CloseHandle(CreateThread(NULL, 0, ui_update_thread, NULL, 0, NULL));
  PostMessageW(g_win, WM_STARTWEB, 0, 0);

  p_cef_run_message_loop();                            // the engine runs the Windows message loop too
  p_cef_shutdown();
  CoUninitialize();
  (void)cmd;
  return 0;
}
int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show) { (void)cmd; return wWinMain(inst, prev, GetCommandLineW(), show); }
