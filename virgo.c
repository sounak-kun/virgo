#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#ifndef MOD_NOREPEAT
#define MOD_NOREPEAT 0x4000
#endif

#define NUM_DESKTOPS 4
#define NUM_MONITORS 4

#define NUM_WIN_HOOK 3

char *icons[NUM_MONITORS][NUM_DESKTOPS] = {
	{"11.ico", "12.ico", "13.ico", "14.ico"},
	{"21.ico", "22.ico", "23.ico", "24.ico"},
	{"31.ico", "32.ico", "33.ico", "34.ico"},
	{"41.ico", "42.ico", "43.ico", "44.ico"}};

typedef struct {
	HWND *windows;
	HWND lastFocus;
	unsigned count;
	unsigned capacity;
} Windows;

typedef struct {
	NOTIFYICONDATA nid;
	HWND hwnd;
	unsigned bitmapWidth;
} Trayicon;

typedef struct {
	unsigned current;
	HMONITOR monitor;
	Windows desktops[NUM_DESKTOPS];
} MonitorState;

typedef struct {
	unsigned handle_hotkeys;
	unsigned monitor_count;
	Trayicon trayicons[NUM_MONITORS];
	MonitorState monitors[NUM_MONITORS];
	HWINEVENTHOOK hooks[NUM_WIN_HOOK];
} Virgo;

static Virgo virgo; /* This is fine as .bss section is zero initialized */

static void trayicon_draw(Trayicon *t, unsigned monitor, unsigned number) {
	ExtractIconEx(icons[monitor][number], 0, &t->nid.hIcon, NULL, 1);
}

static void trayicon_init() {
	int m;
	Trayicon *t;
	for (m = 0; m < virgo.monitor_count; m++) {
		t = &virgo.trayicons[m];
		t->hwnd = CreateWindowA("STATIC", "virgo", 0, 0, 0, 0, 0, NULL, NULL,
								NULL, NULL);
		t->bitmapWidth = GetSystemMetrics(SM_CXSMICON);
		t->nid.cbSize = sizeof(t->nid);
		t->nid.hWnd = t->hwnd;
		t->nid.uID = 100;
		t->nid.uFlags = NIF_ICON;
		trayicon_draw(t, m, 0);
		Shell_NotifyIcon(NIM_ADD, &t->nid);
	}
}

static void trayicon_set(unsigned monitor, unsigned number) {
	Trayicon *t;
	t = &virgo.trayicons[monitor];
	DestroyIcon(t->nid.hIcon);
	trayicon_draw(t, monitor, number);
	Shell_NotifyIcon(NIM_MODIFY, &t->nid);
}

static void trayicon_deinit() {
	int m;
	Trayicon *t;
	for (m = 0; m < virgo.monitor_count; m++) {
		t = &virgo.trayicons[m];
		Shell_NotifyIcon(NIM_DELETE, &t->nid);
		DestroyIcon(t->nid.hIcon);
		DestroyWindow(t->hwnd);
	}
}

static BOOL CALLBACK find_monitor_handles(HMONITOR monitor, HDC hdc,
										  LPRECT rect, LPARAM lParam) {
	MonitorState *state;
	(void)hdc;
	(void)rect;
	(void)lParam;

	if (virgo.monitor_count >= NUM_MONITORS)
		return FALSE;

	state = &virgo.monitors[virgo.monitor_count];
	SecureZeroMemory(state, sizeof(*state));

	state->monitor = monitor;
	virgo.monitor_count++;
	return TRUE;
}

static void virgo_init_monitors() {
	MonitorState *state;
	POINT pt;

	virgo.monitor_count = 0;

	EnumDisplayMonitors(NULL, NULL, find_monitor_handles, (LPARAM)NULL);

	if (virgo.monitor_count == 0) {
		state = &virgo.monitors[0];
		SecureZeroMemory(state, sizeof(*state));
		pt.x = 0;
		pt.y = 0;
		state->monitor = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
		virgo.monitor_count++;
	}
}

