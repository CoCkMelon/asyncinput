// Agent: Agent Mode, Date: 2025-08-21, Observation: Windows backend using Raw Input with a hidden message window on a worker thread
#include "asyncinput.h"
#ifdef _WIN32
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RING_SIZE 1024

struct ringbuf {
	struct ni_event ev[RING_SIZE];
	int head;
	int tail;
	CRITICAL_SECTION lock;
};

struct keyringbuf {
	struct ni_key_event ev[RING_SIZE];
	int head;
	int tail;
	CRITICAL_SECTION lock;
};

static struct {
	int initialized;
	volatile int stop;
	HANDLE thread;
	DWORD thread_id;
	HWND hwnd;
	struct ringbuf queue;
	ni_callback cb;
	void *cb_user;
	/* placeholder filter API on Windows: we don't have per-device open; we can filter by name/vendor in future */
	ni_device_filter filter;
	void *filter_user;
	int device_count;
	/* high-level key queue (xkb not available on Windows, we emit minimal text for WM_CHAR) */
	struct keyringbuf key_queue;
	ni_key_callback key_cb;
	void *key_cb_user;
} g;

static LONGLONG now_ns(void)
{
	LARGE_INTEGER fq, ct;
	QueryPerformanceFrequency(&fq);
	QueryPerformanceCounter(&ct);
	double s = (double)ct.QuadPart / (double)fq.QuadPart;
	return (LONGLONG)(s * 1000000000.0);
}

static void ring_init(struct ringbuf *r)
{
	memset(r, 0, sizeof(*r));
	InitializeCriticalSection(&r->lock);
}

static void ring_push(struct ringbuf *r, const struct ni_event *ev)
{
	EnterCriticalSection(&r->lock);
	int next = (r->head + 1) % RING_SIZE;
	if (next != r->tail) {
		r->ev[r->head] = *ev;
		r->head = next;
	}
	LeaveCriticalSection(&r->lock);
}

static int ring_pop_many(struct ringbuf *r, struct ni_event *out, int max)
{
	int n = 0;
	EnterCriticalSection(&r->lock);
	while (n < max && r->tail != r->head) {
		out[n++] = r->ev[r->tail];
		r->tail = (r->tail + 1) % RING_SIZE;
	}
	LeaveCriticalSection(&r->lock);
	return n;
}

static void keyring_init(struct keyringbuf *r)
{
	memset(r, 0, sizeof(*r));
	InitializeCriticalSection(&r->lock);
}

static void keyring_push(struct keyringbuf *r, const struct ni_key_event *ev)
{
	EnterCriticalSection(&r->lock);
	int next = (r->head + 1) % RING_SIZE;
	if (next != r->tail) {
		r->ev[r->head] = *ev;
		r->head = next;
	}
	LeaveCriticalSection(&r->lock);
}

static int keyring_pop_many(struct keyringbuf *r, struct ni_key_event *out, int max)
{
	int n = 0;
	EnterCriticalSection(&r->lock);
	while (n < max && r->tail != r->head) {
		out[n++] = r->ev[r->tail];
		r->tail = (r->tail + 1) % RING_SIZE;
	}
	LeaveCriticalSection(&r->lock);
	return n;
}

static void emit_or_queue(const struct ni_event *ev)
{
	if (g.cb) g.cb(ev, g.cb_user); else ring_push(&g.queue, ev);
}

