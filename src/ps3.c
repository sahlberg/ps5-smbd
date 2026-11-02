/* The PS3 side of the server: the network, the screen, and what the PS3's C library lacks
   (threads, pread, statvfs, getifaddrs). The whole file system is one share, \\<ip>\root. The full libsmb2
   server, srvsvc and discovery run on another thread. This thread flips a frame and pumps system
   callbacks, which is what lets the PS button open the XMB. Quit Game sets g_quit. */

#include <errno.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/utime.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <io/pad.h>
#include <lv2/sysfs.h>
#include <net/net.h>
#include <net/netctl.h>
#include <rsx/rsx.h>
#include <sys/mutex.h>
#include <sys/systime.h>
#include <sys/thread.h>
#include <sysmodule/sysmodule.h>
#include <sysutil/sysutil.h>
#include <sysutil/video.h>

#include <net/if.h>

#include "font8x8_basic.h"
#include "ifaddrs.h"
#include "pthread.h"
#include "smbd.h"
#include "sys/statvfs.h"

#define PS3_LOG_DIR "/dev_hdd0/tmp/ps3-smbd"
#define SCREEN_BG 0x00101828u
#define SCREEN_FG 0x00f0f0f0u
#define SCREEN_HINT 0x00e0d090u
#define SCREEN_BAR 0x00385068u
#define SCREEN_EXIT "Press any button to exit."

static volatile int g_quit;
static volatile int g_xmb;
static int g_net_ok;
static int screen_dirty = 1;

/* The serve thread writes the line. Only the main thread reads it. */
static sys_mutex_t g_note_mu;
static int g_note_mu_ok;
static char g_note[160];
static unsigned g_note_seq;
static unsigned g_note_seen;

static void on_sysutil(u64 status, u64 param, void *usr)
{
	(void)param;
	(void)usr;
	/* Stop flipping while the XMB draws, or the menu never appears on top of this frame. */
	if (status == SYSUTIL_EXIT_GAME)
		g_quit = 1;
	else if (status == SYSUTIL_DRAW_BEGIN)
		g_xmb = 1;
	else if (status == SYSUTIL_DRAW_END) {
		g_xmb = 0;
		screen_dirty = 1;
	}
}

void ps3_note(const char *line)
{
	size_t n;

	if (!line || !g_note_mu_ok)
		return;
	sysMutexLock(g_note_mu, 0);
	n = 0;
	while (line[n] && line[n] != '\n' && n + 1 < sizeof(g_note)) {
		g_note[n] = line[n];
		n++;
	}
	g_note[n] = 0;
	g_note_seq++;
	sysMutexUnlock(g_note_mu);
}

/* Copy the newest line the screen has not shown yet. */
static int note_copy(char *dst, size_t size, unsigned *seq)
{
	if (!g_note_mu_ok)
		return 0;
	sysMutexLock(g_note_mu, 0);
	if (g_note_seq == g_note_seen || !g_note[0]) {
		sysMutexUnlock(g_note_mu);
		return 0;
	}
	snprintf(dst, size, "%s", g_note);
	*seq = g_note_seq;
	sysMutexUnlock(g_note_mu);
	return 1;
}

static void note_mark(unsigned seq)
{
	if (!g_note_mu_ok)
		return;
	sysMutexLock(g_note_mu, 0);
	g_note_seen = seq;
	sysMutexUnlock(g_note_mu);
}

/* Startup lines are already in the log. The screen starts at "waiting", then shows what happens next. */
static void note_mark_now(void)
{
	if (!g_note_mu_ok)
		return;
	sysMutexLock(g_note_mu, 0);
	g_note_seen = g_note_seq;
	sysMutexUnlock(g_note_mu);
}

static gcmContextData *screen_ctx;
static void *screen_host;
static u32 *screen_buf[2];
static u32 screen_off[2];
static u32 screen_depth_off;
static u32 screen_w, screen_h, screen_pitch, screen_scale, screen_fb;
static int screen_ok, screen_first = 1;
static char screen_shown[512];
static char screen_error[96];

static void rsx_barrier(void)
{
	__asm__ __volatile__("eieio" ::: "memory");
}

static void fill_rect(u32 *buf, u32 stride, u32 w, u32 h, u32 x, u32 y, u32 rw, u32 rh, u32 color)
{
	u32 y1, x1;

	if (x >= w || y >= h)
		return;
	if (x + rw > w)
		rw = w - x;
	if (y + rh > h)
		rh = h - y;
	for (y1 = y; y1 < y + rh; y1++) {
		u32 *row = buf + (size_t)y1 * stride + x;

		for (x1 = 0; x1 < rw; x1++)
			row[x1] = color;
	}
}

