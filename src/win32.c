/* win32.c — fenêtre Windows, contexte OpenGL, clavier/souris, glisser-déposer, réglages */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <commdlg.h>
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
	case CUR_HAND: SetCursor(cur_hand); break;
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
	/* fermeture instantanée : on arrête les rendus, Windows libère la mémoire */
	core_quick_exit();
	ExitProcess(0);
	return 0;
}
