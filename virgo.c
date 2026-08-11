#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#define sb_free(a) ((a) ? HeapFree(GetProcessHeap(), 0, stb__sbraw(a)), 0 : 0)
#define sb_push(a, v) (stb__sbmaybegrow(a, 1), (a)[stb__sbn(a)++] = (v))
#define sb_count(a) ((a) ? stb__sbn(a) : 0)

#define stb__sbraw(a) ((int *)(a) - 2)
#define stb__sbm(a) stb__sbraw(a)[0]
#define stb__sbn(a) stb__sbraw(a)[1]

#define stb__sbneedgrow(a, n) ((a) == 0 || stb__sbn(a) + (n) >= stb__sbm(a))
#define stb__sbmaybegrow(a, n) (stb__sbneedgrow(a, (n)) ? stb__sbgrow(a, n) : 0)
#define stb__sbgrow(a, n) ((a) = stb__sbgrowf((a), (n), sizeof(*(a))))

#ifndef MOD_NOREPEAT
#define MOD_NOREPEAT 0x4000
#endif

#define NUM_DESKTOPS 4
#define NUM_MONITORS 4

char *icons[] = {"1.ico", "2.ico", "3.ico", "4.ico"};

typedef struct {
	HWND *windows;
	unsigned count;
} Windows;

typedef struct {
	NOTIFYICONDATA nid;
	HWND hwnd;
	unsigned bitmapWidth;
} Trayicon;

typedef struct {
	unsigned current;
	HMONITOR monitor;
	HWND lastFocus[NUM_DESKTOPS];
	Windows desktops[NUM_DESKTOPS];
} MonitorState;

typedef struct {
	unsigned handle_hotkeys;
	Trayicon trayicon;
	MonitorState monitors[NUM_MONITORS];
} Virgo;

static void *stb__sbgrowf(void *arr, unsigned increment, unsigned itemsize) {
	unsigned dbl_cur = arr ? 2 * stb__sbm(arr) : 0;
	unsigned min_needed = sb_count(arr) + increment;
	unsigned m = dbl_cur > min_needed ? dbl_cur : min_needed;
	unsigned *p;
	if (arr) {
		p = HeapReAlloc(GetProcessHeap(), 0, stb__sbraw(arr),
						itemsize * m + sizeof(unsigned) * 2);
	} else {
		p = HeapAlloc(GetProcessHeap(), 0, itemsize * m + sizeof(unsigned) * 2);
	}
	if (p) {
		if (!arr) {
			p[1] = 0;
		}
		p[0] = m;
		return p + 2;
	} else {
		ExitProcess(1);
		return (void *)(2 * sizeof(unsigned));
	}
}

static void trayicon_draw(Trayicon *t, unsigned number) {
	ExtractIconEx(icons[number], 0, &t->nid.hIcon, NULL, 1);
}

static void trayicon_init(Trayicon *t) {
	t->hwnd =
		CreateWindowA("STATIC", "virgo", 0, 0, 0, 0, 0, NULL, NULL, NULL, NULL);
	t->bitmapWidth = GetSystemMetrics(SM_CXSMICON);
	t->nid.cbSize = sizeof(t->nid);
	t->nid.hWnd = t->hwnd;
	t->nid.uID = 100;
	t->nid.uFlags = NIF_ICON;
	trayicon_draw(t, 0);
	Shell_NotifyIcon(NIM_ADD, &t->nid);
}

static void trayicon_set(Trayicon *t, unsigned number) {
	if (number > 9) {
		return;
	}
	DestroyIcon(t->nid.hIcon);
	trayicon_draw(t, number);
	Shell_NotifyIcon(NIM_MODIFY, &t->nid);
}

static void trayicon_deinit(Trayicon *t) {
	Shell_NotifyIcon(NIM_DELETE, &t->nid);
	DestroyIcon(t->nid.hIcon);
	DestroyWindow(t->hwnd);
}

typedef struct {
	unsigned index;
	Virgo *virgo;
} MonitorEnumContext;