static void clear_buf(u32 *buf, u32 stride, u32 w, u32 h, u32 color)
{
	u32 y, x;

	for (y = 0; y < h; y++) {
		u32 *row = buf + (size_t)y * stride;

		for (x = 0; x < w; x++)
			row[x] = color;
	}
}

static void draw_text(u32 *buf, u32 stride, const char *text)
{
	u32 scale = screen_scale ? screen_scale : 2;
	u32 advance = 8 * scale;
	u32 line_h = advance + scale;
	u32 x0 = advance;
	u32 x = x0;
	u32 y = advance;
	u32 color = SCREEN_FG;

	for (; text && *text; text++) {
		unsigned char ch = (unsigned char)*text;
		const unsigned char *glyph;
		u32 row, col;

		if (ch == '\n') {
			x = x0;
			y += line_h;
			continue;
		}
		if (y >= screen_h)
			break;
		/* A status line can be longer than the frame. Continue it underneath. */
		if (x > x0 && x + advance > screen_w) {
			x = x0;
			y += line_h;
			if (y >= screen_h)
				break;
		}
		if (x == x0)
			color = strncmp(text, "Press ", 6) == 0 ? SCREEN_HINT : SCREEN_FG;
		if (ch < 32 || ch > 127)
			ch = '?';
		glyph = font8x8_basic[ch];
		for (row = 0; row < 8; row++) {
			unsigned char bits = glyph[row];

			for (col = 0; col < 8; col++) {
				if (bits & (1u << col))
					fill_rect(buf, stride, screen_w, screen_h, x + col * scale, y + row * scale, scale,
					          scale, color);
			}
		}
		x += advance;
	}
}

static void screen_paint(const char *text)
{
	u32 stride = screen_pitch / 4;
	int i;

	for (i = 0; i < 2; i++) {
		clear_buf(screen_buf[i], stride, screen_w, screen_h, SCREEN_BG);
		fill_rect(screen_buf[i], stride, screen_w, screen_h, 0, 0, screen_w, screen_scale * 4, SCREEN_BAR);
		draw_text(screen_buf[i], stride, text);
	}
	rsx_barrier();
}

static void set_surface(u32 index)
{
	gcmSurface sf;

	memset(&sf, 0, sizeof(sf));
	sf.type = GCM_SURFACE_TYPE_LINEAR;
	sf.antiAlias = GCM_SURFACE_CENTER_1;
	sf.colorFormat = GCM_SURFACE_X8R8G8B8;
	sf.colorTarget = GCM_SURFACE_TARGET_0;
	sf.colorLocation[0] = GCM_LOCATION_RSX;
	sf.colorOffset[0] = screen_off[index];
	sf.colorPitch[0] = screen_pitch;
	sf.colorLocation[1] = GCM_LOCATION_RSX;
	sf.colorLocation[2] = GCM_LOCATION_RSX;
	sf.colorLocation[3] = GCM_LOCATION_RSX;
	sf.colorPitch[1] = 64;
	sf.colorPitch[2] = 64;
	sf.colorPitch[3] = 64;
	sf.depthFormat = GCM_SURFACE_ZETA_Z24S8;
	sf.depthLocation = GCM_LOCATION_RSX;
	sf.depthOffset = screen_depth_off;
	sf.depthPitch = screen_pitch;
	sf.width = (u16)screen_w;
	sf.height = (u16)screen_h;
	rsxSetSurface(screen_ctx, &sf);
}

/* Present the back buffer. The first flip only resets the status; later flips wait for the previous one. */
static void screen_flip(void)
{
	int i;

	if (!screen_ok || g_xmb)
		return;
	set_surface(screen_fb);
	if (screen_first) {
		gcmResetFlipStatus();
		screen_first = 0;
	} else {
		for (i = 0; i < 500 && gcmGetFlipStatus() != 0; i++)
			sysUsleep(200);
		gcmResetFlipStatus();
	}
	if (gcmSetFlip(screen_ctx, (u8)screen_fb) != 0)
		return;
	rsxFlushBuffer(screen_ctx);
	gcmSetWaitFlip(screen_ctx);
	screen_fb ^= 1;
}