static unsigned virgo_monitor_index(HMONITOR monitor) {
	unsigned i;
	for (i = 0; i < NUM_MONITORS; i++)
		if (virgo.monitors[i].monitor == monitor)
			return i;

	return 0; /* Fallback to 0 if not found */
}

static unsigned virgo_monitor_from_hwnd(HWND hwnd) {
	HMONITOR monitor;

	monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
	return virgo_monitor_index(monitor);
}

static BOOL virgo_monitor_desk_from_hwnd_local(HWND hwnd, MonitorState **state,
											   Windows **desk) {
	unsigned m, d, w;
	MonitorState *_st;
	Windows *_dsk;

	for (m = 0; m < NUM_MONITORS; m++) {
		_st = &virgo.monitors[m];
		for (d = 0; d < NUM_DESKTOPS; d++) {
			_dsk = &_st->desktops[d];
			for (w = 0; w < _dsk->count; w++) {
				if (_dsk->windows[w] == hwnd) {
					if (state)
						*state = _st;
					if (desk)
						*desk = _dsk;
					return TRUE;
				}
			}
		}
	}

	return FALSE;
}

static BOOL virgo_contains_window(HWND hwnd) {
	return virgo_monitor_desk_from_hwnd_local(hwnd, NULL, NULL);
}

static unsigned virgo_active_monitor() {
	POINT pt;
	HMONITOR monitor;

	if (GetCursorPos(&pt)) {
		monitor = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
		return virgo_monitor_index(monitor);
	}
	return virgo_monitor_from_hwnd(GetForegroundWindow());
}

static void windows_buffer_grow(Windows *wins) {
	unsigned target = (wins && wins->capacity) ? 2 * wins->capacity : 4;
	HWND *new;
	if (wins->windows) {
		new = HeapReAlloc(GetProcessHeap(), 0, wins->windows,
						  sizeof(HWND) * target);
	} else {
		new = HeapAlloc(GetProcessHeap(), 0, sizeof(HWND) * target);
	}
	if (new) {
		wins->windows = new;
		wins->capacity = target;
	} else {
		MessageBox(NULL, "heap allocation error", "error", MB_ICONEXCLAMATION);
		ExitProcess(1);
	}
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
	if (wins->count >= wins->capacity)
		windows_buffer_grow(wins);
	wins->windows[wins->count++] = hwnd;
}

static void windows_del(Windows *wins, HWND hwnd) {
	unsigned i;
	for (i = 0; i < wins->count; i++) {
		if (wins->windows[i] == hwnd) {
			wins->windows[i] = wins->windows[--wins->count];
			break;
		}
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
		ExitProcess(2);
	}
}

static BOOL CALLBACK find_window_handles(HWND hwnd, LPARAM lParam) {
	unsigned monitor_idx;
	MonitorState *state;
	(void)lParam;

	if (!is_valid_window(hwnd))
		return TRUE;
	if (virgo_contains_window(hwnd))
		return TRUE;

	monitor_idx = virgo_monitor_from_hwnd(hwnd);
	state = &virgo.monitors[monitor_idx];
	windows_add(&state->desktops[state->current], hwnd);
	return TRUE;
}

static void virgo_update() {
	unsigned m, d, w, n;
	HWND hwnd;
	Windows *desk;
	MonitorState *state;

	for (m = 0; m < NUM_MONITORS; m++) {
		state = &virgo.monitors[m];
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
				n = virgo_monitor_from_hwnd(hwnd);
				if (n != m) {
					windows_del(desk, hwnd);
					windows_add(
						&virgo.monitors[n].desktops[virgo.monitors[n].current],
						hwnd);
					ShowWindow(hwnd, SW_SHOW);
					continue;
				}
				w++;
			}
		}
	}
	EnumWindows(find_window_handles, (LPARAM)NULL);
}