static BOOL CALLBACK find_monitor_handles(HMONITOR monitor, HDC hdc,
										  LPRECT rect, LPARAM lParam) {
	MonitorEnumContext *ctx;
	MonitorState *state;
	(void)hdc;
	(void)rect;

	ctx = (MonitorEnumContext *)lParam;
	if (ctx->index >= NUM_MONITORS)
		return FALSE;

	state = &ctx->virgo->monitors[ctx->index];
	SecureZeroMemory(state, sizeof(*state));

	state->monitor = monitor;
	ctx->index++;
	return TRUE;
}

static void virgo_init_monitors(Virgo *v) {
	MonitorEnumContext ctx;
	MonitorState *state;
	POINT pt;

	SecureZeroMemory(&ctx, sizeof(ctx));
	ctx.virgo = v;

	EnumDisplayMonitors(NULL, NULL, find_monitor_handles, (LPARAM)&ctx);

	if (ctx.index == 0) {
		state = &v->monitors[0];
		SecureZeroMemory(state, sizeof(*state));
		pt.x = 0;
		pt.y = 0;
		state->monitor = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
	}
}

static unsigned virgo_monitor_index(Virgo *v, HMONITOR monitor) {
	unsigned i;
	for (i = 0; i < NUM_MONITORS; i++)
		if (v->monitors[i].monitor == monitor)
			return i;

	return 0; /* Fallback to 0 if not found */
}

static unsigned virgo_monitor_from_hwnd(Virgo *v, HWND hwnd) {
	HMONITOR monitor;

	monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
	return virgo_monitor_index(v, monitor);
}

static BOOL virgo_contains_window(Virgo *v, HWND hwnd) {
	unsigned m, d, w;
	MonitorState *state;
	Windows *desk;

	for (m = 0; m < NUM_MONITORS; m++) {
		state = &v->monitors[m];
		for (d = 0; d < NUM_DESKTOPS; d++) {
			desk = &state->desktops[d];
			for (w = 0; w < desk->count; w++)
				if (desk->windows[w] == hwnd)
					return TRUE;
		}
	}

	return FALSE;
}

static unsigned virgo_active_monitor(Virgo *v) {
	POINT pt;
	HMONITOR monitor;

	if (GetCursorPos(&pt)) {
		monitor = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
		return virgo_monitor_index(v, monitor);
	}
	return virgo_monitor_from_hwnd(v, GetForegroundWindow());
}

static void windows_mod(Windows *wins, unsigned state) {
	unsigned i;
	for (i = 0; i < wins->count; i++) {
		ShowWindow(wins->windows[i], state);
	}
}

static void windows_show(Windows *wins) { windows_mod(wins, SW_SHOW); }

static void windows_hide(Windows *wins) { windows_mod(wins, SW_HIDE); }

static void windows_add(Windows *wins, HWND hwnd) {
	if (wins->count >= sb_count(wins->windows)) {
		sb_push(wins->windows, hwnd);
	} else {
		wins->windows[wins->count] = hwnd;
	}
	wins->count++;
}

static void windows_del(Windows *wins, HWND hwnd) {
	unsigned i, e;
	for (i = 0; i < wins->count; i++) {
		if (wins->windows[i] != hwnd) {
			continue;
		}
		if (i != wins->count - 1) {
			for (e = i; e < wins->count - 1; e++) {
				wins->windows[e] = wins->windows[e + 1];
			}
		}
		wins->count--;
		break;
	}
}

static unsigned is_valid_window(HWND hwnd) {
	WINDOWINFO wi;
	wi.cbSize = sizeof(wi);
	GetWindowInfo(hwnd, &wi);
	return (wi.dwStyle & WS_VISIBLE) && !(wi.dwExStyle & WS_EX_TOOLWINDOW);
}