/* Raw Input handling */
static void handle_rawinput(HRAWINPUT hri)
{
	UINT size = 0;
	if (GetRawInputData(hri, RID_INPUT, NULL, &size, sizeof(RAWINPUTHEADER)) != 0 || size == 0)
		return;
	RAWINPUT *ri = (RAWINPUT*)malloc(size);
	if (!ri) return;
	if (GetRawInputData(hri, RID_INPUT, ri, &size, sizeof(RAWINPUTHEADER)) != size) { free(ri); return; }
	LONGLONG ts = now_ns();
	if (ri->header.dwType == RIM_TYPEKEYBOARD) {
		const RAWKEYBOARD *kb = &ri->data.keyboard;
		struct ni_event ev = {0};
		ev.device_id = (int)(intptr_t)ri->header.hDevice; /* not stable across sessions but consistent during run */
		ev.timestamp_ns = ts;
		ev.type = NI_EV_KEY;
		/* Map Windows VKey/MakeCode to NI_KEY_*: we use MakeCode (scancode) which is close to evdev set1 */
		ev.code = (int)kb->MakeCode; /* best-effort scancode */
		if (kb->Flags & RI_KEY_BREAK) ev.value = 0; else ev.value = 1;
		emit_or_queue(&ev);
		/* Produce basic text on keydown via WM_CHAR handled separately in message loop */
	} else if (ri->header.dwType == RIM_TYPEMOUSE) {
		const RAWMOUSE *m = &ri->data.mouse;
		struct ni_event ev = {0};
		ev.device_id = (int)(intptr_t)ri->header.hDevice;
		ev.timestamp_ns = ts;
		if (m->usFlags & MOUSE_MOVE_RELATIVE) {
			if (m->lLastX) { ev.type = NI_EV_REL; ev.code = NI_REL_X; ev.value = (int)m->lLastX; emit_or_queue(&ev); }
			if (m->lLastY) { ev.type = NI_EV_REL; ev.code = NI_REL_Y; ev.value = -(int)m->lLastY; emit_or_queue(&ev); }
		}
		if (m->usButtonFlags) {
			if (m->usButtonFlags & RI_MOUSE_LEFT_BUTTON_DOWN)  { ev.type = NI_EV_KEY; ev.code = NI_BTN_LEFT; ev.value = 1; emit_or_queue(&ev); }
			if (m->usButtonFlags & RI_MOUSE_LEFT_BUTTON_UP)    { ev.type = NI_EV_KEY; ev.code = NI_BTN_LEFT; ev.value = 0; emit_or_queue(&ev); }
			if (m->usButtonFlags & RI_MOUSE_RIGHT_BUTTON_DOWN) { ev.type = NI_EV_KEY; ev.code = NI_BTN_RIGHT; ev.value = 1; emit_or_queue(&ev); }
			if (m->usButtonFlags & RI_MOUSE_RIGHT_BUTTON_UP)   { ev.type = NI_EV_KEY; ev.code = NI_BTN_RIGHT; ev.value = 0; emit_or_queue(&ev); }
			if (m->usButtonFlags & RI_MOUSE_MIDDLE_BUTTON_DOWN){ ev.type = NI_EV_KEY; ev.code = NI_BTN_MIDDLE; ev.value = 1; emit_or_queue(&ev); }
			if (m->usButtonFlags & RI_MOUSE_MIDDLE_BUTTON_UP)  { ev.type = NI_EV_KEY; ev.code = NI_BTN_MIDDLE; ev.value = 0; emit_or_queue(&ev); }
			if (m->usButtonFlags & RI_MOUSE_BUTTON_4_DOWN)     { ev.type = NI_EV_KEY; ev.code = NI_BTN_SIDE; ev.value = 1; emit_or_queue(&ev); }
			if (m->usButtonFlags & RI_MOUSE_BUTTON_4_UP)       { ev.type = NI_EV_KEY; ev.code = NI_BTN_SIDE; ev.value = 0; emit_or_queue(&ev); }
			if (m->usButtonFlags & RI_MOUSE_BUTTON_5_DOWN)     { ev.type = NI_EV_KEY; ev.code = NI_BTN_EXTRA; ev.value = 1; emit_or_queue(&ev); }
			if (m->usButtonFlags & RI_MOUSE_BUTTON_5_UP)       { ev.type = NI_EV_KEY; ev.code = NI_BTN_EXTRA; ev.value = 0; emit_or_queue(&ev); }
			if (m->usButtonFlags & RI_MOUSE_WHEEL) {
				SHORT dz = (SHORT)m->usButtonData;
				ev.type = NI_EV_REL; ev.code = NI_REL_WHEEL; ev.value = (int)(dz / WHEEL_DELTA); emit_or_queue(&ev);
			}
		}
	}
	free(ri);
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg) {
	case WM_INPUT:
		handle_rawinput((HRAWINPUT)lParam);
		break;
	case WM_CHAR: {
		/* Emit a high-level key event with UTF-8 text for simple characters */
		if (!g.key_cb) break;
		struct ni_key_event kev; memset(&kev, 0, sizeof(kev));
		kev.device_id = -1;
		kev.down = 1;
		kev.timestamp_ns = now_ns();
		/* Convert UTF-16 wchar in wParam to UTF-8 */
		wchar_t wc = (wchar_t)wParam;
		int n = WideCharToMultiByte(CP_UTF8, 0, &wc, 1, kev.text, (int)sizeof(kev.text)-1, NULL, NULL);
		if (n > 0) kev.text[n] = '\0'; else kev.text[0] = '\0';
		g.key_cb(&kev, g.key_cb_user);
		break; }
	case WM_DESTROY:
		PostQuitMessage(0);
		break;
	default:
		return DefWindowProc(hwnd, msg, wParam, lParam);
	}
	return 0;
}

