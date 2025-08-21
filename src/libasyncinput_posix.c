// Agent: Agent Mode, Date: 2025-08-16, Observation: Initial linux MVP implementation, worker thread reads /dev/input/event* with epoll, ring buffer for polling
#include "asyncinput.h"
#if defined(ASYNCINPUT_HAVE_XKBCOMMON)
#include <xkbcommon/xkbcommon.h>
#endif

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
/* Non-Linux builds compile an empty translation unit here; platform code lives elsewhere. */


#define MAX_DEVICES 128
#define RING_SIZE 1024
#define MAX_EPOLL_EVENTS 16

struct device {
	int fd;
	int id;
	char path[128];
};

struct ringbuf {
	struct ni_event ev[RING_SIZE];
	int head;
	int tail;
	pthread_mutex_t lock;
};

struct keyringbuf {
	struct ni_key_event ev[RING_SIZE];
	int head;
	int tail;
	pthread_mutex_t lock;
};

static struct {
	int initialized;
	int epoll_fd;
	int inotify_fd;
	pthread_t thread;
	volatile int stop;
	struct device devices[MAX_DEVICES];
	int ndevi;
	pthread_mutex_t dev_lock;
	struct ringbuf queue;
	ni_callback cb;
	void *cb_user;
	ni_device_filter filter;
	void *filter_user;
	volatile long long rescan_until_ns;
	/* optional /dev/input/mice reader */
	int mice_enabled;
	int mice_fd;
	pthread_t mice_thread;
	/* xkb layer */
	int xkb_enabled;
#ifdef ASYNCINPUT_HAVE_XKBCOMMON
	struct xkb_context *xkb_ctx;
	struct xkb_keymap *xkb_keymap;
	struct xkb_state *xkb_state;
	char xkb_rules[32];
	char xkb_model[32];
	char xkb_layout[64];
	char xkb_variant[32];
	char xkb_options[128];
#endif
	struct keyringbuf key_queue;
	ni_key_callback key_cb;
	void *key_cb_user;
} g;

static long long
now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void
ring_init(struct ringbuf *r)
{
	memset(r, 0, sizeof(*r));
	pthread_mutex_init(&r->lock, NULL);
}

static void
ring_push(struct ringbuf *r, const struct ni_event *ev)
{
	pthread_mutex_lock(&r->lock);
	int next = (r->head + 1) % RING_SIZE;
	if (next != r->tail) {
		r->ev[r->head] = *ev;
		r->head = next;
	}
	pthread_mutex_unlock(&r->lock);
}

static int
ring_pop_many(struct ringbuf *r, struct ni_event *out, int max)
{
	int n = 0;
	pthread_mutex_lock(&r->lock);
	while (n < max && r->tail != r->head) {
		out[n++] = r->ev[r->tail];
		r->tail = (r->tail + 1) % RING_SIZE;
	}
	pthread_mutex_unlock(&r->lock);
	return n;
}

static inline void emit_or_queue(struct ni_event *ev)
{
	if (g.cb) g.cb(ev, g.cb_user); else ring_push(&g.queue, ev);
}

static void keyring_init(struct keyringbuf *r)
{
	memset(r, 0, sizeof(*r));
	pthread_mutex_init(&r->lock, NULL);
}

static void keyring_push(struct keyringbuf *r, const struct ni_key_event *ev)
{
	pthread_mutex_lock(&r->lock);
	int next = (r->head + 1) % RING_SIZE;
	if (next != r->tail) {
		r->ev[r->head] = *ev;
		r->head = next;
	}
	pthread_mutex_unlock(&r->lock);
}

static int keyring_pop_many(struct keyringbuf *r, struct ni_key_event *out, int max)
{
	int n = 0;
	pthread_mutex_lock(&r->lock);
	while (n < max && r->tail != r->head) {
		out[n++] = r->ev[r->tail];
		r->tail = (r->tail + 1) % RING_SIZE;
	}
	pthread_mutex_unlock(&r->lock);
	return n;
}


