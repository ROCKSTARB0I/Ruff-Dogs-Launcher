// Ruff Dogs Setup: the installer for the Ruff Dogs Launcher. One small Windows program with its own art and screens:
// Welcome -> Terms -> Location -> Installing -> Done. The launcher itself rides on the end of this .exe:
//   [setup program][RuffDogsLauncher.exe bytes]["RDSETUP1"][u64 offset][u64 length]
// Installs per-user (no admin prompt) and registers itself in Windows "Installed apps". A copy of this program is left in the
// install folder as "Uninstall Ruff Dogs.exe"; run with --uninstall it removes the launcher (games and saves stay).
#define COBJMACROS
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <windowsx.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BIG 1024
#define W_ 860
#define H_ 540
#define ART_W 320
#define WM_STEP (WM_APP + 1)
#define T_ANIM 1

enum { P_WELCOME, P_TERMS, P_PLACE, P_INSTALL, P_DONE, P_UNINSTALL, P_REMOVING, P_REMOVED, P_ERROR };
enum { B_NONE, B_NEXT, B_BACK, B_CANCEL, B_CLOSE, B_MIN, B_AGREE, B_BROWSE, B_DESK, B_MENU, B_LAUNCH, B_FINISH, B_REMOVE };

static HWND g_win, g_edit;
static HINSTANCE g_inst;
static float S = 1;                                 // DPI scale
static int g_page = P_WELCOME, g_hover, g_down, g_uninstall;
static int g_agree, g_desk = 1, g_menu = 1, g_launch = 1, g_update;
static volatile float g_prog; static volatile int g_doneFlag;
static WCHAR g_status[256] = L"", g_err[512] = L"", g_dir[BIG], g_self[BIG], g_removeDir[BIG];
static GpImage *g_art, *g_word, *g_dog;
static HFONT g_editFont; static HBRUSH g_editBrush;
static float g_anim;                                // seconds since start, for little animations
typedef struct { float x, y, w, h; int id, on; } Hit;
static Hit g_hits[24]; static int g_hitN;

// ---------------------------------------------------------------------------------------------------------- helpers
static int exists(const WCHAR *p) { return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES; }
static GpImage *load_png(int id) {
  HRSRC r = FindResourceW(g_inst, MAKEINTRESOURCEW(id), (LPCWSTR)RT_RCDATA); if (!r) return NULL;
  HGLOBAL h = LoadResource(g_inst, r); DWORD n = SizeofResource(g_inst, r);
  IStream *st = SHCreateMemStream(LockResource(h), n); if (!st) return NULL;
  GpBitmap *b = NULL; GdipCreateBitmapFromStream(st, &b); IStream_Release(st);
  return (GpImage *)b;
}
static void default_dir(WCHAR *out) {
  DWORD n = BIG * sizeof(WCHAR);
  if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Ruff Dogs\\Launcher", L"InstallDir", RRF_RT_REG_SZ, NULL, out, &n) == ERROR_SUCCESS && out[0]) { g_update = 1; return; }
  WCHAR la[MAX_PATH]; SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, la);
  _snwprintf(out, BIG, L"%ls\\Programs\\Ruff Dogs Launcher", la);
}
static void link_paths(WCHAR *desk, WCHAR *menu) {
  WCHAR d[MAX_PATH];
  SHGetFolderPathW(NULL, CSIDL_DESKTOPDIRECTORY, NULL, 0, d); _snwprintf(desk, BIG, L"%ls\\Ruff Dogs Launcher.lnk", d);
  SHGetFolderPathW(NULL, CSIDL_PROGRAMS, NULL, 0, d); _snwprintf(menu, BIG, L"%ls\\Ruff Dogs\\Ruff Dogs Launcher.lnk", d);
}
static int make_link(const WCHAR *lnk, const WCHAR *target, const WCHAR *dir) {
  IShellLinkW *sl; IPersistFile *pf; int ok = 0;
  if (FAILED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&sl))) return 0;
  IShellLinkW_SetPath(sl, target); IShellLinkW_SetIconLocation(sl, target, 0);
  IShellLinkW_SetWorkingDirectory(sl, dir); IShellLinkW_SetDescription(sl, L"Ruff Dogs Launcher");
  WCHAR parent[BIG]; wcscpy(parent, lnk); PathRemoveFileSpecW(parent); SHCreateDirectoryExW(NULL, parent, NULL);
  if (SUCCEEDED(IShellLinkW_QueryInterface(sl, &IID_IPersistFile, (void **)&pf))) { ok = SUCCEEDED(IPersistFile_Save(pf, lnk, TRUE)); IPersistFile_Release(pf); }
  IShellLinkW_Release(sl); return ok;
}
// the launcher bytes carried on the end of this .exe
static unsigned char *payload(DWORD *len) {
  HANDLE f = CreateFileW(g_self, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
  if (f == INVALID_HANDLE_VALUE) return NULL;
  LARGE_INTEGER sz; GetFileSizeEx(f, &sz);
  unsigned char foot[24]; DWORD got; LARGE_INTEGER at; at.QuadPart = sz.QuadPart - 24;
  unsigned char *buf = NULL;
  if (sz.QuadPart > 24 && SetFilePointerEx(f, at, NULL, FILE_BEGIN) && ReadFile(f, foot, 24, &got, NULL) && got == 24 && !memcmp(foot, "RDSETUP1", 8)) {
    unsigned long long off, n; memcpy(&off, foot + 8, 8); memcpy(&n, foot + 16, 8);
    if (off + n <= (unsigned long long)sz.QuadPart && n < 512u * 1024 * 1024) {
      at.QuadPart = off; buf = malloc((size_t)n);
      if (buf && SetFilePointerEx(f, at, NULL, FILE_BEGIN) && ReadFile(f, buf, (DWORD)n, &got, NULL) && got == n) *len = (DWORD)n;
      else { free(buf); buf = NULL; }
    }
  }
  CloseHandle(f); return buf;
}
// asks a running launcher to close and waits for it (it can't be replaced while it runs)
static int close_launcher(void) {
  for (int i = 0; i < 60; i++) {
    HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, L"RuffDogsLauncher.Single");
    if (!m) return 1;
    CloseHandle(m);
    HWND w = FindWindowW(L"RuffDogsLauncher", NULL); if (w) PostMessageW(w, WM_CLOSE, 0, 0);
    Sleep(100);
  }
  return 0;
}
static void step(float p, const WCHAR *s) { g_prog = p; if (s) wcsncpy(g_status, s, 255); PostMessageW(g_win, WM_STEP, 0, 0); }
static void fail(const WCHAR *msg) { wcsncpy(g_err, msg, 511); g_page = P_ERROR; PostMessageW(g_win, WM_STEP, 1, 0); }
static void reg_str(HKEY k, const WCHAR *name, const WCHAR *v) { RegSetValueExW(k, name, 0, REG_SZ, (const BYTE *)v, (DWORD)(wcslen(v) + 1) * sizeof(WCHAR)); }
static void reg_dword(HKEY k, const WCHAR *name, DWORD v) { RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE *)&v, sizeof v); }
#define UNINSTALL_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\RuffDogsLauncher"