static void screen_frame(const char *text)
{
	if (!screen_ok || g_xmb || !text)
		return;
	if (screen_dirty || strcmp(screen_shown, text) != 0) {
		snprintf(screen_shown, sizeof(screen_shown), "%s", text);
		screen_paint(screen_shown);
		screen_dirty = 0;
	}
	screen_flip();
}

static void screen_service(const char *text)
{
	if (!screen_ok || g_xmb)
		sysUsleep(50000);
	else
		screen_frame(text);
}

static void screen_shutdown(void)
{
	if (!screen_ctx)
		return;
	gcmSetWaitFlip(screen_ctx);
	rsxFinish(screen_ctx, 1);
	screen_ctx = NULL;
	screen_ok = 0;
}

static void screen_fail(const char *msg)
{
	snprintf(screen_error, sizeof(screen_error), "%s", msg);
}

static int screen_init(void)
{
	static const u32 ids[] = {VIDEO_RESOLUTION_720, VIDEO_RESOLUTION_480, VIDEO_RESOLUTION_576,
	                          VIDEO_RESOLUTION_960x1080};
	videoResolution res;
	videoConfiguration config;
	void *depth;
	u32 res_id = 0;
	u32 pitch;
	int found = 0;
	size_t i;

	if (screen_ok)
		return 0;
	screen_host = memalign(1024 * 1024, 1024 * 1024);
	if (!screen_host) {
		screen_fail("screen: out of memory");
		return -1;
	}
	if (rsxInit(&screen_ctx, 0x10000, 1024 * 1024, screen_host) != 0 || !screen_ctx) {
		screen_ctx = NULL;
		screen_fail("screen: rsxInit failed");
		return -1;
	}
	memset(&res, 0, sizeof(res));
	for (i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
		if (videoGetResolutionAvailability(VIDEO_PRIMARY, ids[i], VIDEO_ASPECT_AUTO, 0) != 1)
			continue;
		if (videoGetResolution((s32)ids[i], &res) == 0 && res.width && res.height) {
			res_id = ids[i];
			found = 1;
			break;
		}
	}
	if (!found) {
		screen_fail("screen: no video mode");
		return -1;
	}
	pitch = (u32)res.width * 4;
	if (pitch & 63)
		pitch = (pitch + 63) & ~63u;
	memset(&config, 0, sizeof(config));
	config.resolution = (u8)res_id;
	config.format = VIDEO_BUFFER_FORMAT_XRGB;
	config.aspect = VIDEO_ASPECT_AUTO;
	config.pitch = pitch;
	if (videoConfigure(VIDEO_PRIMARY, &config, NULL, 0) != 0) {
		screen_fail("screen: videoConfigure failed");
		return -1;
	}
	gcmSetFlipMode(GCM_FLIP_VSYNC);
	for (i = 0; i < 2; i++) {
		screen_buf[i] = rsxMemalign(64, (u32)res.height * pitch);
		if (!screen_buf[i] || rsxAddressToOffset(screen_buf[i], &screen_off[i]) != 0 ||
		    gcmSetDisplayBuffer((u8)i, screen_off[i], pitch, res.width, res.height) != 0) {
			screen_fail("screen: framebuffer failed");
			return -1;
		}
	}
	depth = rsxMemalign(64, (u32)res.height * pitch);
	if (!depth || rsxAddressToOffset(depth, &screen_depth_off) != 0) {
		screen_fail("screen: depth buffer failed");
		return -1;
	}
	screen_w = res.width;
	screen_h = res.height;
	screen_pitch = pitch;
	/* 8px glyphs. Scale 2 is the largest that keeps a line on the frame. The old scale of 4
	   was wider than a 960-wide mode, which the console then stretches across the TV. */
	screen_scale = screen_w >= 40u * 16u ? 2u : 1u;
	screen_ok = 1;
	return 0;
}

static int pad_ready;
static int pad_idle;

static int pad_down(const padData *data)
{
	return data->BTN_LEFT || data->BTN_DOWN || data->BTN_RIGHT || data->BTN_UP || data->BTN_START ||
	       data->BTN_R3 || data->BTN_L3 || data->BTN_SELECT || data->BTN_SQUARE || data->BTN_CROSS ||
	       data->BTN_CIRCLE || data->BTN_TRIANGLE || data->BTN_R1 || data->BTN_L1 || data->BTN_R2 ||
	       data->BTN_L2;
}

/* The button that launched the game is still down on the first reads. Wait until it is released,
   then the next press exits. Analog sticks sit at center and are not a press. */