static void register_hotkey(unsigned id, unsigned mod, unsigned vk) {
	if (!RegisterHotKey(NULL, id, mod, vk)) {
		MessageBox(NULL, "could not register hotkey", "error",
				   MB_ICONEXCLAMATION);
		ExitProcess(1);
	}
}

static BOOL CALLBACK find_window_handles(HWND hwnd, LPARAM lParam) {
	unsigned monitor_idx;
	Virgo *v;
	MonitorState *state;

	v = (Virgo *)lParam;
	if (!is_valid_window(hwnd))
		return TRUE;
	if (virgo_contains_window(v, hwnd))
		return TRUE;

	monitor_idx = virgo_monitor_from_hwnd(v, hwnd);
	state = &v->monitors[monitor_idx];
	windows_add(&state->desktops[state->current], hwnd);
	return TRUE;
}

static void virgo_update(Virgo *v) {
	unsigned m, d, w, n;
	HWND hwnd;
	Windows *desk;
	MonitorState *state;

	for (m = 0; m < NUM_MONITORS; m++) {
		state = &v->monitors[m];
		for (d = 0; d < NUM_DESKTOPS; d++) {
			desk = &state->desktops[d];
			w = 0;
			while (w < desk->count) {
				hwnd = desk->windows[w];
				if (!GetWindowThreadProcessId(hwnd, NULL)) {
					windows_del(desk, hwnd);
					continue;
				}
				if (d == state->current && !IsWindowVisible(hwnd)) {
					windows_del(desk, hwnd);
					continue;
				}
				n = virgo_monitor_from_hwnd(v, hwnd);
				if (n != m) {
					windows_del(desk, hwnd);
					windows_add(
						&v->monitors[n].desktops[v->monitors[n].current], hwnd);
					ShowWindow(hwnd, SW_SHOW);
					continue;
				}
				w++;
			}
		}
	}
	EnumWindows(find_window_handles, (LPARAM)v);
}

static void virgo_toggle_hotkeys(Virgo *v) {
	unsigned i;
	v->handle_hotkeys = !v->handle_hotkeys;
	if (v->handle_hotkeys) {
		for (i = 0; i < NUM_DESKTOPS; i++) {
			register_hotkey(i * 2, MOD_ALT | MOD_NOREPEAT, i + 1 + '0');
			register_hotkey(i * 2 + 1, MOD_CONTROL | MOD_NOREPEAT, i + 1 + '0');
		}
	} else {
		for (i = 0; i < NUM_DESKTOPS; i++) {
			UnregisterHotKey(NULL, i * 2);
			UnregisterHotKey(NULL, i * 2 + 1);
		}
	}
}

static void virgo_init(Virgo *v) {
	unsigned i;
	virgo_init_monitors(v);
	v->handle_hotkeys = 1;
	for (i = 0; i < NUM_DESKTOPS; i++) {
		register_hotkey(i * 2, MOD_ALT | MOD_NOREPEAT, i + 1 + '0');
		register_hotkey(i * 2 + 1, MOD_CONTROL | MOD_NOREPEAT, i + 1 + '0');
	}
	register_hotkey(i * 2, MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT,
					'Q');
	register_hotkey(i * 2 + 1, MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT,
					'S');
	trayicon_init(&v->trayicon);
}

static void virgo_deinit(Virgo *v) {
	unsigned m, d;
	for (m = 0; m < NUM_MONITORS; m++) {
		for (d = 0; d < NUM_DESKTOPS; d++) {
			windows_show(&v->monitors[m].desktops[d]);
			sb_free(v->monitors[m].desktops[d].windows);
		}
	}
	trayicon_deinit(&v->trayicon);
}

static void virgo_move_to_desk(Virgo *v, unsigned desk) {
	unsigned monitor_idx;
	HWND hwnd;
	MonitorState *state;
	virgo_update(v);
	hwnd = GetForegroundWindow();
	if (!hwnd || !is_valid_window(hwnd)) {
		return;
	}
	monitor_idx = virgo_monitor_from_hwnd(v, hwnd);
	state = &v->monitors[monitor_idx];
	if (state->current == desk)
		return;
	windows_del(&state->desktops[state->current], hwnd);
	windows_add(&state->desktops[desk], hwnd);
	ShowWindow(hwnd, SW_HIDE);
}

