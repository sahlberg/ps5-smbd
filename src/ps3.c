/* The PS3 side of the server: the network, the on-screen dialog, and what the PS3's C library lacks
   (threads, pread, statvfs, getifaddrs). The whole file system is one share, \\<ip>\root. The full libsmb2
   server, srvsvc and discovery run; the main thread only pumps the system callbacks, because a server loop
   that blocked it would be killed. */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/utime.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <lv2/sysfs.h>
#include <net/net.h>
#include <net/netctl.h>
#include <sys/mutex.h>
#include <sys/systime.h>
#include <sys/thread.h>
#include <sysmodule/sysmodule.h>
#include <sysutil/msg.h>
#include <sysutil/sysutil.h>

#include <net/if.h>

#include "ifaddrs.h"
#include "pthread.h"
#include "smbd.h"
#include "sys/statvfs.h"

#define PS3_LOG_DIR "/dev_hdd0/tmp/ps3-smbd"

static volatile int g_quit;
static int g_net_ok;

static void on_sysutil(u64 status, u64 param, void *usr)
{
	(void)param;
	(void)usr;
	if (status == SYSUTIL_EXIT_GAME)
		g_quit = 1;
}

static void on_dialog(msgButton button, void *usr)
{
	(void)button;
	(void)usr;
	g_quit = 1;
}

/* Load the modules and wait for an address. The callbacks are pumped while waiting: the system wants
   them from this thread. A missing network is not fatal; the dialog says so and the log has the rest. */
int ps3_init(void)
{
	static const sysModuleId modules[] = {SYSMODULE_FS, SYSMODULE_NET, SYSMODULE_NETCTL, SYSMODULE_SYSUTIL,
	                                      SYSMODULE_IO};

	for (size_t i = 0; i < sizeof(modules) / sizeof(modules[0]); i++)
		sysModuleLoad(modules[i]);
	mkdir("/dev_hdd0/tmp", 0777);
	mkdir(PS3_LOG_DIR, 0777);
	netInitialize();
	netCtlInit();
	sysUtilRegisterCallback(SYSUTIL_EVENT_SLOT0, on_sysutil, NULL);
	for (int i = 0; i < 300 && !g_quit; i++) {
		s32 state = 0;

		sysUtilCheckCallback();
		if (netCtlGetState(&state) == 0 && state == NET_CTL_STATE_IPObtained) {
			g_net_ok = 1;
			break;
		}
		sysUsleep(100000);
	}
	return g_quit ? -1 : 0;
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

/* The server runs on its own thread. This thread shows \\ip\root and waits for OK or an XMB quit,
   then asks the loop to return. */
int ps3_serve(struct smb2_server *server, int max_clients, void (*cb)(struct smb2_context *, void *), void *data)
{
	struct serve_arg arg;
	sys_ppu_thread_t thread;
	static char name[] = "smbd";
	int dialog = 0;
	char ip[16];
	char msg[256];

	memset(&arg, 0, sizeof(arg));
	arg.server = server;
	arg.max_clients = max_clients;
	arg.cb = cb;
	arg.data = data;
	arg.result = -1;
	if (sysThreadCreate(&thread, serve_entry, &arg, 1000, 1 << 20, THREAD_JOINABLE, name) != 0)
		return -1;

	ps3_ip(ip, sizeof(ip));
	for (;;) {
		sysUtilCheckCallback();
		if (g_quit)
			break;
		if (!dialog && (server->listener_ready || arg.done)) {
			if (server->listener_ready)
				snprintf(msg, sizeof(msg), "\\\\%s\\root\nuser ps5  password ps5\nEverything under /.\nOK stops the server.",
				         ip);
			else if (!g_net_ok)
				snprintf(msg, sizeof(msg), "No network.\nOK to quit.");
			else
				snprintf(msg, sizeof(msg), "Could not listen on port %d.\nOK to quit.", server->port);
			smbd_log("%s", msg);
			if (msgDialogOpen2((msgType)(MSG_DIALOG_NORMAL | MSG_DIALOG_BTN_TYPE_OK), msg, on_dialog, NULL, NULL) == 0)
				dialog = 1;
			else
				dialog = -1; /* don't retry an open that the system refused */
			if (dialog < 0 && arg.done)
				break;
		}
		sysUsleep(50000);
	}
	server->stop_requested = 1;
	while (!arg.done) {
		sysUtilCheckCallback();
		sysUsleep(50000);
	}
	if (dialog > 0)
		msgDialogAbort();
	sysThreadJoin(thread, NULL);
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
	if (sysThreadCreate(&id, trampoline, tr, 1000, 256 * 1024, 0, name) != 0) {
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