static void pad_poll(void)
{
	u32 i;
	int down = 0;

	if (!pad_ready || g_quit)
		return;
	for (i = 0; i < MAX_PORT_NUM; i++) {
		padData data;

		memset(&data, 0, sizeof(data));
		if (ioPadGetData(i, &data) != 0 || data.len < 3)
			continue;
		if (pad_down(&data))
			down = 1;
	}
	if (!pad_idle) {
		if (!down)
			pad_idle = 1;
		return;
	}
	if (down)
		g_quit = 1;
}

static void pad_init(void)
{
	if (ioPadInit(MAX_PORT_NUM) == 0)
		pad_ready = 1;
	else
		smbd_log("pad: init failed");
}

/* Load the modules, bring up a frame, and wait for an address. Callbacks stay on this thread. */
int ps3_init(void)
{
	static const sysModuleId modules[] = {SYSMODULE_FS, SYSMODULE_NET, SYSMODULE_NETCTL, SYSMODULE_SYSUTIL,
	                                      SYSMODULE_IO};
	static const char starting[] = "ps3-smbd\n\nStarting.\n\n" SCREEN_EXIT;
	u64 t0 = 0, nsec = 0;

	for (size_t i = 0; i < sizeof(modules) / sizeof(modules[0]); i++)
		sysModuleLoad(modules[i]);
	mkdir("/dev_hdd0/tmp", 0777);
	mkdir(PS3_LOG_DIR, 0777);
	netInitialize();
	netCtlInit();
	{
		sys_mutex_attr_t attr;

		sysMutexAttrInitialize(attr);
		g_note_mu_ok = sysMutexCreate(&g_note_mu, &attr) == 0;
	}
	sysUtilRegisterCallback(SYSUTIL_EVENT_SLOT0, on_sysutil, NULL);
	screen_init();
	pad_init();
	sysGetCurrentTime(&t0, &nsec);
	for (;;) {
		u64 sec = 0;
		s32 state = 0;

		sysUtilCheckCallback();
		pad_poll();
		if (g_quit)
			break;
		screen_service(starting);
		if (netCtlGetState(&state) == 0 && state == NET_CTL_STATE_IPObtained) {
			g_net_ok = 1;
			break;
		}
		if (sysGetCurrentTime(&sec, &nsec) == 0 && sec - t0 >= 30)
			break;
	}
	if (g_quit) {
		screen_shutdown();
		return -1;
	}
	return 0;
}

int ps3_console_name(char *buf, size_t size)
{
	if (!buf || size == 0)
		return -1;
	buf[0] = 0;
	if (sysUtilGetSystemParamString(SYSUTIL_SYSTEMPARAM_ID_NICKNAME, buf, (u32)size) != 0)
		buf[0] = 0;
	buf[size - 1] = 0;
	if (!buf[0])
		snprintf(buf, size, "PS3");
	return 0;
}

static void ps3_ip(char *buf, size_t size)
{
	union net_ctl_info info;

	snprintf(buf, size, "?");
	if (!g_net_ok || netCtlGetInfo(NET_CTL_INFO_IP_ADDRESS, &info) != 0)
		return;
	info.ip_address[sizeof(info.ip_address) - 1] = 0;
	if (info.ip_address[0])
		snprintf(buf, size, "%s", info.ip_address);
}

struct serve_arg {
	struct smb2_server *server;
	int max_clients;
	void (*cb)(struct smb2_context *, void *);
	void *data;
	int result;
	volatile int done;
};

static void serve_entry(void *p)
{
	struct serve_arg *a = p;

	a->result = smb2_serve_port(a->server, a->max_clients, a->cb, a->data);
	a->done = 1;
	sysThreadExit(0);
}

/* One step below the caller. A serve loop that shares the main thread's priority
   never returns to sysUtilCheckCallback, and the PS button stops opening the menu. */
static s32 below_main(void)
{
	sys_ppu_thread_t self;
	s32 prio = 1000;

	if (sysThreadGetId(&self) == 0 && sysThreadGetPriority(self, &prio) == 0 && prio < 3071)
		prio++;
	return prio;
}