static void *mice_worker(void *arg)
{
	(void)arg;
	if (g.mice_fd < 0) {
		g.mice_fd = open("/dev/input/mice", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (g.mice_fd < 0) return NULL;
	}
	unsigned char buf[8];
	unsigned char pkt[4];
	int have = 0;
while (!g.stop && g.mice_enabled) {
		ssize_t r = read(g.mice_fd, buf, sizeof(buf));
		if (r <= 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) { usleep(1000); continue; }
			break;
		}
		for (ssize_t i = 0; i < r; i++) {
			pkt[have++] = buf[i];
			if (have >= 3) {
				int btn = pkt[0];
				signed char dx = (signed char)pkt[1];
				signed char dy = (signed char)pkt[2];
				struct ni_event ev = {0};
				ev.device_id = -2; /* pseudo mice id */
				ev.timestamp_ns = now_ns();
				/* buttons */
				ev.type = NI_EV_KEY; ev.code = NI_BTN_LEFT; ev.value = (btn & 0x1) ? 1 : 0; emit_or_queue(&ev);
				ev.code = NI_BTN_RIGHT; ev.value = (btn & 0x2) ? 1 : 0; emit_or_queue(&ev);
				ev.code = NI_BTN_MIDDLE; ev.value = (btn & 0x4) ? 1 : 0; emit_or_queue(&ev);
				/* Also emit a unified NI_EV_MOUSE button event for compatibility */
				if (btn & 0x1) { struct ni_event mev = {0}; mev.device_id = ev.device_id; mev.timestamp_ns = ev.timestamp_ns; mev.type = NI_EV_MOUSE; mev.code = NI_MOUSE_BUTTON; mev.extra = 1; mev.value = 1; emit_or_queue(&mev); }
				if (!(btn & 0x1)) { struct ni_event mev = {0}; mev.device_id = ev.device_id; mev.timestamp_ns = ev.timestamp_ns; mev.type = NI_EV_MOUSE; mev.code = NI_MOUSE_BUTTON; mev.extra = 1; mev.value = 0; emit_or_queue(&mev); }
				if (btn & 0x2) { struct ni_event mev = {0}; mev.device_id = ev.device_id; mev.timestamp_ns = ev.timestamp_ns; mev.type = NI_EV_MOUSE; mev.code = NI_MOUSE_BUTTON; mev.extra = 2; mev.value = 1; emit_or_queue(&mev); }
				if (!(btn & 0x2)) { struct ni_event mev = {0}; mev.device_id = ev.device_id; mev.timestamp_ns = ev.timestamp_ns; mev.type = NI_EV_MOUSE; mev.code = NI_MOUSE_BUTTON; mev.extra = 2; mev.value = 0; emit_or_queue(&mev); }
				if (btn & 0x4) { struct ni_event mev = {0}; mev.device_id = ev.device_id; mev.timestamp_ns = ev.timestamp_ns; mev.type = NI_EV_MOUSE; mev.code = NI_MOUSE_BUTTON; mev.extra = 3; mev.value = 1; emit_or_queue(&mev); }
				if (!(btn & 0x4)) { struct ni_event mev = {0}; mev.device_id = ev.device_id; mev.timestamp_ns = ev.timestamp_ns; mev.type = NI_EV_MOUSE; mev.code = NI_MOUSE_BUTTON; mev.extra = 3; mev.value = 0; emit_or_queue(&mev); }
				/* rel moves: dy inverted to match evdev coords */
				ev.type = NI_EV_REL; ev.code = NI_REL_X; ev.value = (int)dx; emit_or_queue(&ev);
				ev.type = NI_EV_REL; ev.code = NI_REL_Y; ev.value = -(int)dy; emit_or_queue(&ev);
				/* And a unified NI_EV_MOUSE move event */
				if (dx || dy) { struct ni_event mev = {0}; mev.device_id = ev.device_id; mev.timestamp_ns = ev.timestamp_ns; mev.type = NI_EV_MOUSE; mev.code = NI_MOUSE_MOVE; mev.x = (int)dx; mev.y = -(int)dy; emit_or_queue(&mev); }
				if (have >= 4) {
					signed char dz = (signed char)pkt[3];
					ev.type = NI_EV_REL; ev.code = NI_REL_WHEEL; ev.value = (int)dz; emit_or_queue(&ev);
				}
				have = 0;
			}
		}
	}
	if (g.mice_fd >= 0) { close(g.mice_fd); g.mice_fd = -1; }
	return NULL;
}


