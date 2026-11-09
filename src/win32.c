/* win32.c — fenêtre Windows, contexte OpenGL, clavier/souris, glisser-déposer, réglages */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <commdlg.h>
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <GL/gl.h>
#include <stdio.h>
#include <stdlib.h>
#include "core.h"
#include "gfx.h"

#define WM_LIRE_WAKE (WM_APP + 1)
#define TIMER_REDRAW 1

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

static HWND hwnd;
static HDC hdc;
static HGLRC glrc;
static volatile LONG wake_pending;
static LARGE_INTEGER qpf;
static int swallow_char;
static int fullscreen;
static WINDOWPLACEMENT saved_wp;
static int buttons;
static HCURSOR cur_arrow, cur_hand, cur_cross;
static int tracking;
static HICON menu_icons[64];   /* icônes du menu « Ouvrir avec » (par identifiant de commande) */
static int menu_icon_px = 16;

#define LIRE_COPYDATA 0x4C495245   /* 'LIRE' */

/* ---------- conversions UTF-8 / UTF-16 ---------- */

static wchar_t *to_wide(const char *s)
{
	int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
	wchar_t *w = malloc((n > 0 ? n : 1) * sizeof *w);
	if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n); else w[0] = 0;
	return w;
}

static char *to_utf8(const wchar_t *w)
{
	int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
	char *s = malloc(n > 0 ? n : 1);
	if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL); else s[0] = 0;
	return s;
}

/* ---------- services demandés par le moteur ---------- */

void platform_wake(void)
{
	if (InterlockedExchange(&wake_pending, 1) == 0)
		PostMessageW(hwnd, WM_LIRE_WAKE, 0, 0);
}

void platform_set_title(const char *s)
{
	wchar_t *w = to_wide(s);
	SetWindowTextW(hwnd, w);
	free(w);
}

double platform_time(void)
{
	LARGE_INTEGER c;
	QueryPerformanceCounter(&c);
	return (double)c.QuadPart / (double)qpf.QuadPart;
}

void platform_quit(void) { PostMessageW(hwnd, WM_CLOSE, 0, 0); }
void platform_wheel_mode_changed(int z) { (void)z; }

/* ouvre un fichier dans un nouvel onglet (chemin rendu absolu pour reconnaître un fichier déjà ouvert) */
static void open_file_w(const wchar_t *path)
{
	static wchar_t full[32768];
	char *u;
	DWORD n = GetFullPathNameW(path, sizeof full / sizeof full[0], full, NULL);
	u = to_utf8(n > 0 && n < sizeof full / sizeof full[0] ? full : path);
	core_open(u);
	free(u);
	InvalidateRect(hwnd, NULL, FALSE);
}

void platform_open_dialog(void)
{
	static wchar_t file[65536];
	OPENFILENAMEW ofn;
	memset(&ofn, 0, sizeof ofn);
	file[0] = 0;
	ofn.lStructSize = sizeof ofn;
	ofn.hwndOwner = hwnd;
	ofn.lpstrFilter = L"Documents PDF (*.pdf)\0*.pdf\0Tous les fichiers (*.*)\0*.*\0";
	ofn.lpstrFile = file;
	ofn.nMaxFile = sizeof file / sizeof file[0];
	ofn.lpstrTitle = L"Ouvrir un ou plusieurs PDF";
	ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_HIDEREADONLY | OFN_ALLOWMULTISELECT;
	if (GetOpenFileNameW(&ofn)) {
		size_t n = wcslen(file);
		if (file[n + 1] == 0) {
			open_file_w(file);                       /* un seul fichier : chemin complet */
		} else {
			/* plusieurs fichiers : dossier, puis les noms, séparés par des zéros */
			static wchar_t one[32768];
			const wchar_t *p = file + n + 1;
			while (*p) {
				_snwprintf(one, sizeof one / sizeof one[0], L"%ls\\%ls", file, p);
				one[sizeof one / sizeof one[0] - 1] = 0;
				open_file_w(one);
				p += wcslen(p) + 1;
			}
		}
	}
}


/* ---------- « Ouvrir avec » : le même fichier dans un autre logiciel ---------- */

/* Menu proposé : les logiciels que Windows recommande pour les PDF (Acrobat, Edge,
   Foxit…), puis ceux choisis à la main dans Lire (logiciels portables comme Annoter,
   mémorisés dans HKCU\Software\Lire\OuvrirAvec), puis « Choisir un programme… » et
   la fenêtre « Ouvrir avec » de Windows. */

#define OW_MAX_HANDLERS 24
#define OW_MAX_MRU 8
#define OW_ID_HANDLER 100
#define OW_ID_MRU 200
#define OW_ID_BROWSE 300
#define OW_ID_WINDOWS 301