static void virgo_toggle_hotkeys() {
	unsigned i;
	virgo.handle_hotkeys = !virgo.handle_hotkeys;
	if (virgo.handle_hotkeys) {
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

static void CALLBACK virgo_update_window_focus_on_new_foreground(
	HWINEVENTHOOK hWinEventHook, DWORD event, HWND hwnd, LONG idObject,
	LONG idChild, DWORD dwEventThread, DWORD dwmsEventTime) {
	unsigned monitor_idx;
	MonitorState *state;
	(void)hWinEventHook;
	(void)event;
	(void)dwEventThread;
	(void)dwmsEventTime;

	/* Only trigger on top level windows */
	if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF)
		return;

	if (is_valid_window(hwnd)) {
		monitor_idx = virgo_monitor_from_hwnd(hwnd);
		state = &virgo.monitors[monitor_idx];
		state->desktops[state->current].lastFocus = hwnd;
	}
}

static void CALLBACK virgo_remove_window_from_monitor_on_move(
	HWINEVENTHOOK hWinEventHook, DWORD event, HWND hwnd, LONG idObject,
	LONG idChild, DWORD dwEventThread, DWORD dwmsEventTime) {
	Windows *desk;
	MonitorState *state;
	(void)hWinEventHook;
	(void)event;
	(void)dwEventThread;
	(void)dwmsEventTime;

	/* Only trigger on top level windows */
	if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF)
		return;

	if (is_valid_window(hwnd)) {
		while (virgo_monitor_desk_from_hwnd_local(hwnd, &state, &desk)) {
			windows_del(desk, hwnd);
			if (desk->lastFocus == hwnd)
				desk->lastFocus = NULL;
		}
	}
}

static void CALLBACK virgo_add_window_to_monitor_on_move(
	HWINEVENTHOOK hWinEventHook, DWORD event, HWND hwnd, LONG idObject,
	LONG idChild, DWORD dwEventThread, DWORD dwmsEventTime) {
	unsigned monitor_idx;
	Windows *desk;
	MonitorState *state;
	(void)hWinEventHook;
	(void)event;
	(void)dwEventThread;
	(void)dwmsEventTime;

	/* Only trigger on top level windows */
	if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF)
		return;

	if (is_valid_window(hwnd)) {
		while (virgo_monitor_desk_from_hwnd_local(hwnd, NULL, &desk)) {
			windows_del(desk, hwnd);
		}
		monitor_idx = virgo_monitor_from_hwnd(hwnd);
		state = &virgo.monitors[monitor_idx];
		desk = &state->desktops[state->current];
		windows_add(desk, hwnd);
		desk->lastFocus = hwnd;
	}
}

static void virgo_init() {
	unsigned i;
	virgo_init_monitors();

	virgo.hooks[0] =
		SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, NULL,
						virgo_update_window_focus_on_new_foreground, 0, 0,
						WINEVENT_OUTOFCONTEXT);
	virgo.hooks[1] = SetWinEventHook(
		EVENT_SYSTEM_MOVESIZESTART, EVENT_SYSTEM_MOVESIZESTART, NULL,
		virgo_remove_window_from_monitor_on_move, 0, 0, WINEVENT_OUTOFCONTEXT);
	virgo.hooks[2] = SetWinEventHook(
		EVENT_SYSTEM_MOVESIZEEND, EVENT_SYSTEM_MOVESIZEEND, NULL,
		virgo_add_window_to_monitor_on_move, 0, 0, WINEVENT_OUTOFCONTEXT);

	virgo.handle_hotkeys = 1;
	for (i = 0; i < NUM_DESKTOPS; i++) {
		register_hotkey(i * 2, MOD_ALT | MOD_NOREPEAT, i + 1 + '0');
		register_hotkey(i * 2 + 1, MOD_CONTROL | MOD_NOREPEAT, i + 1 + '0');
	}
	register_hotkey(i * 2, MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT,
					'Q');
	register_hotkey(i * 2 + 1, MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT,
					'S');
	trayicon_init();
}