// ---------------------------------------------------------------------------------------------------------- install / remove
static DWORD WINAPI install_thread(void *arg) {
  (void)arg; CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
  WCHAR exe[BIG], unin[BIG], tmp[BIG];
  step(0.03f, L"Getting ready\x2026"); Sleep(350);
  DWORD n = 0; unsigned char *data = payload(&n);
  if (!data) { fail(L"This setup file is damaged. Please download it again."); return 0; }
  step(0.08f, L"Closing the launcher if it's open\x2026");
  if (!close_launcher()) { free(data); fail(L"The Ruff Dogs Launcher is still open. Close it and try again."); return 0; }
  if (SHCreateDirectoryExW(NULL, g_dir, NULL) != ERROR_SUCCESS && !PathIsDirectoryW(g_dir)) { free(data); fail(L"Couldn't make that folder. Pick another place to install."); return 0; }
  _snwprintf(exe, BIG, L"%ls\\RuffDogsLauncher.exe", g_dir);
  _snwprintf(tmp, BIG, L"%ls\\RuffDogsLauncher.exe.new", g_dir);
  _snwprintf(unin, BIG, L"%ls\\Uninstall Ruff Dogs.exe", g_dir);
  // write the launcher in chunks so the bar moves, then swap it into place
  HANDLE f = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  if (f == INVALID_HANDLE_VALUE) { free(data); fail(L"Couldn't write to that folder. Pick another place to install."); return 0; }
  DWORD done = 0, chunk = 64 * 1024, w;
  while (done < n) {
    DWORD c = n - done < chunk ? n - done : chunk;
    if (!WriteFile(f, data + done, c, &w, NULL)) { CloseHandle(f); DeleteFileW(tmp); free(data); fail(L"Ran out of room on that drive."); return 0; }
    done += c; step(0.10f + 0.55f * done / n, L"Copying the Ruff Dogs Launcher\x2026"); Sleep(18);
  }
  CloseHandle(f); free(data);
  if (!MoveFileExW(tmp, exe, MOVEFILE_REPLACE_EXISTING)) { DeleteFileW(tmp); fail(L"Couldn't replace the old launcher. Restart your PC and try again."); return 0; }
  step(0.70f, L"Adding the uninstaller\x2026"); Sleep(200);
  if (_wcsicmp(unin, g_self)) CopyFileW(g_self, unin, FALSE);
  step(0.80f, L"Adding shortcuts\x2026"); Sleep(250);
  WCHAR desk[BIG], menu[BIG]; link_paths(desk, menu);
  if (g_menu) make_link(menu, exe, g_dir); else DeleteFileW(menu);
  if (g_desk) make_link(desk, exe, g_dir); else DeleteFileW(desk);
  step(0.90f, L"Adding Ruff Dogs to your installed apps\x2026"); Sleep(250);
  HKEY k;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, UNINSTALL_KEY, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL) == ERROR_SUCCESS) {
    WCHAR cmd[BIG + 32]; _snwprintf(cmd, BIG + 32, L"\"%ls\" --uninstall", unin);
    reg_str(k, L"DisplayName", L"Ruff Dogs Launcher"); reg_str(k, L"DisplayIcon", exe); reg_str(k, L"DisplayVersion", L"1.0.0");
    reg_str(k, L"Publisher", L"Ruff Dogs Studios"); reg_str(k, L"InstallLocation", g_dir); reg_str(k, L"UninstallString", cmd);
    reg_dword(k, L"NoModify", 1); reg_dword(k, L"NoRepair", 1); reg_dword(k, L"EstimatedSize", (n / 1024) * 2 + 64);
    RegCloseKey(k);
  }
  if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Ruff Dogs\\Launcher", 0, NULL, 0, KEY_WRITE, NULL, &k, NULL) == ERROR_SUCCESS) {
    reg_str(k, L"InstallDir", g_dir); RegCloseKey(k);
  }
  SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
  step(1, L"All done!"); Sleep(500);
  g_page = P_DONE; PostMessageW(g_win, WM_STEP, 1, 0);
  CoUninitialize(); return 0;
}
static void del_tree(const WCHAR *dir) {                // only used on the launcher's own page cache
  WCHAR from[BIG + 2]; memset(from, 0, sizeof from); wcsncpy(from, dir, BIG - 1);
  SHFILEOPSTRUCTW op = { 0 }; op.wFunc = FO_DELETE; op.pFrom = from; op.fFlags = FOF_NO_UI;
  if (exists(dir)) SHFileOperationW(&op);
}
static DWORD WINAPI remove_thread(void *arg) {
  (void)arg; CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
  step(0.1f, L"Closing the launcher if it's open\x2026");
  if (!close_launcher()) { fail(L"The Ruff Dogs Launcher is still open. Close it and try again."); return 0; }
  WCHAR p[BIG];
  step(0.35f, L"Removing the launcher\x2026"); Sleep(300);
  _snwprintf(p, BIG, L"%ls\\RuffDogsLauncher.exe", g_removeDir); DeleteFileW(p);
  _snwprintf(p, BIG, L"%ls\\Uninstall Ruff Dogs.exe", g_removeDir); DeleteFileW(p);
  RemoveDirectoryW(g_removeDir);                          // only goes if nothing else is in it
  step(0.6f, L"Removing shortcuts\x2026"); Sleep(250);
  WCHAR desk[BIG], menu[BIG]; link_paths(desk, menu); DeleteFileW(desk); DeleteFileW(menu);
  wcscpy(p, menu); PathRemoveFileSpecW(p); RemoveDirectoryW(p);   // the Ruff Dogs Start menu folder, if it's empty now
  step(0.8f, L"Tidying up\x2026"); Sleep(250);
  RegDeleteTreeW(HKEY_CURRENT_USER, UNINSTALL_KEY);
  RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Ruff Dogs\\Launcher");
  WCHAR la[MAX_PATH]; SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, la);
  _snwprintf(p, BIG, L"%ls\\Ruff Dogs\\ui", la); del_tree(p);
  _snwprintf(p, BIG, L"%ls\\Ruff Dogs\\ui-next", la); del_tree(p);
  SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
  step(1, L"Removed."); Sleep(400);
  g_page = P_REMOVED; PostMessageW(g_win, WM_STEP, 1, 0);
  CoUninitialize(); return 0;
}