static int
fill_device_info(int fd, const char *path, struct ni_device_info *out)
{
	if (!out) return -1;
	memset(out, 0, sizeof(*out));
	strncpy(out->path, path ? path : "", sizeof(out->path)-1);
	struct input_id id = {0};
	if (ioctl(fd, EVIOCGID, &id) == 0) {
		out->bustype = id.bustype;
		out->vendor = id.vendor;
		out->product = id.product;
		out->version = id.version;
	}
	char name[256] = {0};
	if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) >= 0) {
		strncpy(out->name, name, sizeof(out->name)-1);
	}
	return 0;
}

static int
open_device_filtered(const char *path, int *out_devid)
{
	int devid = -1;
	/* deduce device id from path /dev/input/eventN */
	const char *p = strrchr(path, 't');
	if (p) {
		int n = atoi(p+1);
		if (n >= 0) devid = n;
	}
	int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return -1;
	if (g.filter) {
		struct ni_device_info info = {0};
		info.id = devid;
		(void)fill_device_info(fd, path, &info);
		if (!g.filter(&info, g.filter_user)) {
			close(fd);
			return -1;
		}
	}
	if (out_devid) *out_devid = devid;
	return fd;
}

static int has_device_id(int id) {
	for (int i = 0; i < g.ndevi; i++) if (g.devices[i].id == id) return 1;
	return 0;
}

static void add_device_fd(int fd, int devid, const char *path)
{
	pthread_mutex_lock(&g.dev_lock);
	struct device *dev = &g.devices[g.ndevi];
	dev->fd = fd;
	dev->id = devid;
	strncpy(dev->path, path ? path : "", sizeof(dev->path)-1);
	g.ndevi++;
	pthread_mutex_unlock(&g.dev_lock);
	
	/* Use device pointer directly in epoll to eliminate lookup */
	struct epoll_event ev = {0};
	ev.events = EPOLLIN;
	ev.data.ptr = dev;  /* Direct pointer instead of device_id */
	epoll_ctl(g.epoll_fd, EPOLL_CTL_ADD, fd, &ev);
}

static void remove_device_by_id(int devid)
{
	pthread_mutex_lock(&g.dev_lock);
	for (int i = 0; i < g.ndevi; i++) {
		if (g.devices[i].id == devid) {
			epoll_ctl(g.epoll_fd, EPOLL_CTL_DEL, g.devices[i].fd, NULL);
			close(g.devices[i].fd);
			/* compact array */
			g.devices[i] = g.devices[g.ndevi-1];
			g.ndevi--;
			break;
		}
	}
	pthread_mutex_unlock(&g.dev_lock);
}

static void scan_devices(void)
{
	char path[64];
	for (int i = 0; i < MAX_DEVICES; i++) {
		snprintf(path, sizeof(path), "/dev/input/event%d", i);
		if (has_device_id(i)) continue;
		int devid = -1;
		int fd = open_device_filtered(path, &devid);
		if (fd < 0) continue;
		add_device_fd(fd, devid >= 0 ? devid : i, path);
	}
}

static int
fd_to_device_id(int fd)
{
	for (int i = 0; i < g.ndevi; i++) {
		if (g.devices[i].fd == fd)
			return g.devices[i].id;
	}
	return -1;
}

#include <sys/inotify.h>

#define EPOLL_DATA_INOTIFY 0xFFFFFFFFu

static void handle_inotify_event(void)
{
	char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
	ssize_t len;
	for (;;) {
		len = read(g.inotify_fd, buf, sizeof(buf));
		if (len <= 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) break;
			break;
		}
		ssize_t off = 0;
		while (off < len) {
			struct inotify_event *ie = (struct inotify_event*)(buf + off);
			if (((ie->mask & IN_CREATE) || (ie->mask & IN_MOVED_TO)) && ie->len > 0) {
				if (strncmp(ie->name, "event", 5) == 0) {
					char path[128];
					snprintf(path, sizeof(path), "/dev/input/%s", ie->name);
					int devid = -1; int fd = open_device_filtered(path, &devid);
					if (fd >= 0) {
						add_device_fd(fd, devid, path);
					} else {
						/* schedule rescans for a short time to tolerate udev races */
						long long t = now_ns();
						g.rescan_until_ns = t + 3000000000LL; /* 3s */
					}
				}
			}
			if ((ie->mask & IN_DELETE) && ie->len > 0) {
				if (strncmp(ie->name, "event", 5) == 0) {
					int devid = atoi(ie->name + 5);
					remove_device_by_id(devid);
				}
			}
			off += sizeof(struct inotify_event) + ie->len;
		}
	}
}