static void compose_screen(char *msg, size_t size, struct smb2_server *server, int done)
{
	char ip[16];
	char line[160];
	unsigned seq = 0;
	static char status[160] = "Waiting for a client.";

	ps3_ip(ip, sizeof(ip));
	if (note_copy(line, sizeof(line), &seq)) {
		snprintf(status, sizeof(status), "%s", line);
		note_mark(seq);
	}
	if (!g_net_ok) {
		snprintf(msg, size, "ps3-smbd\n\nNo network.\n\n" SCREEN_EXIT);
		return;
	}
	if (done && !server->listener_ready) {
		snprintf(msg, size, "ps3-smbd\n\nCould not listen on port %d.\n\n" SCREEN_EXIT, server->port);
		return;
	}
	if (!server->listener_ready) {
		snprintf(msg, size, "ps3-smbd\n\nStarting.\n\n" SCREEN_EXIT);
		return;
	}
	if (done) {
		snprintf(msg, size, "ps3-smbd\n\n\\\\%s\\root\n\nThe server stopped.\n\n" SCREEN_EXIT, ip);
		return;
	}
	snprintf(msg, size, "ps3-smbd\n\n\\\\%s\\root\nuser ps3    password ps3\n\n" SCREEN_EXIT "\n\n%s", ip,
	         status);
}

/* The server runs on its own thread. This thread keeps a frame on screen and waits for a button. */
int ps3_serve(struct smb2_server *server, int max_clients, void (*cb)(struct smb2_context *, void *), void *data)
{
	struct serve_arg arg;
	sys_ppu_thread_t thread;
	static char name[] = "smbd";
	char msg[512];
	int fail_logged = 0;

	memset(&arg, 0, sizeof(arg));
	arg.server = server;
	arg.max_clients = max_clients;
	arg.cb = cb;
	arg.data = data;
	arg.result = -1;
	if (screen_error[0])
		smbd_log("%s", screen_error);
	else if (screen_ok)
		smbd_log("screen %ux%u scale %u", screen_w, screen_h, screen_scale);
	else
		smbd_log("screen: display init failed");
	if (!pad_ready)
		smbd_log("pad: init failed");
	note_mark_now();
	if (sysThreadCreate(&thread, serve_entry, &arg, below_main(), 1 << 20, THREAD_JOINABLE, name) != 0) {
		screen_shutdown();
		return -1;
	}
	for (;;) {
		sysUtilCheckCallback();
		pad_poll();
		if (g_quit)
			break;
		if (!fail_logged && !g_net_ok) {
			smbd_log("no network");
			fail_logged = 1;
		} else if (!fail_logged && arg.done && !server->listener_ready) {
			smbd_log("could not listen on port %d", server->port);
			fail_logged = 1;
		}
		compose_screen(msg, sizeof(msg), server, arg.done);
		screen_service(msg);
	}
	server->stop_requested = 1;
	while (!arg.done) {
		sysUtilCheckCallback();
		sysUsleep(50000);
	}
	sysThreadJoin(thread, NULL);
	screen_shutdown();
	return arg.result;
}

/* ---- what libc declares and does not define ------------------------------------------------------------ */

ssize_t pread(int fd, void *buf, size_t count, off_t offset)
{
	off_t cur = lseek(fd, 0, SEEK_CUR);
	ssize_t n;

	if (lseek(fd, offset, SEEK_SET) < 0)
		return -1;
	n = read(fd, buf, count);
	if (cur >= 0)
		lseek(fd, cur, SEEK_SET);
	return n;
}

ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset)
{
	off_t cur = lseek(fd, 0, SEEK_CUR);
	ssize_t n;

	if (lseek(fd, offset, SEEK_SET) < 0)
		return -1;
	n = write(fd, buf, count);
	if (cur >= 0)
		lseek(fd, cur, SEEK_SET);
	return n;
}

int utimes(const char *path, const struct timeval tv[2])
{
	struct utimbuf times;

	if (!tv)
		return utime(path, NULL);
	times.actime = tv[0].tv_sec;
	times.modtime = tv[1].tv_sec;
	return utime(path, &times);
}

int lstat(const char *path, struct stat *st)
{
	return stat(path, st);
}

/* libsmb2 calls this when a server has no hostname yet. main() sets one before it serves. */
int gethostname(char *name, size_t len)
{
	if (!name || len == 0)
		return -1;
	snprintf(name, len, "PS3");
	return 0;
}

/* The mode on a game-os path is not a Unix mode. open and mkdir already pass 0777. */
int fchmod(int fd, mode_t mode)
{
	(void)fd;
	(void)mode;
	return 0;
}

/* sysFsGetFreeSize reports the free space and not the size of the volume. The total is reported as the
   free space, so a copy is not refused for want of room and the total is not smaller than the free. */