// ---------------------------------------------------------------------------------------------------------- drawing
#define C_BG 0xff100e0c
#define C_PANEL 0xff1a1713
#define C_LINE 0xff2d2822
#define C_TEXT 0xfff3efe8
#define C_DIM 0xffa39d92
#define C_GOLD 0xfff0b43a
#define C_GOLD2 0xffffd27a
#define C_RED 0xffff5a4a

static GpGraphics *G;
static void rr_path(GpPath *p, float x, float y, float w, float h, float r) {
  float d = r * 2;
  GdipAddPathArc(p, x, y, d, d, 180, 90); GdipAddPathArc(p, x + w - d, y, d, d, 270, 90);
  GdipAddPathArc(p, x + w - d, y + h - d, d, d, 0, 90); GdipAddPathArc(p, x, y + h - d, d, d, 90, 90);
  GdipClosePathFigure(p);
}
static void fill_rr(float x, float y, float w, float h, float r, ARGB c) {
  GpPath *p; GdipCreatePath(FillModeAlternate, &p); rr_path(p, x, y, w, h, r);
  GpSolidFill *b; GdipCreateSolidFill(c, &b); GdipFillPath(G, (GpBrush *)b, p); GdipDeleteBrush((GpBrush *)b); GdipDeletePath(p);
}
static void grad_rr(float x, float y, float w, float h, float r, ARGB a, ARGB b2) {
  GpPath *p; GdipCreatePath(FillModeAlternate, &p); rr_path(p, x, y, w, h, r);
  GpRectF rc = { x, y, w, h }; GpLineGradient *b;
  GdipCreateLineBrushFromRect(&rc, a, b2, LinearGradientModeVertical, WrapModeTileFlipXY, &b);
  GdipFillPath(G, (GpBrush *)b, p); GdipDeleteBrush((GpBrush *)b); GdipDeletePath(p);
}
static void stroke_rr(float x, float y, float w, float h, float r, ARGB c, float t) {
  GpPath *p; GdipCreatePath(FillModeAlternate, &p); rr_path(p, x, y, w, h, r);
  GpPen *pen; GdipCreatePen1(c, t, UnitPixel, &pen); GdipDrawPath(G, pen, p); GdipDeletePen(pen); GdipDeletePath(p);
}
static void fill_rect(float x, float y, float w, float h, ARGB c) { GpSolidFill *b; GdipCreateSolidFill(c, &b); GdipFillRectangle(G, (GpBrush *)b, x, y, w, h); GdipDeleteBrush((GpBrush *)b); }
static GpFontFamily *family(int display) {
  static GpFontFamily *disp, *body;
  GpFontFamily **f = display ? &disp : &body;
  if (!*f) {
    if (display && GdipCreateFontFamilyFromName(L"Bahnschrift", NULL, f) == Ok) return *f;
    GdipCreateFontFamilyFromName(L"Segoe UI", NULL, f);
  }
  return *f;
}
// text in a box; align 0 left / 1 centre / 2 right; valign the same, top/middle/bottom
static float text(const WCHAR *s, float x, float y, float w, float h, float size, int display, int bold, ARGB c, int align, int valign) {
  GpFont *font; GdipCreateFont(family(display), size, bold ? FontStyleBold : FontStyleRegular, UnitPixel, &font);
  GpStringFormat *fmt; GdipCreateStringFormat(0, LANG_NEUTRAL, &fmt);
  GdipSetStringFormatAlign(fmt, align == 1 ? StringAlignmentCenter : align == 2 ? StringAlignmentFar : StringAlignmentNear);
  GdipSetStringFormatLineAlign(fmt, valign == 1 ? StringAlignmentCenter : valign == 2 ? StringAlignmentFar : StringAlignmentNear);
  GpRectF rc = { x, y, w, h }, out = { 0 };
  GpSolidFill *b; GdipCreateSolidFill(c, &b);
  GdipDrawString(G, s, -1, font, &rc, fmt, (GpBrush *)b);
  GdipMeasureString(G, s, -1, font, &rc, fmt, &out, NULL, NULL);
  GdipDeleteBrush((GpBrush *)b); GdipDeleteStringFormat(fmt); GdipDeleteFont(font);
  return out.Height;
}
static void hit(float x, float y, float w, float h, int id, int on) { if (g_hitN < 24) g_hits[g_hitN++] = (Hit){ x, y, w, h, id, on }; }