static inline uint32_t mods_mask_from_state(void)
{
	uint32_t m = 0;
#ifdef ASYNCINPUT_HAVE_XKBCOMMON
	if (g.xkb_state) {
		if (xkb_state_mod_name_is_active(g.xkb_state, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE)) m |= 1u << 0;
		if (xkb_state_mod_name_is_active(g.xkb_state, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE)) m |= 1u << 1;
		if (xkb_state_mod_name_is_active(g.xkb_state, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE)) m |= 1u << 2;
		if (xkb_state_mod_name_is_active(g.xkb_state, XKB_MOD_NAME_LOGO, XKB_STATE_MODS_EFFECTIVE)) m |= 1u << 3;
	}
#endif
	return m;
}

static inline void maybe_emit_key_event(const struct ni_event *base)
{
	if (!g.xkb_enabled) return;
#ifdef ASYNCINPUT_HAVE_XKBCOMMON
	if (base->type != NI_EV_KEY) return;
	/* Evdev to XKB keycode conversion */
	uint32_t xkb_code = (uint32_t)base->code + 8u;
	if (g.xkb_state) {
		xkb_state_update_key(g.xkb_state, xkb_code, base->value ? XKB_KEY_DOWN : XKB_KEY_UP);
		struct ni_key_event kev = {0};
		kev.device_id = base->device_id;
		kev.timestamp_ns = base->timestamp_ns;
		kev.down = base->value ? 1 : 0;
		kev.mods = mods_mask_from_state();
		/* Keysym for this key */
		xkb_keysym_t sym = xkb_state_key_get_one_sym(g.xkb_state, xkb_code);
		kev.keysym = (uint32_t)sym;
		kev.text[0] = '\0';
		if (kev.down) {
			/* Get UTF-8 text (if any) */
			int n = xkb_state_key_get_utf8(g.xkb_state, xkb_code, kev.text, (int)sizeof(kev.text));
			(void)n; /* n  0 on error; zero means no text */
		}
if (g.key_cb) g.key_cb(&kev, g.key_cb_user); else keyring_push(&g.key_queue, &kev);
	}
#else
	(void)base;
#endif
}

static void *
worker(void *arg)
{
	(void)arg;
	struct epoll_event evs[MAX_EPOLL_EVENTS];
	struct input_event iev;

	while (!g.stop) {
		/* Opportunistically rescan while within rescan window (e.g., after IN_CREATE/MOVED_TO) */
		long long tnow = now_ns();
		if (g.rescan_until_ns && tnow < g.rescan_until_ns) {
			scan_devices();
		}
		int n = epoll_wait(g.epoll_fd, evs, MAX_EPOLL_EVENTS, 50);
		if (n <= 0)
			continue;
		for (int i = 0; i < n; i++) {
			/* Check for inotify events using special pointer value */
			if (evs[i].data.ptr == (void*)EPOLL_DATA_INOTIFY) {
				handle_inotify_event();
				continue;
			}
			/* Direct device pointer from epoll - no lookup needed! */
			struct device *dev = (struct device*)evs[i].data.ptr;
			if (!dev)
				continue;
			int fd = dev->fd;
			int devid = dev->id;
			for (;;) {
				ssize_t r = read(fd, &iev, sizeof(iev));
				if (r < 0) {
					if (errno == EAGAIN || errno == EWOULDBLOCK)
						break;
					/* device error, ignore for now */
					break;
				}
				if (r != (ssize_t)sizeof(iev))
					break;

				struct ni_event ev = {0};
				ev.device_id = devid;
				ev.type = iev.type;
				ev.code = iev.code;
				ev.value = iev.value;
				/* Use kernel event timestamp (input_event.time) for latency calculations */
				ev.timestamp_ns = (long long)iev.time.tv_sec * 1000000000LL + (long long)iev.time.tv_usec * 1000LL;

				if (g.cb)
					g.cb(&ev, g.cb_user);
				else
					ring_push(&g.queue, &ev);
				/* Generate high-level key events if xkb is enabled */
				maybe_emit_key_event(&ev);
			}
		}
	}
	return NULL;
}