static int create_message_window(void)
{
	WNDCLASSW wc = {0};
	wc.lpfnWndProc = wndproc;
	wc.hInstance = GetModuleHandle(NULL);
	wc.lpszClassName = L"AsyncInputHiddenWindow";
	RegisterClassW(&wc);
	HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, wc.hInstance, NULL);
	if (!hwnd) return -1;
	g.hwnd = hwnd;
	/* Register for raw input for keyboard and mouse */
	RAWINPUTDEVICE rid[2];
	rid[0].usUsagePage = 0x01; rid[0].usUsage = 0x06; /* Keyboard */
	rid[0].dwFlags = RIDEV_INPUTSINK; rid[0].hwndTarget = hwnd;
	rid[1].usUsagePage = 0x01; rid[1].usUsage = 0x02; /* Mouse */
	rid[1].dwFlags = RIDEV_INPUTSINK; rid[1].hwndTarget = hwnd;
	if (!RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE))) {
		DestroyWindow(hwnd); g.hwnd = NULL; return -1;
	}
	return 0;
}

static DWORD WINAPI worker_thread(LPVOID param)
{
	(void)param;
	if (create_message_window() != 0) return 0;
	/* We don't have explicit device list; treat as one logical keyboard and mouse present */
	g.device_count = 2;
	MSG msg;
	while (!g.stop) {
		while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
		WaitMessage();
	}
	DestroyWindow(g.hwnd); g.hwnd = NULL;
	return 0;
}

int ni_init(int flags)
{
	if (flags != 0) return -1;
	if (g.initialized) return 0;
	memset(&g, 0, sizeof(g));
	ring_init(&g.queue);
	keyring_init(&g.key_queue);
	g.stop = 0;
	g.thread = CreateThread(NULL, 0, worker_thread, NULL, 0, &g.thread_id);
	if (!g.thread) return -1;
	g.initialized = 1;
	return 0;
}

int ni_set_device_filter(ni_device_filter filter, void *user_data)
{
	/* No-op on Windows for now; we don't enumerate devices individually */
	g.filter = filter; g.filter_user = user_data; (void)filter; (void)user_data;
	return 0;
}

int ni_device_count(void)
{
	return g.device_count;
}

int ni_register_callback(ni_callback cb, void *user_data, int flags)
{
	if (!g.initialized || flags != 0) return -1;
	g.cb = cb; g.cb_user = user_data; return 0;
}

int ni_poll(struct ni_event *evts, int max_events)
{
	if (!g.initialized || !evts || max_events <= 0) return -1;
	return ring_pop_many(&g.queue, evts, max_events);
}

int ni_shutdown(void)
{
	if (!g.initialized) return 0;
	g.stop = 1;
	if (g.hwnd) PostMessage(g.hwnd, WM_CLOSE, 0, 0);
	WaitForSingleObject(g.thread, 2000);
	CloseHandle(g.thread); g.thread = NULL;
	g.initialized = 0;
	return 0;
}

int ni_register_key_callback(ni_key_callback cb, void *user_data, int flags)
{
	(void)flags;
	if (!g.initialized) return -1;
	g.key_cb = cb; g.key_cb_user = user_data; return 0;
}

int ni_poll_key_events(struct ni_key_event *evts, int max_events)
{
	if (!g.initialized || !evts || max_events <= 0) return -1;
	return keyring_pop_many(&g.key_queue, evts, max_events);
}

int ni_enable_xkb(int enabled)
{
	(void)enabled; return -1; /* Not supported on Windows */
}

int ni_set_xkb_names(const char *rules, const char *model, const char *layout,
                     const char *variant, const char *options)
{
	(void)rules; (void)model; (void)layout; (void)variant; (void)options; return -1;
}

int ni_enable_mice(int enabled)
{
	(void)enabled; return 0; /* Raw Input already handles mice */
}

#endif /* _WIN32 */