static void button(const WCHAR *label, float x, float y, float w, int id, int primary, int on) {
  float h = 42; int hov = g_hover == id && on, dn = g_down == id && hov;
  if (primary) {
    if (on) grad_rr(x, y + (dn ? 1 : 0), w, h, 7, hov ? 0xffffdf8e : C_GOLD2, hov ? 0xfff6b844 : 0xffe39b25);
    else fill_rr(x, y, w, h, 7, 0xff3a342b);
    text(label, x, y + (dn ? 1 : 0), w, h, 16, 1, 1, on ? 0xff1a1206 : 0xff7d766b, 1, 1);
  } else {
    fill_rr(x, y, w, h, 7, hov ? 0xff2a2620 : 0xff1d1a16); stroke_rr(x + .5f, y + .5f, w - 1, h - 1, 7, hov ? 0xff4a4237 : C_LINE, 1);
    text(label, x, y, w, h, 15, 1, 0, on ? C_TEXT : 0xff6b655c, 1, 1);
  }
  hit(x, y, w, h, id, on);
}
static void checkbox(const WCHAR *label, float x, float y, float w, int id, int value) {
  int hov = g_hover == id;
  fill_rr(x, y + 2, 20, 20, 5, value ? C_GOLD : (hov ? 0xff2a2620 : 0xff1d1a16));
  if (!value) stroke_rr(x + .5f, y + 2.5f, 19, 19, 5, hov ? 0xff6a5f50 : 0xff4a4237, 1.2f);
  else { GpPen *pen; GdipCreatePen1(0xff1a1206, 2.6f, UnitPixel, &pen); GdipSetPenLineCap197819(pen, LineCapRound, LineCapRound, DashCapRound);
         GdipDrawLine(G, pen, x + 5, y + 12.5f, x + 9, y + 16.5f); GdipDrawLine(G, pen, x + 9, y + 16.5f, x + 15.5f, y + 8); GdipDeletePen(pen); }
  text(label, x + 32, y, w - 32, 24, 15, 0, 0, hov ? 0xffffffff : C_TEXT, 0, 1);
  hit(x, y, w, 24, id, 1);
}
static void steps_bar(int cur) {
  static const WCHAR *names[] = { L"WELCOME", L"TERMS", L"LOCATION", L"INSTALL" };
  float x = ART_W + 44, y = 34;
  for (int i = 0; i < 4; i++) {
    ARGB c = i < cur ? C_GOLD : i == cur ? C_GOLD2 : 0xff4a4237;
    fill_rr(x, y, 34, 4, 2, c);
    text(names[i], x, y + 9, 120, 16, 11, 1, i == cur, i == cur ? C_GOLD2 : i < cur ? 0xffb9935a : 0xff5f584e, 0, 0);
    x += 110;
  }
}
static void heading(const WCHAR *h, const WCHAR *sub, float y) {
  float x = ART_W + 44, w = W_ - ART_W - 88;
  text(h, x, y, w, 50, 34, 1, 1, C_TEXT, 0, 0);
  if (sub) text(sub, x, y + 50, w, 120, 15.5f, 0, 0, C_DIM, 0, 0);
}
static void draw_art(void) {
  if (g_art) GdipDrawImageRectI(G, g_art, 0, 0, ART_W, H_);
  else grad_rr(0, 0, ART_W, H_, 0, 0xff2a1f12, 0xff0b0a09);
  // gold edge between the art and the page
  GpRectF rc = { ART_W - 2, 0, 2, H_ }; GpLineGradient *b;
  GdipCreateLineBrushFromRect(&rc, 0x00f0b43a, 0xfff0b43a, LinearGradientModeVertical, WrapModeTileFlipXY, &b);
  GdipFillRectangle(G, (GpBrush *)b, ART_W - 2, 0, 2, H_); GdipDeleteBrush((GpBrush *)b);
}
static void draw_progress(float p, const WCHAR *status) {
  float x = ART_W + 44, w = W_ - ART_W - 88, y = 262;
  fill_rr(x, y, w, 14, 7, 0xff221e19);
  if (p > 0.004f) grad_rr(x, y, w * p < 14 ? 14 : w * p, 14, 7, C_GOLD2, 0xffe08f1c);
  // the little dog trots along the bar
  if (g_dog) { float dx = x + w * p - 22, bob = (float)(sinf(g_anim * 14) * 1.6f); GdipDrawImageRectI(G, g_dog, (INT)dx, (INT)(y - 50 + bob), 44, 44); }
  text(status, x, y + 26, w, 24, 15, 0, 0, C_DIM, 0, 0);
  WCHAR pct[16]; _snwprintf(pct, 16, L"%d%%", (int)(p * 100 + .5f));
  text(pct, x, y + 26, w, 24, 15, 1, 1, C_GOLD2, 2, 0);
}

