// Agent: Agent Mode, Date: 2025-08-16, Observation: Initial linux MVP implementation, worker thread reads /dev/input/event* with epoll, ring buffer for polling
#include "asyncinput.h"

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
	struct epoll_event ev = {0};
	ev.events = EPOLLIN;
	ev.data.u32 = (uint32_t)devid;
	epoll_ctl(g.epoll_fd, EPOLL_CTL_ADD, fd, &ev);
	pthread_mutex_lock(&g.dev_lock);
	g.devices[g.ndevi].fd = fd;
	g.devices[g.ndevi].id = devid;
	strncpy(g.devices[g.ndevi].path, path ? path : "", sizeof(g.devices[g.ndevi].path)-1);
	g.ndevi++;
	pthread_mutex_unlock(&g.dev_lock);
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
			if ((ie->mask & IN_CREATE) && ie->len > 0) {
				if (strncmp(ie->name, "event", 5) == 0) {
					char path[128];
					snprintf(path, sizeof(path), "/dev/input/%s", ie->name);
					int devid = -1; int fd = open_device_filtered(path, &devid);
					if (fd >= 0) add_device_fd(fd, devid, path);
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

static void *
worker(void *arg)
{
	(void)arg;
	struct epoll_event evs[MAX_EPOLL_EVENTS];
	struct input_event iev;

	while (!g.stop) {
		int n = epoll_wait(g.epoll_fd, evs, MAX_EPOLL_EVENTS, 50);
		if (n <= 0)
			continue;
		for (int i = 0; i < n; i++) {
			uint32_t tag = evs[i].data.u32;
			if (tag == EPOLL_DATA_INOTIFY) {
				handle_inotify_event();
				continue;
			}
			/* find fd by id: epoll stores id in data.u32, map to fd */
			int devid = (int)tag;
			int fd = -1;
			for (int j = 0; j < g.ndevi; j++) {
				if (g.devices[j].id == devid) {
					fd = g.devices[j].fd;
					break;
				}
			}
			if (fd < 0)
				continue;
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
	pthread_mutex_init(&g.dev_lock, NULL);
	g.epoll_fd = epoll_create1(EPOLL_CLOEXEC);
	if (g.epoll_fd < 0)
		return -1;
	/* inotify for hotplug */
	g.inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (g.inotify_fd >= 0) {
		inotify_add_watch(g.inotify_fd, "/dev/input", IN_CREATE | IN_DELETE);
		struct epoll_event iev = {0};
		iev.events = EPOLLIN;
		iev.data.u32 = EPOLL_DATA_INOTIFY;
		epoll_ctl(g.epoll_fd, EPOLL_CTL_ADD, g.inotify_fd, &iev);
	}
	scan_devices();
	g.stop = 0;
	if (pthread_create(&g.thread, NULL, worker, NULL) != 0)
		return -1;
	g.initialized = 1;
	return 0;
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
	pthread_join(g.thread, NULL);
	for (int i = 0; i < g.ndevi; i++)
		close(g.devices[i].fd);
	if (g.inotify_fd >= 0) close(g.inotify_fd);
	close(g.epoll_fd);
	g.initialized = 0;
	return 0;
}