int
ni_init(int flags)
{
	if (flags != 0)
		return -1;
	if (g.initialized)
		return 0;
	memset(&g, 0, sizeof(g));
	ring_init(&g.queue);
	keyring_init(&g.key_queue);
	pthread_mutex_init(&g.dev_lock, NULL);
	g.epoll_fd = epoll_create1(EPOLL_CLOEXEC);
	g.mice_fd = -1;
	if (g.epoll_fd < 0)
		return -1;
	/* inotify for hotplug */
	g.inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (g.inotify_fd >= 0) {
		/* Also watch for IN_MOVED_TO because udev may create then rename nodes */
		inotify_add_watch(g.inotify_fd, "/dev/input", IN_CREATE | IN_MOVED_TO | IN_DELETE);
		struct epoll_event iev = {0};
		iev.events = EPOLLIN;
		iev.data.ptr = (void*)EPOLL_DATA_INOTIFY;  /* Use pointer for consistency */
		epoll_ctl(g.epoll_fd, EPOLL_CTL_ADD, g.inotify_fd, &iev);
	}
	scan_devices();
	g.stop = 0;
#ifdef ASYNCINPUT_HAVE_XKBCOMMON
	/* Default xkb names if not set */
	if (!g.xkb_rules[0]) snprintf(g.xkb_rules, sizeof(g.xkb_rules), "evdev");
	if (!g.xkb_model[0]) snprintf(g.xkb_model, sizeof(g.xkb_model), "pc105");
	if (!g.xkb_layout[0]) snprintf(g.xkb_layout, sizeof(g.xkb_layout), "us");
	/* xkb will be created on ni_enable_xkb(1) */

	if (pthread_create(&g.thread, NULL, worker, NULL) != 0)
		return -1;

	if (g.mice_enabled) {
		if (pthread_create(&g.mice_thread, NULL, mice_worker, NULL) != 0) {
			g.mice_enabled = 0; /* non-fatal */
		}
	}

	g.initialized = 1;
	return 0;
#else
	(void)flags; return -1; /* Non-Linux: this TU is inactive */
#endif
}

int
ni_set_device_filter(ni_device_filter filter, void *user_data)
{
	g.filter = filter;
	g.filter_user = user_data;
	if (!g.initialized) return 0;
	/* Rescan: close devices that no longer match; try to open new matching ones */
	/* Close non-matching */
	pthread_mutex_lock(&g.dev_lock);
	for (int i = g.ndevi - 1; i >= 0; i--) {
		int fd = g.devices[i].fd;
		struct ni_device_info info = {0};
		fill_device_info(fd, g.devices[i].path, &info);
		info.id = g.devices[i].id;
		int keep = (g.filter ? g.filter(&info, g.filter_user) : 1);
		if (!keep) {
			epoll_ctl(g.epoll_fd, EPOLL_CTL_DEL, fd, NULL);
			close(fd);
			g.devices[i] = g.devices[g.ndevi-1];
			g.ndevi--;
		}
	}
	pthread_mutex_unlock(&g.dev_lock);
	/* Try open any new devices that match */
	scan_devices();
	return 0;
}

int
ni_device_count(void)
{
	int n;
	pthread_mutex_lock(&g.dev_lock);
	n = g.ndevi;
	pthread_mutex_unlock(&g.dev_lock);
	return n;
}

int
ni_register_callback(ni_callback cb, void *user_data, int flags)
{
	if (!g.initialized || flags != 0)
		return -1;
	g.cb = cb;
	g.cb_user = user_data;
	return 0;
}

int
ni_poll(struct ni_event *evts, int max_events)
{
	if (!g.initialized || !evts || max_events <= 0)
		return -1;
	return ring_pop_many(&g.queue, evts, max_events);
}