static void virgo_go_to_desk(Virgo *v, unsigned desk) {
	unsigned monitor_idx;
	MonitorState *state;
	monitor_idx = virgo_active_monitor(v);
	state = &v->monitors[monitor_idx];
	if (state->current == desk) {
		return;
	}
	virgo_update(v);
	state->lastFocus[state->current] = GetForegroundWindow();
	windows_hide(&state->desktops[state->current]);
	trayicon_set(&v->trayicon, desk);
	windows_show(&state->desktops[desk]);
	state->current = desk;
	SetForegroundWindow(state->lastFocus[state->current]);
}

static void virgo_save_state(Virgo *v) {
	unsigned m, d;
	Windows *desk;
	MonitorState *state;
	DWORD written;
	HANDLE hFile = CreateFile("virgo.state", GENERIC_WRITE, 0, NULL,
							  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE)
		return;

	for (m = 0; m < NUM_MONITORS; m++) {
		state = &v->monitors[m];
		WriteFile(hFile, &state->current, sizeof(state->current), &written,
				  NULL);

		for (d = 0; d < NUM_DESKTOPS; d++) {
			desk = &state->desktops[d];
			WriteFile(hFile, &desk->count, sizeof(desk->count), &written, NULL);
			if (desk->count > 0)
				WriteFile(hFile, desk->windows, sizeof(HWND) * desk->count,
						  &written, NULL);
		}
	}

	CloseHandle(hFile);
}

static void virgo_load_state(Virgo *v) {
	unsigned m, d, w, count;
	HWND hwnd;
	Windows *desk;
	MonitorState *state;
	DWORD read;
	HANDLE hFile = CreateFile("virgo.state", GENERIC_READ, FILE_SHARE_READ,
							  NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE)
		return;

	for (m = 0; m < NUM_MONITORS; m++) {
		state = &v->monitors[m];
		ReadFile(hFile, &state->current, sizeof(state->current), &read, NULL);

		for (d = 0; d < NUM_DESKTOPS; d++) {
			desk = &state->desktops[d];
			ReadFile(hFile, &count, sizeof(count), &read, NULL);

			for (w = 0; w < count; w++) {
				ReadFile(hFile, &hwnd, sizeof(hwnd), &read, NULL);
				if (read == sizeof(hwnd) && is_valid_window(hwnd)) {
					windows_add(desk, hwnd);

					if (d == state->current)
						ShowWindow(hwnd, SW_SHOW);
					else
						ShowWindow(hwnd, SW_HIDE);
				}
			}
		}
	}

	CloseHandle(hFile);
	DeleteFile("virgo.state");
}

void __main(void) __asm__("__main");
void __main(void) {
	Virgo v /* = {0}*/; /* On x86-64 this forces a call to memset */
	MSG msg;

	/* Zeros out Virgo v without calling memset */
	SecureZeroMemory(&v, sizeof(v));

	virgo_init(&v);
	virgo_load_state(&v);
	trayicon_set(&v.trayicon, v.monitors[virgo_active_monitor(&v)].current);
	while (GetMessage(&msg, NULL, 0, 0)) {
		if (msg.message != WM_HOTKEY) {
			continue;
		}
		if (msg.wParam == NUM_DESKTOPS * 2) {
			break;
		}
		if (msg.wParam == NUM_DESKTOPS * 2 + 1) {
			virgo_toggle_hotkeys(&v);
		} else if (msg.wParam % 2 == 0) {
			virgo_go_to_desk(&v, msg.wParam / 2);
		} else {
			virgo_move_to_desk(&v, (msg.wParam - 1) / 2);
		}
	}
	virgo_update(&v);
	virgo_save_state(&v);
	virgo_deinit(&v);
	ExitProcess(0);
}