static const GUID lire_IID_IShellItem = { 0x43826d1e, 0xe718, 0x42ee, { 0xbc, 0x55, 0xa1, 0xe2, 0x61, 0xc3, 0x7b, 0xfe } };
static const GUID lire_IID_IDataObject = { 0x0000010e, 0x0000, 0x0000, { 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
static const GUID lire_BHID_DataObject = { 0xb8c0bd9f, 0xed24, 0x455c, { 0x83, 0xe6, 0xd5, 0x39, 0x0c, 0x4f, 0xe8, 0xc4 } };

typedef HRESULT (WINAPI *EnumHandlersFn)(PCWSTR, ASSOC_FILTER, IEnumAssocHandlers **);
typedef HRESULT (WINAPI *ItemFromPathFn)(PCWSTR, IBindCtx *, REFIID, void **);
typedef HRESULT (WINAPI *OpenWithDlgFn)(HWND, const OPENASINFO *);

static FARPROC shell_fn(const char *name)
{
	HMODULE m = GetModuleHandleW(L"shell32.dll");
	if (!m) m = LoadLibraryW(L"shell32.dll");
	return m ? GetProcAddress(m, name) : NULL;
}

static int same_file_w(const wchar_t *a, const wchar_t *b)
{
	return a && b && CompareStringOrdinal(a, -1, b, -1, TRUE) == CSTR_EQUAL;
}

/* nom lisible d'un programme : description de l'exe (« Adobe Acrobat »), sinon son nom de fichier */
static void program_name(const wchar_t *exe, wchar_t *out, int cap)
{
	DWORD h = 0, sz = GetFileVersionInfoSizeW(exe, &h);
	const wchar_t *base = wcsrchr(exe, L'\\');
	out[0] = 0;
	if (sz > 0) {
		void *buf = malloc(sz);
		struct { WORD lang, cp; } *tr;
		UINT n = 0;
		if (buf && GetFileVersionInfoW(exe, 0, sz, buf) &&
				VerQueryValueW(buf, L"\\VarFileInfo\\Translation", (void **)&tr, &n) && n >= 4) {
			static const wchar_t *keys[] = { L"FileDescription", L"ProductName" };
			int k;
			for (k = 0; k < 2 && !out[0]; k++) {
				wchar_t q[128], *v = NULL;
				UINT vn = 0;
				_snwprintf(q, 128, L"\\StringFileInfo\\%04x%04x\\%ls", tr->lang, tr->cp, keys[k]);
				q[127] = 0;
				if (VerQueryValueW(buf, q, (void **)&v, &vn) && v && vn > 1 && v[0]) {
					wcsncpy(out, v, cap - 1);
					out[cap - 1] = 0;
				}
			}
		}
		free(buf);
	}
	if (!out[0]) {
		wchar_t *dot;
		wcsncpy(out, base ? base + 1 : exe, cap - 1);
		out[cap - 1] = 0;
		dot = wcsrchr(out, L'.');
		if (dot) *dot = 0;
	}
}

static HICON small_icon(const wchar_t *file, int index)
{
	HICON big = NULL, small = NULL;
	if (!file || !file[0]) return NULL;
	if (menu_icon_px > 20) {
		/* écran haute densité : on prend la grande icône, réduite au dessin */
		if (ExtractIconExW(file, index, &big, NULL, 1) > 0 && big) return big;
	}
	if (ExtractIconExW(file, index, NULL, &small, 1) > 0 && small) return small;
	if (ExtractIconExW(file, index, &big, NULL, 1) > 0) return big;
	return NULL;
}

static int mru_load(wchar_t mru[OW_MAX_MRU][MAX_PATH])
{
	HKEY k;
	wchar_t buf[OW_MAX_MRU * MAX_PATH + 2];
	DWORD sz = sizeof buf - 2 * sizeof(wchar_t), type = 0;
	int n = 0;
	const wchar_t *p;
	if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Lire\\OuvrirAvec", 0, KEY_READ, &k) != ERROR_SUCCESS) return 0;
	memset(buf, 0, sizeof buf);
	if (RegQueryValueExW(k, L"Programmes", NULL, &type, (BYTE *)buf, &sz) == ERROR_SUCCESS && type == REG_MULTI_SZ)
		for (p = buf; *p && n < OW_MAX_MRU; p += wcslen(p) + 1)
			if (wcslen(p) < MAX_PATH) wcscpy(mru[n++], p);
	RegCloseKey(k);
	return n;
}

/* met exe en tête de la liste des programmes mémorisés */
static void mru_push(const wchar_t *exe)
{
	wchar_t mru[OW_MAX_MRU][MAX_PATH], buf[OW_MAX_MRU * MAX_PATH + 2];
	int n = mru_load(mru), i, m = 0;
	size_t pos = 0;
	HKEY k;
	if (wcslen(exe) >= MAX_PATH) return;
	wcscpy(buf, exe);
	pos = wcslen(exe) + 1;
	for (i = 0; i < n && m < OW_MAX_MRU - 1; i++) {
		if (same_file_w(mru[i], exe)) continue;
		wcscpy(buf + pos, mru[i]);
		pos += wcslen(mru[i]) + 1;
		m++;
	}
	buf[pos++] = 0;
	if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Lire\\OuvrirAvec", 0, NULL, 0, KEY_WRITE, NULL, &k, NULL) != ERROR_SUCCESS) return;
	RegSetValueExW(k, L"Programmes", 0, REG_MULTI_SZ, (const BYTE *)buf, (DWORD)(pos * sizeof(wchar_t)));
	RegCloseKey(k);
}

static int launch_exe(const wchar_t *exe, const wchar_t *file)
{
	wchar_t *args = malloc((wcslen(file) + 3) * sizeof *args), dir[MAX_PATH], *slash;
	INT_PTR r;
	_snwprintf(args, wcslen(file) + 3, L"\"%ls\"", file);
	args[wcslen(file) + 2] = 0;
	wcsncpy(dir, exe, MAX_PATH - 1);
	dir[MAX_PATH - 1] = 0;
	slash = wcsrchr(dir, L'\\');
	if (slash) *slash = 0;
	r = (INT_PTR)ShellExecuteW(hwnd, NULL, exe, args, slash ? dir : NULL, SW_SHOWNORMAL);
	free(args);
	return r > 32;
}

static int invoke_handler(IAssocHandler *h, const wchar_t *file)
{
	ItemFromPathFn from_path = (ItemFromPathFn)(void *)shell_fn("SHCreateItemFromParsingName");
	IShellItem *item = NULL;
	IDataObject *data = NULL;
	HRESULT hr = E_FAIL;
	if (from_path && SUCCEEDED(from_path(file, NULL, &lire_IID_IShellItem, (void **)&item))) {
		if (SUCCEEDED(IShellItem_BindToHandler(item, NULL, &lire_BHID_DataObject, &lire_IID_IDataObject, (void **)&data))) {
			hr = IAssocHandler_Invoke(h, data);
			IDataObject_Release(data);
		}
		IShellItem_Release(item);
	}
	if (FAILED(hr)) {   /* repli : lancer directement l'exe du logiciel */
		LPWSTR exe = NULL;
		if (SUCCEEDED(IAssocHandler_GetName(h, &exe)) && exe) {
			int ok = launch_exe(exe, file);
			CoTaskMemFree(exe);
			return ok;
		}
		return 0;
	}
	return 1;
}

static void browse_program(const wchar_t *file)
{
	wchar_t exe[MAX_PATH];
	OPENFILENAMEW ofn;
	memset(&ofn, 0, sizeof ofn);
	exe[0] = 0;
	ofn.lStructSize = sizeof ofn;
	ofn.hwndOwner = hwnd;
	ofn.lpstrFilter = L"Programmes (*.exe)\0*.exe\0";
	ofn.lpstrFile = exe;
	ofn.nMaxFile = MAX_PATH;
	ofn.lpstrTitle = L"Choisir le logiciel qui ouvrira ce PDF";
	ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
	if (GetOpenFileNameW(&ofn) && launch_exe(exe, file)) mru_push(exe);
}

void platform_open_with(const char *path, int x, int y)
{
	EnumHandlersFn enum_handlers = (EnumHandlersFn)(void *)shell_fn("SHAssocEnumHandlers");
	OpenWithDlgFn open_with_dlg = (OpenWithDlgFn)(void *)shell_fn("SHOpenWithDialog");
	IAssocHandler *handlers[OW_MAX_HANDLERS];
	wchar_t mru[OW_MAX_MRU][MAX_PATH], self[MAX_PATH], *file = to_wide(path);
	wchar_t handler_exe[OW_MAX_HANDLERS][MAX_PATH];
	int nh = 0, nm, i, cmd, items = 0;
	HMENU menu = CreatePopupMenu();
	POINT pt;
	UINT dpi = 96;
	typedef UINT (WINAPI *DpiWinFn)(HWND);
	DpiWinFn get_dpi = (DpiWinFn)(void *)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
	if (get_dpi) dpi = get_dpi(hwnd);
	menu_icon_px = MulDiv(16, (int)dpi, 96);
	memset(menu_icons, 0, sizeof menu_icons);
	GetModuleFileNameW(NULL, self, MAX_PATH);

	/* 1) logiciels recommandés par Windows pour les PDF (sauf Lire lui-même) */
	if (enum_handlers) {
		IEnumAssocHandlers *e = NULL;
		if (SUCCEEDED(enum_handlers(L".pdf", ASSOC_FILTER_RECOMMENDED, &e)) && e) {
			IAssocHandler *h = NULL;
			ULONG got = 0;
			while (nh < OW_MAX_HANDLERS && IEnumAssocHandlers_Next(e, 1, &h, &got) == S_OK && got == 1) {
				LPWSTR exe = NULL, ui = NULL, icon = NULL;
				int idx = 0;
				handler_exe[nh][0] = 0;
				if (SUCCEEDED(IAssocHandler_GetName(h, &exe)) && exe) {
					wcsncpy(handler_exe[nh], exe, MAX_PATH - 1);
					handler_exe[nh][MAX_PATH - 1] = 0;
					CoTaskMemFree(exe);
				}
				if (same_file_w(handler_exe[nh], self) || FAILED(IAssocHandler_GetUIName(h, &ui)) || !ui) {
					IAssocHandler_Release(h);
					continue;
				}
				AppendMenuW(menu, MF_STRING, OW_ID_HANDLER + nh, ui);
				CoTaskMemFree(ui);
				if (SUCCEEDED(IAssocHandler_GetIconLocation(h, &icon, &idx)) && icon) {
					menu_icons[nh] = small_icon(icon, idx);
					CoTaskMemFree(icon);
				}
				handlers[nh++] = h;
				items++;
			}
			IEnumAssocHandlers_Release(e);
		}
	}

	/* 2) logiciels choisis à la main auparavant (s'ils existent encore et ne sont pas déjà listés) */
	nm = mru_load(mru);
	{
		int sep = 0;
		for (i = 0; i < nm; i++) {
			wchar_t name[256];
			int k, dup = same_file_w(mru[i], self);
			for (k = 0; k < nh && !dup; k++) dup = same_file_w(mru[i], handler_exe[k]);
			if (dup || GetFileAttributesW(mru[i]) == INVALID_FILE_ATTRIBUTES) continue;
			if (items && !sep) { AppendMenuW(menu, MF_SEPARATOR, 0, NULL); sep = 1; }
			program_name(mru[i], name, 256);
			AppendMenuW(menu, MF_STRING, OW_ID_MRU + i, name);
			menu_icons[OW_MAX_HANDLERS + i] = small_icon(mru[i], 0);
			items++;
		}
	}

	/* 3) choix libre */
	if (items) AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
	AppendMenuW(menu, MF_STRING, OW_ID_BROWSE, L"Choisir un programme…");
	if (open_with_dlg) AppendMenuW(menu, MF_STRING, OW_ID_WINDOWS, L"Autres applications (Windows)…");

	/* icônes dessinées à la demande (WM_MEASUREITEM / WM_DRAWITEM) */
	for (i = 0; i < nh + OW_MAX_MRU; i++) {
		int id = i < nh ? OW_ID_HANDLER + i : OW_ID_MRU + (i - nh);
		HICON ic = i < nh ? menu_icons[i] : menu_icons[OW_MAX_HANDLERS + (i - nh)];
		if (ic) {
			MENUITEMINFOW mi;
			memset(&mi, 0, sizeof mi);
			mi.cbSize = sizeof mi;
			mi.fMask = MIIM_BITMAP | MIIM_DATA;
			mi.hbmpItem = HBMMENU_CALLBACK;
			mi.dwItemData = (ULONG_PTR)ic;
			SetMenuItemInfoW(menu, id, FALSE, &mi);
		}
	}

	pt.x = x;
	pt.y = y;
	ClientToScreen(hwnd, &pt);
	cmd = (int)TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON, pt.x, pt.y, hwnd, NULL);

	if (cmd >= OW_ID_HANDLER && cmd < OW_ID_HANDLER + nh) invoke_handler(handlers[cmd - OW_ID_HANDLER], file);
	else if (cmd >= OW_ID_MRU && cmd < OW_ID_MRU + nm) {
		if (launch_exe(mru[cmd - OW_ID_MRU], file)) mru_push(mru[cmd - OW_ID_MRU]);
	} else if (cmd == OW_ID_BROWSE) browse_program(file);
	else if (cmd == OW_ID_WINDOWS && open_with_dlg) {
		OPENASINFO oi;
		memset(&oi, 0, sizeof oi);
		oi.pcszFile = file;
		oi.oaifInFlags = OAIF_EXEC | OAIF_HIDE_REGISTRATION;   /* ne change jamais l'application par défaut */
		open_with_dlg(hwnd, &oi);
	}

	for (i = 0; i < nh; i++) IAssocHandler_Release(handlers[i]);
	for (i = 0; i < (int)(sizeof menu_icons / sizeof menu_icons[0]); i++)
		if (menu_icons[i]) { DestroyIcon(menu_icons[i]); menu_icons[i] = NULL; }
	DestroyMenu(menu);
	free(file);
}

void platform_toggle_fullscreen(void)
{
	if (!fullscreen) {
		MONITORINFO mi;
		mi.cbSize = sizeof mi;
		saved_wp.length = sizeof saved_wp;
		GetWindowPlacement(hwnd, &saved_wp);
		GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
		SetWindowLongPtrW(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
		SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
				mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
				SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
		fullscreen = 1;
	} else {
		SetWindowLongPtrW(hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
		SetWindowPlacement(hwnd, &saved_wp);
		SetWindowPos(hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
		fullscreen = 0;
	}
}

/* ---------- réglages (registre : HKCU\Software\Lire) ---------- */

static void load_settings(int *wheel, WINDOWPLACEMENT *wp, int *has_wp)
{
	HKEY k;
	DWORD v, sz;
	if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Lire", 0, KEY_READ, &k) != ERROR_SUCCESS) return;
	sz = sizeof v;
	if (RegQueryValueExW(k, L"WheelZoom", NULL, NULL, (BYTE *)&v, &sz) == ERROR_SUCCESS) *wheel = (int)v;
	sz = sizeof *wp;
	if (RegQueryValueExW(k, L"Window", NULL, NULL, (BYTE *)wp, &sz) == ERROR_SUCCESS && sz == sizeof *wp)
		*has_wp = 1;
	RegCloseKey(k);
}

static void save_settings(void)
{
	HKEY k;
	DWORD v = (DWORD)core_wheel_zoom();
	WINDOWPLACEMENT wp;
	if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Lire", 0, NULL, 0, KEY_WRITE, NULL, &k, NULL) != ERROR_SUCCESS) return;
	RegSetValueExW(k, L"WheelZoom", 0, REG_DWORD, (const BYTE *)&v, sizeof v);
	if (fullscreen) wp = saved_wp;
	else { wp.length = sizeof wp; GetWindowPlacement(hwnd, &wp); }
	RegSetValueExW(k, L"Window", 0, REG_BINARY, (const BYTE *)&wp, sizeof wp);
	RegCloseKey(k);
}

/* ---------- OpenGL ---------- */

static int init_gl(void)
{
	PIXELFORMATDESCRIPTOR pfd;
	int pf;
	typedef BOOL (WINAPI *SwapFn)(int);
	SwapFn swap;
	memset(&pfd, 0, sizeof pfd);
	pfd.nSize = sizeof pfd;
	pfd.nVersion = 1;
	pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
	pfd.iPixelType = PFD_TYPE_RGBA;
	pfd.cColorBits = 32;
	pfd.cAlphaBits = 8;
	pfd.iLayerType = PFD_MAIN_PLANE;
	hdc = GetDC(hwnd);
	pf = ChoosePixelFormat(hdc, &pfd);
	if (!pf || !SetPixelFormat(hdc, pf, &pfd)) return -1;
	glrc = wglCreateContext(hdc);
	if (!glrc || !wglMakeCurrent(hdc, glrc)) return -1;
	swap = (SwapFn)(void *)wglGetProcAddress("wglSwapIntervalEXT");
	if (swap) swap(1);
	return gfx_init();
}

/* ---------- entrées ---------- */

static int mods(void)
{
	int m = 0;
	if (GetKeyState(VK_CONTROL) < 0) m |= LMOD_CTRL;
	if (GetKeyState(VK_SHIFT) < 0) m |= LMOD_SHIFT;
	if (GetKeyState(VK_MENU) < 0) m |= LMOD_ALT;
	return m;
}

static void refresh(void)
{
	if (core_dirty()) InvalidateRect(hwnd, NULL, FALSE);
}

static void update_cursor(void)
{
	switch (core_cursor()) {
	case CUR_HAND: case CUR_AUTO: SetCursor(cur_hand); break;
	case CUR_CROSS: SetCursor(cur_cross); break;
	default: SetCursor(cur_arrow); break;
	}
}

static int map_key(WPARAM vk)
{
	switch (vk) {
	case VK_PRIOR: return K_PGUP;
	case VK_NEXT: return K_PGDN;
	case VK_HOME: return K_HOME;
	case VK_END: return K_END;
	case VK_UP: return K_UP;
	case VK_DOWN: return K_DOWN;
	case VK_LEFT: return K_LEFT;
	case VK_RIGHT: return K_RIGHT;
	case VK_SPACE: return K_SPACE;
	case VK_RETURN: return K_ENTER;
	case VK_ESCAPE: return K_ESC;
	case VK_BACK: return K_BACKSPACE;
	case VK_F1: return K_F1;
	case VK_F5: return K_F5;
	case VK_F11: return K_F11;
	case VK_TAB: return K_TAB;
	case VK_F4: return K_F4;
	}
	if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) return (int)vk;
	if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return (int)('0' + (vk - VK_NUMPAD0));
	return 0;
}

static int button_of(UINT msg)
{
	switch (msg) {
	case WM_LBUTTONDOWN: case WM_LBUTTONUP: return BTN_LEFT;
	case WM_MBUTTONDOWN: case WM_MBUTTONUP: return BTN_MIDDLE;
	default: return BTN_RIGHT;
	}
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg) {
	case WM_LIRE_WAKE:
		InterlockedExchange(&wake_pending, 0);
		InvalidateRect(h, NULL, FALSE);
		return 0;

	case WM_PAINT: {
		PAINTSTRUCT ps;
		int t;
		BeginPaint(h, &ps);
		core_poll();
		core_draw();
		SwapBuffers(hdc);
		EndPaint(h, &ps);
		t = core_timeout_ms();
		if (t >= 0) SetTimer(h, TIMER_REDRAW, (UINT)t, NULL);
		if (core_dirty()) InvalidateRect(h, NULL, FALSE);
		return 0;
	}

	case WM_TIMER:
		KillTimer(h, wp);
		InvalidateRect(h, NULL, FALSE);
		return 0;

	case WM_ERASEBKGND:
		return 1;

	case WM_SIZE:
		if (wp != SIZE_MINIMIZED) {
			core_resize(LOWORD(lp), HIWORD(lp));
			InvalidateRect(h, NULL, FALSE);
		}
		return 0;

	case WM_DPICHANGED: {
		RECT *r = (RECT *)lp;
		core_set_dpi((float)HIWORD(wp));
		SetWindowPos(h, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
		InvalidateRect(h, NULL, FALSE);
		return 0;
	}

	case WM_MOUSEMOVE:
		if (!tracking) {
			TRACKMOUSEEVENT tme;
			tme.cbSize = sizeof tme;
			tme.dwFlags = TME_LEAVE;
			tme.hwndTrack = h;
			tme.dwHoverTime = 0;
			tracking = TrackMouseEvent(&tme);
		}
		core_mouse_move(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), mods());
		refresh();
		return 0;

	case WM_MOUSELEAVE:
		tracking = 0;
		core_mouse_leave();
		refresh();
		return 0;

	case WM_LBUTTONDOWN: case WM_MBUTTONDOWN: case WM_RBUTTONDOWN: {
		int b = button_of(msg);
		SetFocus(h);
		SetCapture(h);
		buttons |= 1 << b;
		core_mouse_down(b, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), mods());
		update_cursor();
		refresh();
		return 0;
	}

	case WM_LBUTTONUP: case WM_MBUTTONUP: case WM_RBUTTONUP: {
		int b = button_of(msg);
		buttons &= ~(1 << b);
		core_mouse_up(b, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), mods());
		if (!buttons) ReleaseCapture();
		update_cursor();
		refresh();
		return 0;
	}

	case WM_CAPTURECHANGED:
		if (buttons) {
			POINT pt;
			GetCursorPos(&pt);
			ScreenToClient(h, &pt);
			core_mouse_up(BTN_LEFT, pt.x, pt.y, 0);
			core_mouse_up(BTN_MIDDLE, pt.x, pt.y, 0);
			core_mouse_up(BTN_RIGHT, pt.x, pt.y, 0);
			buttons = 0;
			refresh();
		}
		return 0;

	case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL: {
		POINT pt;
		float d = GET_WHEEL_DELTA_WPARAM(wp) / 120.0f;
		pt.x = GET_X_LPARAM(lp);
		pt.y = GET_Y_LPARAM(lp);
		ScreenToClient(h, &pt);
		if (msg == WM_MOUSEWHEEL) core_wheel(d, 0, pt.x, pt.y, mods());
		else core_wheel(0, d, pt.x, pt.y, mods());
		refresh();
		return 0;
	}

	case WM_SETCURSOR:
		if (LOWORD(lp) == HTCLIENT) {
			update_cursor();
			return TRUE;
		}
		break;

	case WM_KEYDOWN: {
		int m = mods(), k;
		swallow_char = 0;
		if (core_input_mode() == INPUT_GOTO) {
			int d = -1;
			if (wp >= '0' && wp <= '9') d = (int)(wp - '0');
			else if (wp >= VK_NUMPAD0 && wp <= VK_NUMPAD9) d = (int)(wp - VK_NUMPAD0);
			if (d >= 0) {
				core_char('0' + d);
				swallow_char = 1;
				refresh();
				return 0;
			}
		}
		if ((m & LMOD_CTRL) && (wp == VK_ADD || wp == VK_OEM_PLUS)) { core_key(K_PLUS, m); refresh(); return 0; }
		if ((m & LMOD_CTRL) && (wp == VK_SUBTRACT || wp == VK_OEM_MINUS || wp == '6')) { core_key(K_MINUS, m); refresh(); return 0; }
		k = map_key(wp);
		if (k >= 256 || (k && (m & LMOD_CTRL))) {
			core_key(k, m);
			refresh();
			return 0;
		}
		break;
	}

	case WM_CHAR:
		if (swallow_char) { swallow_char = 0; return 0; }
		if (wp >= 32 && !(wp >= 0xD800 && wp <= 0xDFFF)) {
			core_char((unsigned)wp);
			refresh();
		}
		return 0;

	case WM_DROPFILES: {
		HDROP d = (HDROP)wp;
		static wchar_t f[32768];
		UINT i, n = DragQueryFileW(d, 0xFFFFFFFF, NULL, 0);
		for (i = 0; i < n; i++)
			if (DragQueryFileW(d, i, f, sizeof f / sizeof f[0]))
				open_file_w(f);
		DragFinish(d);
		SetForegroundWindow(h);
		return 0;
	}

	case WM_COPYDATA: {
		/* fichier ouvert depuis l'Explorateur alors que Lire tourne déjà : nouvel onglet ici */
		COPYDATASTRUCT *cds = (COPYDATASTRUCT *)lp;
		if (cds && cds->dwData == LIRE_COPYDATA && cds->cbData >= sizeof(wchar_t) && cds->cbData < 65536) {
			wchar_t *w = malloc(cds->cbData + sizeof(wchar_t));
			memcpy(w, cds->lpData, cds->cbData);
			w[cds->cbData / sizeof(wchar_t)] = 0;
			open_file_w(w);
			free(w);
			if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
			SetForegroundWindow(h);
		}
		return TRUE;
	}

	case WM_MEASUREITEM: {
		MEASUREITEMSTRUCT *mis = (MEASUREITEMSTRUCT *)lp;
		if (mis && mis->CtlType == ODT_MENU) {
			mis->itemWidth = menu_icon_px;
			mis->itemHeight = menu_icon_px;
			return TRUE;
		}
		break;
	}

	case WM_DRAWITEM: {
		DRAWITEMSTRUCT *dis = (DRAWITEMSTRUCT *)lp;
		if (dis && dis->CtlType == ODT_MENU && dis->itemData) {
			int hgt = dis->rcItem.bottom - dis->rcItem.top;
			DrawIconEx(dis->hDC, dis->rcItem.left, dis->rcItem.top + (hgt - menu_icon_px) / 2,
					(HICON)dis->itemData, menu_icon_px, menu_icon_px, 0, NULL, DI_NORMAL);
			return TRUE;
		}
		break;
	}

	case WM_CLOSE:
		save_settings();
		ShowWindow(h, SW_HIDE);   /* la fenêtre disparaît immédiatement */
		DestroyWindow(h);
		return 0;

	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show)
{
	WNDCLASSEXW wc;
	WINDOWPLACEMENT wpl;
	HMODULE u32 = GetModuleHandleW(L"user32.dll");
	int wheel = 0, has_wp = 0, argc = 0;
	UINT dpi = 96;
	RECT rc;
	MSG m;
	LPWSTR *argv;
	typedef BOOL (WINAPI *DpiCtxFn)(HANDLE);
	typedef UINT (WINAPI *DpiWinFn)(HWND);
	DpiCtxFn set_ctx = (DpiCtxFn)(void *)GetProcAddress(u32, "SetProcessDpiAwarenessContext");
	DpiWinFn get_dpi = (DpiWinFn)(void *)GetProcAddress(u32, "GetDpiForWindow");
	HANDLE instance_mutex;
	(void)prev;
	(void)cmdline;

	/* netteté sur les écrans haute densité : on travaille en vrais pixels */
	if (!set_ctx || !set_ctx((HANDLE)(intptr_t)-4 /* PER_MONITOR_AWARE_V2 */))
		SetProcessDPIAware();
	QueryPerformanceFrequency(&qpf);
	CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);   /* « Ouvrir avec » (shell) */

	/* Instance unique : si Lire est déjà ouvert, on lui confie les fichiers (nouveaux onglets).
	   Le verrou évite d'ouvrir plusieurs fenêtres quand l'Explorateur lance plusieurs fichiers d'un coup. */
	argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	instance_mutex = CreateMutexW(NULL, FALSE, L"Local\\Lire.Visionneuse");
	if (instance_mutex) WaitForSingleObject(instance_mutex, 5000);
	if (argv && argc > 1) {
		HWND other = FindWindowW(L"LireFenetre", NULL);
		if (other) {
			DWORD pid = 0;
			int i;
			static wchar_t full[32768];
			GetWindowThreadProcessId(other, &pid);
			AllowSetForegroundWindow(pid);
			for (i = 1; i < argc; i++) {
				COPYDATASTRUCT cds;
				DWORD n = GetFullPathNameW(argv[i], sizeof full / sizeof full[0], full, NULL);
				const wchar_t *p = n > 0 && n < sizeof full / sizeof full[0] ? full : argv[i];
				cds.dwData = LIRE_COPYDATA;
				cds.cbData = (DWORD)((wcslen(p) + 1) * sizeof(wchar_t));
				cds.lpData = (void *)p;
				SendMessageTimeoutW(other, WM_COPYDATA, 0, (LPARAM)&cds, SMTO_ABORTIFHUNG, 10000, NULL);
			}
			if (instance_mutex) { ReleaseMutex(instance_mutex); CloseHandle(instance_mutex); }
			LocalFree(argv);
			return 0;
		}
	}

	cur_arrow = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
	cur_hand = LoadCursorW(NULL, (LPCWSTR)IDC_SIZEALL);
	cur_cross = LoadCursorW(NULL, (LPCWSTR)IDC_CROSS);

	memset(&wc, 0, sizeof wc);
	wc.cbSize = sizeof wc;
	wc.style = CS_OWNDC;
	wc.lpfnWndProc = wndproc;
	wc.hInstance = inst;
	wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
	wc.hIconSm = wc.hIcon;
	wc.hCursor = cur_arrow;
	wc.lpszClassName = L"LireFenetre";
	RegisterClassExW(&wc);

	memset(&wpl, 0, sizeof wpl);
	wpl.length = sizeof wpl;
	load_settings(&wheel, &wpl, &has_wp);

	hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, L"LireFenetre", L"Lire", WS_OVERLAPPEDWINDOW,
			CW_USEDEFAULT, CW_USEDEFAULT, 1400, 950, NULL, NULL, inst, NULL);
	if (!hwnd) return 1;
	if (instance_mutex) ReleaseMutex(instance_mutex);   /* la fenêtre existe : les suivants la trouveront */
	if (init_gl()) {
		MessageBoxW(NULL, L"Impossible d'initialiser OpenGL sur cette machine.", L"Lire", MB_ICONERROR);
		return 1;
	}
	if (core_init(0)) {
		MessageBoxW(NULL, L"Impossible d'initialiser le moteur PDF.", L"Lire", MB_ICONERROR);
		return 1;
	}
	core_set_wheel_zoom(wheel);
	if (get_dpi) dpi = get_dpi(hwnd);
	core_set_dpi((float)dpi);

	if (has_wp) {
		if (wpl.showCmd != SW_SHOWMAXIMIZED) wpl.showCmd = SW_SHOWNORMAL;
		SetWindowPlacement(hwnd, &wpl);
		if (get_dpi && get_dpi(hwnd) != dpi) core_set_dpi((float)get_dpi(hwnd));
	} else {
		ShowWindow(hwnd, show);
	}
	GetClientRect(hwnd, &rc);
	core_resize(rc.right - rc.left, rc.bottom - rc.top);
	UpdateWindow(hwnd);

	if (argv) {
		int i;
		for (i = 1; i < argc; i++) open_file_w(argv[i]);
		LocalFree(argv);
	}

	while (GetMessageW(&m, NULL, 0, 0) > 0) {
		TranslateMessage(&m);
		DispatchMessageW(&m);
	}
	/* Fermeture instantanée : on arrête les rendus et on laisse Windows récupérer la
	   mémoire d'un bloc. TerminateProcess évite au pilote graphique de libérer une à
	   une des centaines de Mo de textures (plusieurs secondes sur certaines machines).
	   Les réglages sont déjà enregistrés (WM_CLOSE). */
	core_quick_exit();
	TerminateProcess(GetCurrentProcess(), 0);
	ExitProcess(0);
	return 0;
}