int
ni_shutdown(void)
{
	if (!g.initialized)
		return 0;
	g.stop = 1;
	g.mice_enabled = 0;
	if (g.mice_thread) pthread_join(g.mice_thread, NULL);
	pthread_join(g.thread, NULL);
	for (int i = 0; i < g.ndevi; i++)
		close(g.devices[i].fd);
	if (g.inotify_fd >= 0) close(g.inotify_fd);
	close(g.epoll_fd);
	g.initialized = 0;
	return 0;
}

int ni_register_key_callback(ni_key_callback cb, void *user_data, int flags)
{
	(void)flags;
	if (!g.initialized) return -1;
	g.key_cb = cb;
	g.key_cb_user = user_data;
	return 0;
}

int ni_poll_key_events(struct ni_key_event *evts, int max_events)
{
	if (!g.initialized || !evts || max_events <= 0) return -1;
	return keyring_pop_many(&g.key_queue, evts, max_events);
}

static int rebuild_xkb_keymap(void)
{
#ifdef ASYNCINPUT_HAVE_XKBCOMMON
	if (!g.xkb_enabled) return 0;
	if (!g.xkb_ctx) g.xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (!g.xkb_ctx) return -1;
	struct xkb_rule_names rmlvo = {0};
	rmlvo.rules = g.xkb_rules[0] ? g.xkb_rules : NULL;
	rmlvo.model = g.xkb_model[0] ? g.xkb_model : NULL;
	rmlvo.layout = g.xkb_layout[0] ? g.xkb_layout : NULL;
	rmlvo.variant = g.xkb_variant[0] ? g.xkb_variant : NULL;
	rmlvo.options = g.xkb_options[0] ? g.xkb_options : NULL;
	struct xkb_keymap *km = xkb_keymap_new_from_names(g.xkb_ctx, &rmlvo, XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (!km) return -1;
	struct xkb_state *st = xkb_state_new(km);
	if (!st) { xkb_keymap_unref(km); return -1; }
	if (g.xkb_state) xkb_state_unref(g.xkb_state);
	if (g.xkb_keymap) xkb_keymap_unref(g.xkb_keymap);
	g.xkb_keymap = km;
	g.xkb_state = st;
	return 0;
#else
	return -1;
#endif
}

int ni_enable_xkb(int enabled)
{
	g.xkb_enabled = enabled ? 1 : 0;
#ifdef ASYNCINPUT_HAVE_XKBCOMMON
	if (g.xkb_enabled) return rebuild_xkb_keymap();
	/* disabling: free state */
	if (g.xkb_state) { xkb_state_unref(g.xkb_state); g.xkb_state = NULL; }
	if (g.xkb_keymap) { xkb_keymap_unref(g.xkb_keymap); g.xkb_keymap = NULL; }
	if (g.xkb_ctx) { xkb_context_unref(g.xkb_ctx); g.xkb_ctx = NULL; }
	return 0;
#else
	return -1;
#endif
}

int ni_set_xkb_names(const char *rules, const char *model, const char *layout,
                     const char *variant, const char *options)
{
#ifdef ASYNCINPUT_HAVE_XKBCOMMON
	if (rules) { snprintf(g.xkb_rules, sizeof(g.xkb_rules), "%s", rules); }
	if (model) { snprintf(g.xkb_model, sizeof(g.xkb_model), "%s", model); }
	if (layout) { snprintf(g.xkb_layout, sizeof(g.xkb_layout), "%s", layout); }
	if (variant) { snprintf(g.xkb_variant, sizeof(g.xkb_variant), "%s", variant); }
	if (options) { snprintf(g.xkb_options, sizeof(g.xkb_options), "%s", options); }
	if (g.xkb_enabled) return rebuild_xkb_keymap();
	return 0;
#else
	return -1;
#endif
}

int ni_enable_mice(int enabled)
{
	g.mice_enabled = enabled ? 1 : 0;
	if (!g.initialized) return 0;
	if (enabled) {
		if (!g.mice_thread) {
			if (pthread_create(&g.mice_thread, NULL, mice_worker, NULL) != 0) {
				g.mice_enabled = 0;
				return -1;
			}
		}
	}
	return 0;
}