static void paint(HDC dc) {
  RECT cr; GetClientRect(g_win, &cr);
  HDC mem = CreateCompatibleDC(dc); HBITMAP bmp = CreateCompatibleBitmap(dc, cr.right, cr.bottom); HGDIOBJ old = SelectObject(mem, bmp);
  GdipCreateFromHDC(mem, &G);
  GdipSetSmoothingMode(G, SmoothingModeAntiAlias); GdipSetTextRenderingHint(G, TextRenderingHintAntiAliasGridFit);
  GdipSetInterpolationMode(G, InterpolationModeHighQualityBicubic); GdipSetPixelOffsetMode(G, PixelOffsetModeHalf);
  GdipScaleWorldTransform(G, S, S, MatrixOrderPrepend);
  GdipGraphicsClear(G, C_BG); g_hitN = 0;
  // a soft warm glow in the page's top corner
  fill_rect(ART_W, 0, W_ - ART_W, H_, C_BG);
  draw_art();

  // window buttons
  float bx = W_ - 46;
  fill_rect(bx, 0, 46, 34, g_hover == B_CLOSE ? 0xffc42b1c : 0x00000000); text(L"\x2715", bx, 0, 46, 34, 13, 0, 0, C_DIM, 1, 1); hit(bx, 0, 46, 34, B_CLOSE, 1);
  fill_rect(bx - 46, 0, 46, 34, g_hover == B_MIN ? 0xff2a2620 : 0x00000000); text(L"\x2013", bx - 46, 0, 46, 34, 14, 0, 0, C_DIM, 1, 1); hit(bx - 46, 0, 46, 34, B_MIN, 1);

  float x = ART_W + 44, w = W_ - ART_W - 88, by = H_ - 42 - 32;
  // footer line
  fill_rect(ART_W + 1, by - 22, W_ - ART_W, 1, C_LINE);
  text(L"\x00A9 2026 Ruff Dogs Studios", x, by, 260, 42, 12, 0, 0, 0xff5f584e, 0, 1);

  switch (g_page) {
  case P_WELCOME:
    steps_bar(0);
    if (g_word) GdipDrawImageRectI(G, g_word, (INT)x - 6, 96, 280, 65);
    heading(g_update ? L"Update the launcher" : L"Welcome!", NULL, 182);
    text(g_update ? L"A Ruff Dogs Launcher is already on this PC. This puts the newest version in its place. Your games, saves and account stay just as they are."
                  : L"This sets up the Ruff Dogs Launcher: your home for every Ruff Dogs game. Browse the store, keep your games in your library, and they update by themselves.",
         x, 236, w, 90, 15.5f, 0, 0, C_DIM, 0, 0);
    fill_rr(x, 330, w, 66, 9, C_PANEL); stroke_rr(x + .5f, 330.5f, w - 1, 65, 9, C_LINE, 1);
    text(L"FREE", x + 18, 330, 70, 66, 20, 1, 1, 0xff6be08a, 0, 1);
    text(L"Every game is free. No real money, ever.\nIt takes about 20 MB and a few seconds.", x + 86, 330, w - 100, 66, 14, 0, 0, C_TEXT, 0, 1);
    button(L"CANCEL", W_ - 44 - 130 - 12 - 150, by, 130, B_CANCEL, 0, 1);
    button(L"NEXT", W_ - 44 - 150, by, 150, B_NEXT, 1, 1);
    break;
  case P_TERMS:
    steps_bar(1);
    heading(L"The house rules", L"The short version, so you know what you're getting:", 84);
    {
      static const WCHAR *rules[] = {
        L"Every Ruff Dogs game is free. There are no real-money purchases.",
        L"Games and the launcher update themselves from the internet.",
        L"Pick a username that isn't your real name. Never share your password.",
        L"Some games are scary or rated for older players. Check the rating on each store page.",
        L"Your games and saves stay on your PC. Uninstall from Windows Settings any time.",
      };
      float ry = 168;
      fill_rr(x, ry - 10, w, 196, 9, C_PANEL); stroke_rr(x + .5f, ry - 9.5f, w - 1, 195, 9, C_LINE, 1);
      for (int i = 0; i < 5; i++) {
        fill_rr(x + 18, ry + 8, 6, 6, 3, C_GOLD);
        ry += text(rules[i], x + 36, ry, w - 54, 60, 14.5f, 0, 0, C_TEXT, 0, 0) + 9;
      }
    }
    checkbox(L"I've read the house rules and I agree", x, 378, w, B_AGREE, g_agree);
    button(L"BACK", W_ - 44 - 130 - 12 - 150, by, 130, B_BACK, 0, 1);
    button(L"NEXT", W_ - 44 - 150, by, 150, B_NEXT, 1, g_agree);
    break;
  case P_PLACE:
    steps_bar(2);
    heading(L"Where should it go?", L"The launcher lives in this folder. Your games install to C:\\Ruff Dogs Games, and you can change that later in the launcher's Settings.", 84);
    text(L"INSTALL FOLDER", x, 196, w, 18, 12, 1, 1, C_GOLD, 0, 0);
    fill_rr(x, 218, w - 120, 40, 7, 0xff15130f); stroke_rr(x + .5f, 218.5f, w - 121, 39, 7, g_hover == B_BROWSE ? 0xff4a4237 : 0xff3a342b, 1);
    { float bw = 108; int hov = g_hover == B_BROWSE; fill_rr(x + w - bw, 218, bw, 40, 7, hov ? 0xff2a2620 : 0xff1d1a16); stroke_rr(x + w - bw + .5f, 218.5f, bw - 1, 39, 7, C_LINE, 1);
      text(L"BROWSE", x + w - bw, 218, bw, 40, 14, 1, 0, C_TEXT, 1, 1); hit(x + w - bw, 218, bw, 40, B_BROWSE, 1); }
    text(L"Needs about 20 MB.", x, 264, w, 20, 13, 0, 0, 0xff7d766b, 0, 0);
    checkbox(L"Put a shortcut on my desktop", x, 306, w, B_DESK, g_desk);
    checkbox(L"Add it to the Start menu", x, 340, w, B_MENU, g_menu);
    button(L"BACK", W_ - 44 - 130 - 12 - 170, by, 130, B_BACK, 0, 1);
    button(g_update ? L"UPDATE" : L"INSTALL", W_ - 44 - 170, by, 170, B_NEXT, 1, 1);
    break;
  case P_INSTALL: case P_REMOVING:
    if (g_page == P_INSTALL) steps_bar(3);
    heading(g_page == P_INSTALL ? (g_update ? L"Updating\x2026" : L"Installing\x2026") : L"Removing\x2026",
            g_page == P_INSTALL ? L"Hang tight, this only takes a few seconds." : L"Taking the launcher off this PC.", 120);
    draw_progress(g_prog, g_status);
    break;
  case P_DONE:
    if (g_word) GdipDrawImageRectI(G, g_word, (INT)x - 6, 96, 280, 65);
    heading(L"You're all set!", g_update ? L"The Ruff Dogs Launcher is up to date." : L"The Ruff Dogs Launcher is installed. Sign in or make an account, grab some games and have fun.", 182);
    checkbox(L"Open the Ruff Dogs Launcher now", x, 330, w, B_LAUNCH, g_launch);
    button(L"FINISH", W_ - 44 - 150, by, 150, B_FINISH, 1, 1);
    break;
  case P_UNINSTALL:
    if (g_word) GdipDrawImageRectI(G, g_word, (INT)x - 6, 96, 280, 65);
    heading(L"Remove the launcher?", L"This takes the Ruff Dogs Launcher and its shortcuts off this PC.", 182);
    fill_rr(x, 290, w, 84, 9, C_PANEL); stroke_rr(x + .5f, 290.5f, w - 1, 83, 9, C_LINE, 1);
    text(L"Your installed games and saves stay where they are. Their shortcuts open the launcher, so they'll work again when you reinstall it.",
         x + 18, 290, w - 36, 84, 14, 0, 0, C_TEXT, 0, 1);
    button(L"CANCEL", W_ - 44 - 130 - 12 - 150, by, 130, B_CANCEL, 0, 1);
    button(L"REMOVE", W_ - 44 - 150, by, 150, B_REMOVE, 1, 1);
    break;
  case P_REMOVED:
    heading(L"Removed", L"The Ruff Dogs Launcher is gone from this PC. Come back any time!", 182);
    button(L"CLOSE", W_ - 44 - 150, by, 150, B_FINISH, 1, 1);
    break;
  case P_ERROR:
    heading(L"Uh oh", NULL, 140);
    fill_rr(x, 204, w, 90, 9, 0xff2a1512); stroke_rr(x + .5f, 204.5f, w - 1, 89, 9, 0xff5a2a22, 1);
    text(g_err, x + 18, 204, w - 36, 90, 15, 0, 0, 0xffffc9c0, 0, 1);
    button(L"CLOSE", W_ - 44 - 130 - 12 - 150, by, 130, B_CANCEL, 0, 1);
    button(L"TRY AGAIN", W_ - 44 - 150, by, 150, B_BACK, 1, 1);
    break;
  }
  GdipDeleteGraphics(G); G = NULL;
  BitBlt(dc, 0, 0, cr.right, cr.bottom, mem, 0, 0, SRCCOPY);
  SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem);
}