static void virgo_deinit() {
	unsigned m, d, h;
	for (m = 0; m < NUM_MONITORS; m++) {
		for (d = 0; d < NUM_DESKTOPS; d++) {
			windows_show(&virgo.monitors[m].desktops[d]);
			HeapFree(GetProcessHeap(), 0,
					 virgo.monitors[m].desktops[d].windows);
		}
	}
	for (h = 0; h < NUM_WIN_HOOK; h++) {
		if (virgo.hooks[h])
			UnhookWinEvent(virgo.hooks[h]);
	}
	trayicon_deinit();
}

static void virgo_move_to_desk(unsigned desk) {
	unsigned monitor_idx;
	HWND hwnd;
	MonitorState *state;
	virgo_update();
	hwnd = GetForegroundWindow();
	if (!hwnd || !is_valid_window(hwnd)) {
		return;
	}
	monitor_idx = virgo_monitor_from_hwnd(hwnd);
	state = &virgo.monitors[monitor_idx];
	if (state->current == desk)
		return;
	windows_del(&state->desktops[state->current], hwnd);
	windows_add(&state->desktops[desk], hwnd);
	ShowWindow(hwnd, SW_HIDE);
}

static void virgo_go_to_desk(unsigned desk) {
	unsigned monitor_idx;
	MonitorState *state;
	monitor_idx = virgo_active_monitor();
	state = &virgo.monitors[monitor_idx];
	if (state->current == desk) {
		return;
	}
	virgo_update();
	windows_hide(&state->desktops[state->current]);
	trayicon_set(monitor_idx, desk);
	windows_show(&state->desktops[desk]);
	state->current = desk;
	SetForegroundWindow(state->desktops[state->current].lastFocus);
}

static void virgo_save_state() {
	unsigned m, d;
	Windows *desk;
	MonitorState *state;
	DWORD written;
	HANDLE hFile = CreateFile("virgo.state", GENERIC_WRITE, 0, NULL,
							  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE)
		return;

	WriteFile(hFile, &virgo.monitor_count, sizeof(virgo.monitor_count),
			  &written, NULL);

	for (m = 0; m < NUM_MONITORS; m++) {
		state = &virgo.monitors[m];
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

static void virgo_load_state() {
	unsigned m, d, w, count, old_monitor_count;
	HWND hwnd;
	Windows *desk;
	MonitorState *state;
	DWORD read;
	HANDLE hFile = CreateFile("virgo.state", GENERIC_READ, FILE_SHARE_READ,
							  NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE)
		return;

	ReadFile(hFile, &old_monitor_count, sizeof(virgo.monitor_count), &read,
			 NULL);

	if (old_monitor_count <= virgo.monitor_count) {
		for (m = 0; m < old_monitor_count; m++) {
			state = &virgo.monitors[m];
			ReadFile(hFile, &state->current, sizeof(state->current), &read,
					 NULL);
			trayicon_set(m, state->current);

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
	}

	CloseHandle(hFile);
	DeleteFile("virgo.state");
}

void __main(void) __asm__("__main");
void __main(void) {
	MSG msg;

	virgo_init();
	virgo_load_state();
	while (GetMessage(&msg, NULL, 0, 0)) {
		if (msg.message != WM_HOTKEY) {
			continue;
		}
		if (msg.wParam == NUM_DESKTOPS * 2) {
			break;
		}
		if (msg.wParam == NUM_DESKTOPS * 2 + 1) {
			virgo_toggle_hotkeys();
		} else if (msg.wParam % 2 == 0) {
			virgo_go_to_desk(msg.wParam / 2);
		} else {
			virgo_move_to_desk((msg.wParam - 1) / 2);
		}
	}
	virgo_update();
	virgo_save_state();
	virgo_deinit();
	ExitProcess(0);
}