int statvfs(const char *path, struct statvfs *buf)
{
	u32 block = 0;
	u64 blocks = 0;
	uint64_t bytes;

	memset(buf, 0, sizeof(*buf));
	if (sysFsGetFreeSize(path, &block, &blocks) != 0 || block == 0) {
		/* "/" is not a volume. A copy into the share asks the share root, which is the internal disk. */
		block = 0;
		if (path && path[0] == '/' && path[1] == '\0')
			sysFsGetFreeSize("/dev_hdd0", &block, &blocks);
	}
	if (block == 0)
		return -1;
	bytes = blocks * (uint64_t)block;
	buf->f_bsize = 512;
	buf->f_frsize = 512;
	buf->f_blocks = (unsigned long)(bytes / 512);
	buf->f_bfree = buf->f_blocks;
	buf->f_bavail = buf->f_blocks;
	buf->f_namemax = 255;
	return 0;
}

int getifaddrs(struct ifaddrs **ifap)
{
	union net_ctl_info ip, mask;
	struct ifaddrs *ifa;
	struct sockaddr_in *addr, *netmask;

	*ifap = NULL;
	if (!g_net_ok || netCtlGetInfo(NET_CTL_INFO_IP_ADDRESS, &ip) != 0 ||
	    netCtlGetInfo(NET_CTL_INFO_NETMASK, &mask) != 0)
		return -1;
	ip.ip_address[sizeof(ip.ip_address) - 1] = 0;
	mask.netmask[sizeof(mask.netmask) - 1] = 0;
	ifa = calloc(1, sizeof(*ifa) + 2 * sizeof(*addr) + 8);
	if (!ifa)
		return -1;
	addr = (struct sockaddr_in *)(ifa + 1);
	netmask = addr + 1;
	ifa->ifa_name = (char *)(netmask + 1);
	memcpy(ifa->ifa_name, "eth0", 5);
	ifa->ifa_flags = IFF_UP;
	addr->sin_len = sizeof(*addr);
	addr->sin_family = AF_INET;
	netmask->sin_len = sizeof(*netmask);
	netmask->sin_family = AF_INET;
	if (!inet_aton(ip.ip_address, &addr->sin_addr) || !inet_aton(mask.netmask, &netmask->sin_addr)) {
		free(ifa);
		return -1;
	}
	ifa->ifa_addr = (struct sockaddr *)addr;
	ifa->ifa_netmask = (struct sockaddr *)netmask;
	*ifap = ifa;
	return 0;
}

void freeifaddrs(struct ifaddrs *ifa)
{
	free(ifa);
}

/* ---- pthread, for the discovery threads ---------------------------------------------------------------- */

struct tramp {
	void *(*start)(void *);
	void *arg;
};

static void trampoline(void *p)
{
	struct tramp *tr = p;
	void *(*start)(void *) = tr->start;
	void *arg = tr->arg;

	free(tr);
	if (start)
		start(arg);
	sysThreadExit(0);
}

int pthread_create(pthread_t *thread, const void *attr, void *(*start)(void *), void *arg)
{
	struct tramp *tr;
	sys_ppu_thread_t id;
	static char name[] = "smbd";

	(void)attr;
	tr = malloc(sizeof(*tr));
	if (!tr)
		return -1;
	tr->start = start;
	tr->arg = arg;
	if (sysThreadCreate(&id, trampoline, tr, below_main(), 256 * 1024, 0, name) != 0) {
		free(tr);
		return -1;
	}
	if (thread)
		*thread = id;
	return 0;
}

/* Created detached: names.c and wsd.c detach immediately, and nothing joins them. */
int pthread_detach(pthread_t thread)
{
	(void)thread;
	return 0;
}

static void mutex_ready(pthread_mutex_t *mutex)
{
	if (mutex->state == 2)
		return;
	if (__sync_bool_compare_and_swap(&mutex->state, 0, 1)) {
		sys_mutex_attr_t attr;
		sys_mutex_t id;

		sysMutexAttrInitialize(attr);
		if (sysMutexCreate(&id, &attr) == 0)
			mutex->mutex = id;
		__sync_synchronize();
		mutex->state = 2;
	} else {
		while (mutex->state != 2)
			sysUsleep(1000);
	}
}

int pthread_mutex_lock(pthread_mutex_t *mutex)
{
	mutex_ready(mutex);
	return sysMutexLock(mutex->mutex, 0) == 0 ? 0 : -1;
}

int pthread_mutex_unlock(pthread_mutex_t *mutex)
{
	return sysMutexUnlock(mutex->mutex) == 0 ? 0 : -1;
}