// ---------------------------------------------------------------------------------------------------------- behaviour
static void place_edit(void) {
  int show = g_page == P_PLACE;
  float x = ART_W + 44 + 12, w = W_ - ART_W - 88 - 120 - 24;
  MoveWindow(g_edit, (int)(x * S), (int)(229 * S), (int)(w * S), (int)(20 * S), TRUE);
  ShowWindow(g_edit, show ? SW_SHOW : SW_HIDE);
}
static void go(int page) {
  if (g_page == P_PLACE) GetWindowTextW(g_edit, g_dir, BIG);
  g_page = page; g_hover = g_down = 0; place_edit(); InvalidateRect(g_win, NULL, FALSE);
}
static void browse(void) {
  IFileOpenDialog *d;
  if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&d))) return;
  DWORD o; IFileOpenDialog_GetOptions(d, &o); IFileOpenDialog_SetOptions(d, o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
  IFileOpenDialog_SetTitle(d, L"Pick a folder for the Ruff Dogs Launcher");
  if (SUCCEEDED(IFileOpenDialog_Show(d, g_win))) {
    IShellItem *it; PWSTR p;
    if (SUCCEEDED(IFileOpenDialog_GetResult(d, &it))) {
      if (SUCCEEDED(IShellItem_GetDisplayName(it, SIGDN_FILESYSPATH, &p))) {
        WCHAR dir[BIG]; wcsncpy(dir, p, BIG - 1); dir[BIG - 1] = 0; PathRemoveBackslashW(dir);
        if (_wcsicmp(PathFindFileNameW(dir), L"Ruff Dogs Launcher")) { size_t n = wcslen(dir); _snwprintf(dir + n, BIG - n, L"\\Ruff Dogs Launcher"); }
        SetWindowTextW(g_edit, dir); CoTaskMemFree(p);
      }
      IShellItem_Release(it);
    }
  }
  IFileOpenDialog_Release(d);
}
static void launch_installed(void) {
  WCHAR exe[BIG]; _snwprintf(exe, BIG, L"%ls\\RuffDogsLauncher.exe", g_dir);
  STARTUPINFOW si = { sizeof si }; PROCESS_INFORMATION pi;
  if (CreateProcessW(exe, NULL, NULL, NULL, FALSE, 0, NULL, g_dir, &si, &pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
}
static void click(int id) {
  switch (id) {
  case B_CLOSE: case B_CANCEL:
    if (g_page == P_INSTALL || g_page == P_REMOVING) return;
    DestroyWindow(g_win); return;
  case B_MIN: ShowWindow(g_win, SW_MINIMIZE); return;
  case B_AGREE: g_agree = !g_agree; break;
  case B_DESK: g_desk = !g_desk; break;
  case B_MENU: g_menu = !g_menu; break;
  case B_LAUNCH: g_launch = !g_launch; break;
  case B_BROWSE: browse(); break;
  case B_BACK:
    if (g_page == P_ERROR) { go(g_uninstall ? P_UNINSTALL : P_PLACE); return; }
    go(g_page - 1); return;
  case B_NEXT:
    if (g_page == P_TERMS && !g_agree) return;
    if (g_page == P_PLACE) {
      GetWindowTextW(g_edit, g_dir, BIG); PathRemoveBackslashW(g_dir);
      if (!g_dir[0] || PathIsRelativeW(g_dir)) { MessageBoxW(g_win, L"Pick a full folder path, like C:\\Games\\Ruff Dogs Launcher.", L"Ruff Dogs Setup", MB_ICONINFORMATION); return; }
      g_prog = 0; g_status[0] = 0; go(P_INSTALL);
      CloseHandle(CreateThread(NULL, 0, install_thread, NULL, 0, NULL)); return;
    }
    go(g_page + 1); return;
  case B_REMOVE:
    g_prog = 0; g_status[0] = 0; go(P_REMOVING);
    CloseHandle(CreateThread(NULL, 0, remove_thread, NULL, 0, NULL)); return;
  case B_FINISH:
    if (g_page == P_DONE && g_launch) launch_installed();
    if (g_uninstall) MoveFileExW(g_self, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);   // the temp copy of the uninstaller
    DestroyWindow(g_win); return;
  }
  InvalidateRect(g_win, NULL, FALSE);
}
static int hit_at(LPARAM lp) {
  float mx = GET_X_LPARAM(lp) / S, my = GET_Y_LPARAM(lp) / S;
  for (int i = g_hitN - 1; i >= 0; i--) { Hit *h = &g_hits[i]; if (mx >= h->x && mx < h->x + h->w && my >= h->y && my < h->y + h->h) return h->on ? h->id : B_NONE; }
  return B_NONE;
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
  case WM_PAINT: { PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); paint(dc); EndPaint(h, &ps); return 0; }
  case WM_ERASEBKGND: return 1;
  case WM_NCHITTEST: {
    LRESULT r = DefWindowProcW(h, msg, wp, lp);
    if (r != HTCLIENT) return r;
    POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) }; ScreenToClient(h, &p);
    return hit_at(MAKELPARAM(p.x, p.y)) ? HTCLIENT : HTCAPTION;    // drag the window from anywhere that isn't a control
  }
  case WM_MOUSEMOVE: {
    int id = hit_at(lp);
    if (id != g_hover) { g_hover = id; InvalidateRect(h, NULL, FALSE); }
    SetCursor(LoadCursorW(NULL, id ? IDC_HAND : IDC_ARROW));
    TRACKMOUSEEVENT t = { sizeof t, TME_LEAVE, h, 0 }; TrackMouseEvent(&t);
    return 0;
  }
  case WM_MOUSELEAVE: g_hover = 0; g_down = 0; InvalidateRect(h, NULL, FALSE); return 0;
  case WM_NCMOUSEMOVE: if (g_hover) { g_hover = 0; InvalidateRect(h, NULL, FALSE); } break;
  case WM_SETCURSOR: if (LOWORD(lp) == HTCLIENT) return TRUE; break;
  case WM_LBUTTONDOWN: g_down = hit_at(lp); SetCapture(h); InvalidateRect(h, NULL, FALSE); return 0;
  case WM_LBUTTONUP: { int id = hit_at(lp), d = g_down; g_down = 0; ReleaseCapture(); InvalidateRect(h, NULL, FALSE); if (id && id == d) click(id); return 0; }
  case WM_KEYDOWN:
    if (wp == VK_RETURN) { if (g_page == P_DONE || g_page == P_REMOVED) click(B_FINISH); else if (g_page == P_UNINSTALL) click(B_REMOVE); else if (g_page <= P_PLACE) click(B_NEXT); }
    if (wp == VK_ESCAPE) click(B_CANCEL);
    return 0;
  case WM_STEP: InvalidateRect(h, NULL, FALSE); if (wp) place_edit(); return 0;
  case WM_TIMER: g_anim += 0.033f; if (g_page == P_INSTALL || g_page == P_REMOVING) InvalidateRect(h, NULL, FALSE); return 0;
  case WM_CTLCOLOREDIT: SetTextColor((HDC)wp, RGB(243, 239, 232)); SetBkColor((HDC)wp, RGB(21, 19, 15)); return (LRESULT)g_editBrush;
  case WM_DPICHANGED: {
    S = HIWORD(wp) / 96.0f; RECT *r = (RECT *)lp;
    SetWindowPos(h, NULL, r->left, r->top, (int)(W_ * S), (int)(H_ * S), SWP_NOZORDER | SWP_NOACTIVATE);
    DeleteObject(g_editFont); g_editFont = CreateFontW(-(int)(15 * S), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    SendMessageW(g_edit, WM_SETFONT, (WPARAM)g_editFont, TRUE); place_edit(); return 0;
  }
  case WM_CLOSE: click(B_CLOSE); return 0;
  case WM_DESTROY: PostQuitMessage(0); return 0;
  }
  return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show) {
  (void)prev; (void)cmd; g_inst = inst;
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
  GetModuleFileNameW(NULL, g_self, BIG);

  // --uninstall [dir]: Windows "Installed apps" runs the copy in the install folder. That copy can't delete itself, so it
  // copies itself to %TEMP% and runs from there with the folder to remove.
  int argc; LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  for (int i = 1; i < argc; i++) if (!wcscmp(argv[i], L"--uninstall")) {
    g_uninstall = 1;
    if (i + 1 < argc) wcsncpy(g_removeDir, argv[i + 1], BIG - 1);
    else {
      wcscpy(g_removeDir, g_self); PathRemoveFileSpecW(g_removeDir);
      WCHAR tmp[MAX_PATH], copy[BIG], line[BIG * 3]; GetTempPathW(MAX_PATH, tmp);
      _snwprintf(copy, BIG, L"%lsRuffDogsUninstall.exe", tmp);
      if (CopyFileW(g_self, copy, FALSE)) {
        _snwprintf(line, BIG * 3, L"\"%ls\" --uninstall \"%ls\"", copy, g_removeDir);
        STARTUPINFOW si = { sizeof si }; PROCESS_INFORMATION pi;
        if (CreateProcessW(copy, line, NULL, NULL, FALSE, 0, NULL, tmp, &si, &pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); return 0; }
      }
    }
  }
  LocalFree(argv);
  if (!g_uninstall) {
    HANDLE m = CreateMutexW(NULL, TRUE, L"RuffDogsSetup.Single");
    if (GetLastError() == ERROR_ALREADY_EXISTS) { HWND o = FindWindowW(L"RuffDogsSetup", NULL); if (o) SetForegroundWindow(o); return 0; }
    (void)m;
  }
  g_page = g_uninstall ? P_UNINSTALL : P_WELCOME;
  default_dir(g_dir);

  ULONG_PTR tok; GdiplusStartupInput gsi = { 1, NULL, FALSE, FALSE }; GdiplusStartup(&tok, &gsi, NULL);
  g_art = load_png(101); g_word = load_png(102); g_dog = load_png(103);

  WNDCLASSEXW wc = { sizeof wc };
  wc.lpfnWndProc = wndproc; wc.hInstance = inst; wc.lpszClassName = L"RuffDogsSetup";
  wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1)); wc.hIconSm = wc.hIcon; wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
  RegisterClassExW(&wc);
  POINT pt = { 0, 0 }; HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
  UINT dx = 96, dy = 96;
  { typedef HRESULT (WINAPI *F)(HMONITOR, int, UINT *, UINT *); HMODULE sh = LoadLibraryW(L"shcore.dll");
    F f = sh ? (F)(void *)GetProcAddress(sh, "GetDpiForMonitor") : NULL; if (f) f(mon, 0, &dx, &dy); }
  S = dx / 96.0f;
  MONITORINFO mi = { sizeof mi }; GetMonitorInfoW(mon, &mi);
  int w = (int)(W_ * S), h = (int)(H_ * S);
  g_win = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName, g_uninstall ? L"Remove Ruff Dogs Launcher" : L"Ruff Dogs Setup", WS_POPUP | WS_MINIMIZEBOX | WS_SYSMENU,
                          mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - w) / 2, mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - h) / 2, w, h, NULL, NULL, inst, NULL);
  S = GetDpiForWindow(g_win) / 96.0f; SetWindowPos(g_win, NULL, 0, 0, (int)(W_ * S), (int)(H_ * S), SWP_NOMOVE | SWP_NOZORDER);
  // rounded corners (with the system shadow) on Windows 11
  int corner = 2; DwmSetWindowAttribute(g_win, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corner, sizeof corner);
  BOOL dark = TRUE; DwmSetWindowAttribute(g_win, 20, &dark, sizeof dark);

  g_editBrush = CreateSolidBrush(RGB(21, 19, 15));
  g_editFont = CreateFontW(-(int)(15 * S), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
  g_edit = CreateWindowExW(0, L"EDIT", g_dir, WS_CHILD | ES_AUTOHSCROLL, 0, 0, 10, 10, g_win, NULL, inst, NULL);
  SendMessageW(g_edit, WM_SETFONT, (WPARAM)g_editFont, TRUE);
  place_edit();
  SetTimer(g_win, T_ANIM, 33, NULL);
  ShowWindow(g_win, show); UpdateWindow(g_win);

  MSG m;
  while (GetMessageW(&m, NULL, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
  GdiplusShutdown(tok); CoUninitialize();
  return 0;
}
int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show) { (void)cmd; return wWinMain(inst, prev, GetCommandLineW(), show); }
