/* vpu-capture.c — LD_PRELOAD shim that records what libimp hands the VPU driver.
 *
 * t20-rtspd talks to the Ingenic H.264 encoder through libimp.so, which in
 * turn drives the VPU through ioctls on /dev/soc_vpu. The per-frame cost we
 * measured (Encoder-0, ~24%, almost all of it stime-free userspace that
 * scales with pixel count) is libimp's, and the interesting question is
 * exactly what it asks the hardware to do.
 *
 * This shim intercepts ioctl(2) for fds opened on /dev/soc_vpu and records:
 *   - the request number
 *   - struct channel_node, field by field
 *   - a hexdump of the buffer at ncu_addr, which is the proprietary "vconfig"
 *     blob libimp builds (SPS/PPS/QP tables/GOP/RC parameters) and the main
 *     reason a from-scratch encoder is not simply writable
 *
 * Usage:
 *   LD_PRELOAD=/path/vpu-capture.so ./t20-rtspd
 *   output lands in $VPU_CAPTURE_OUT (default /tmp/vpucap.log)
 *
 * The struct layout is the open thingino-linux one for non-T23 parts. If the
 * guess is right the captured fields are obviously sane (small workphase,
 * codecdir == 0 for H.264 encode, plausible channel_id); if the camera's
 * 3.10.14 driver disagrees the values will be visibly garbage, which is the
 * point — it self-validates.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdarg.h>
#include <sys/time.h>
#include <sys/ioctl.h>

#define MAX_TRACKED_FDS 32
static int tracked[MAX_TRACKED_FDS];
static int n_tracked;
static FILE *out;

#define MAX_POLL_ADDRS 4096
static unsigned int poll_paddr[MAX_POLL_ADDRS];
static unsigned int poll_count[MAX_POLL_ADDRS];
static int n_poll;

static void poll_note(unsigned int paddr)
{
	int i;
	for (i = 0; i < n_poll; i++) {
		if (poll_paddr[i] == paddr) {
			poll_count[i]++;
			return;
		}
	}
	if (n_poll < MAX_POLL_ADDRS) {
		poll_paddr[n_poll] = paddr;
		poll_count[n_poll] = 1;
		n_poll++;
	}
}


/* Mirrors drivers/video/soc_vpu/channel_vpu.h (non-T23 layout). */
struct channel_node {
	unsigned int clist;       /* list_head* */
	unsigned int vlist;       /* list_head* */
	unsigned int mdelay;
	unsigned int channel_id;
	int          vpu_id;
	unsigned int codecdir;
	int          workphase;   /* enum workphase */
	unsigned int status;
	unsigned int output_len;
	unsigned int dma_addr;
	int          thread_id;
	unsigned int cmpx;
	unsigned int n_flag;
	unsigned int ncu_addr;    /* void* */
};

#define SOC_VPU_MAGIC 'c'
#define IOCTL_CHANNEL_REQ            _IOWR(SOC_VPU_MAGIC, 0,  struct channel_node)
#define IOCTL_CHANNEL_REL            _IOWR(SOC_VPU_MAGIC, 1,  struct channel_node)
#define IOCTL_CHANNEL_RUN            _IOWR(SOC_VPU_MAGIC, 2,  struct channel_node)
#define IOCTL_CHANNEL_START          _IOWR(SOC_VPU_MAGIC, 3,  struct channel_node)
#define IOCTL_CHANNEL_WAIT_COMPLETE  _IOWR(SOC_VPU_MAGIC, 4,  struct channel_node)
#define IOCTL_CHANNEL_FLUSH_CACHE    _IOWR(SOC_VPU_MAGIC, 5,  struct channel_node)
#define IOCTL_CHANNEL_BUF_INIT       _IOWR(SOC_VPU_MAGIC, 6,  struct channel_node)
#define IOCTL_CHANNEL_WOR_VPU_REG    _IOWR(SOC_VPU_MAGIC, 7,  struct channel_node)
#define IOCTL_CHANNEL_VPU_SUSPEND    _IOWR(SOC_VPU_MAGIC, 8,  struct channel_node)
#define IOCTL_CHANNEL_VPU_RESUME     _IOWR(SOC_VPU_MAGIC, 9,  struct channel_node)
#define IOCTL_CHANNEL_PRIVATE_TLB    _IOWR(SOC_VPU_MAGIC, 10, struct channel_node)
#define IOCTL_CHANNEL_WAIT_BSFULL    _IOWR(SOC_VPU_MAGIC, 11, struct channel_node)
#define IOCTL_CHANNEL_SET_BSFULL     _IOWR(SOC_VPU_MAGIC, 12, struct channel_node)

static void out_open(void)
{
	if (out)
		return;
	const char *p = getenv("VPU_CAPTURE_OUT");
	out = fopen(p ? p : "/tmp/vpucap.log", "a");
	if (out)
		setvbuf(out, NULL, _IONBF, 0);
}

static int is_tracked(int fd)
{
	int i;
	for (i = 0; i < n_tracked; i++)
		if (tracked[i] == fd)
			return 1;
	return 0;
}

static const char *req_name(unsigned long r)
{
	switch (r) {
	case IOCTL_CHANNEL_REQ:           return "REQ";
	case IOCTL_CHANNEL_REL:           return "REL";
	case IOCTL_CHANNEL_RUN:           return "RUN";
	case IOCTL_CHANNEL_START:         return "START";
	case IOCTL_CHANNEL_WAIT_COMPLETE: return "WAIT_COMPLETE";
	case IOCTL_CHANNEL_FLUSH_CACHE:   return "FLUSH_CACHE";
	case IOCTL_CHANNEL_BUF_INIT:      return "BUF_INIT";
	case IOCTL_CHANNEL_WOR_VPU_REG:   return "WOR_VPU_REG";
	case IOCTL_CHANNEL_VPU_SUSPEND:   return "VPU_SUSPEND";
	case IOCTL_CHANNEL_VPU_RESUME:    return "VPU_RESUME";
	case IOCTL_CHANNEL_PRIVATE_TLB:   return "PRIVATE_TLB";
	case IOCTL_CHANNEL_WAIT_BSFULL:   return "WAIT_BSFULL";
	case IOCTL_CHANNEL_SET_BSFULL:    return "SET_BSFULL";
	default:                          return "OTHER";
	}
}

/* Only dump the vconfig once per request kind, otherwise we get thousands of
 * identical multi-KB dumps per second. */
static int traced_runs;
#define TRACE_FRAMES 3
static struct timeval g_t0;

static void dump_vconfig(const char *tag, unsigned int addr, int budget)
{
	unsigned char buf[512];
	int fd, i, n;

	if (!addr)
		return;
	fd = open("/proc/self/mem", O_RDONLY);
	if (fd < 0) {
		fprintf(out, "  vconfig %s: open /proc/self/mem failed\n", tag);
		return;
	}
	if (lseek(fd, (off_t)addr, SEEK_SET) != (off_t)addr) {
		fprintf(out, "  vconfig %s: seek to 0x%x failed\n", tag, addr);
		close(fd);
		return;
	}
	n = read(fd, buf, sizeof(buf));
	close(fd);
	if (n <= 0) {
		fprintf(out, "  vconfig %s: read at 0x%x returned %d\n", tag, addr, n);
		return;
	}
	fprintf(out, "  vconfig %s @0x%x (%d bytes read, showing %d):\n",
			tag, addr, n, n < budget ? n : budget);
	for (i = 0; i < n && i < budget; i += 16) {
		int j;
		fprintf(out, "   %04x ", i);
		for (j = i; j < i + 16 && j < n; j++)
			fprintf(out, "%02x ", buf[j]);
		fprintf(out, "\n");
	}
}

/* --- memcpy accounting ---
 *
 * The ioctl trace shows the VPU interface is trivial: a strict RUN +
 * 4xWOR_VPU_REG per encoded frame, ~12 ioctls/second, which is nowhere near
 * 24% CPU. So the cost is in libimp's own per-frame userspace work, and the
 * shape of it (cpu ~ 9% fixed + 1.64e-5 * pixels) says "copy", not "compute".
 *
 * So count the copies. Only sizes >= FRAME_SIZED are tallied separately, so
 * the per-byte counters stay cheap; everything is forwarded to the real
 * implementation untouched.
 */
#define FRAME_SIZED 65536

static unsigned long long g_memcpy_calls, g_memcpy_bytes;
static unsigned long long g_big_calls, g_big_bytes;
static unsigned long long g_big_max;

static void big_tally(size_t n)
{
	if (n >= FRAME_SIZED) {
		g_big_calls++;
		g_big_bytes += n;
		if ((unsigned long long)n > g_big_max)
			g_big_max = n;
	}
}

void *memcpy(void *dst, const void *src, size_t n)
{
	static void *(*real)(void *, const void *, size_t);
	if (!real)
		real = dlsym(RTLD_NEXT, "memcpy");
	g_memcpy_calls++;
	g_memcpy_bytes += n;
	big_tally(n);
	return real(dst, src, n);
}

void *memmove(void *dst, const void *src, size_t n)
{
	static void *(*real)(void *, const void *, size_t);
	if (!real)
		real = dlsym(RTLD_NEXT, "memmove");
	g_memcpy_calls++;
	g_memcpy_bytes += n;
	big_tally(n);
	return real(dst, src, n);
}

static void memcpy_report(void)
{
	out_open();
	if (!out)
		return;
	fprintf(out, "\n=== memcpy summary ===\n");
	fprintf(out, "all calls   : %llu  total %llu bytes (%.2f MiB)\n",
			g_memcpy_calls, g_memcpy_bytes, g_memcpy_bytes / 1048576.0);
	fprintf(out, ">=%d bytes : %llu calls, %llu bytes (%.2f MiB), max single %llu\n",
			FRAME_SIZED, g_big_calls, g_big_bytes,
			g_big_bytes / 1048576.0, g_big_max);
	if (g_big_calls)
		fprintf(out, "mean big copy: %llu bytes\n", g_big_bytes / g_big_calls);

	/* Most-polled register addresses. A single address dominating means
	 * libimp is spinning on it -- i.e. the 24% is idle waiting on the
	 * hardware, not useful work. */
	{
		int i, j;
		fprintf(out, "\n=== top polled VPU registers (n=%d) ===\n", n_poll);
		for (i = 0; i < n_poll; i++) {
			for (j = i + 1; j < n_poll; j++) {
				if (poll_count[j] > poll_count[i]) {
					unsigned int tp = poll_paddr[i];
					unsigned int tc = poll_count[i];
					poll_paddr[i] = poll_paddr[j];
					poll_count[i] = poll_count[j];
					poll_paddr[j] = tp;
					poll_count[j] = tc;
				}
			}
		}
		for (i = 0; i < n_poll && i < 8; i++)
			fprintf(out, "  paddr=0x%08x  read %u times\n",
					poll_paddr[i], poll_count[i]);
	}
	fflush(out);
}

__attribute__((constructor)) static void vpu_cap_init(void)
{
	out_open();
	gettimeofday(&g_t0, NULL);
	atexit(memcpy_report);
}

int open(const char *pathname, int flags, ...)
{
	static int (*real_open)(const char *, int, ...);
	mode_t mode = 0;
	va_list ap;
	int fd;

	if (!real_open)
		real_open = dlsym(RTLD_NEXT, "open");

	va_start(ap, flags);
	if (flags & O_CREAT)
		mode = (mode_t)va_arg(ap, int);
	va_end(ap);

	fd = real_open(pathname, flags, mode);
	if (fd >= 0 && pathname && strstr(pathname, "soc_vpu")) {
		out_open();
		if (n_tracked < MAX_TRACKED_FDS)
			tracked[n_tracked++] = fd;
		if (out)
			fprintf(out, "OPEN %s -> fd %d\n", pathname, fd);
	}
	return fd;
}

/* WOR_VPU_REG does not carry a channel_node — it carries reg_info. Decoding
 * it as one produced obvious garbage, which is how we noticed. Poll-pattern
 * detection lives here: if libimp is spinning on a status register we will
 * see the same paddr read over and over with the value changing underneath. */
struct reg_info {
	unsigned int paddr;
	unsigned int value;
	int          dir;   /* READ_DIR = 0, WRITE_DIR = 1 */
};

int ioctl(int fd, unsigned long request, ...)
{
	static int (*real_ioctl)(int, unsigned long, ...);
	va_list ap;
	void *arg;
	int r;
	struct channel_node *cn;

	if (!real_ioctl)
		real_ioctl = dlsym(RTLD_NEXT, "ioctl");

	va_start(ap, request);
	arg = va_arg(ap, void *);
	va_end(ap);

	r = real_ioctl(fd, request, arg);

	if (!is_tracked(fd) || !arg)
		return r;
	out_open();
	if (!out)
		return r;

	/* Register access has its own struct; decoding it as channel_node
	 * produced garbage, which is how this was found.
	 *
	 * Trace the first TRACE_FRAMES encodes in full: the register recipe
	 * libimp programs an encode with is the input a replacement encoder
	 * needs, and it is only visible here. */
	if (request == IOCTL_CHANNEL_WOR_VPU_REG) {
		struct reg_info *ri = (struct reg_info *)arg;
		if (ri->dir == 0) {
			poll_note(ri->paddr);
			if (traced_runs < TRACE_FRAMES)
				fprintf(out, "  REG r 0x%08x -> 0x%08x\n",
						ri->paddr, ri->value);
		} else {
			if (traced_runs < TRACE_FRAMES)
				fprintf(out, "  REG w 0x%08x <- 0x%08x\n",
						ri->paddr, ri->value);
		}
		return r;
	}

	cn = (struct channel_node *)arg;
	if (request == IOCTL_CHANNEL_RUN && traced_runs < TRACE_FRAMES) {
		struct timeval tv;
		gettimeofday(&tv, NULL);
		long ms = (tv.tv_sec - g_t0.tv_sec) * 1000 +
			  (tv.tv_usec - g_t0.tv_usec) / 1000;
		fprintf(out, "FRAME %d t=%ldms\n", traced_runs, ms);
		traced_runs++;
	}
	fprintf(out, "IOCTL %s (0x%lx) fd=%d ret=%d\n",
			req_name(request), request, fd, r);
	fprintf(out, "  clist=0x%08x vlist=0x%08x mdelay=%u channel_id=%u vpu_id=%d\n",
			cn->clist, cn->vlist, cn->mdelay, cn->channel_id, cn->vpu_id);
	fprintf(out, "  codecdir=0x%x workphase=%d status=0x%x output_len=%u\n",
			cn->codecdir, cn->workphase, cn->status, cn->output_len);
	fprintf(out, "  dma_addr=0x%08x thread_id=%d cmpx=%u n_flag=%u ncu_addr=0x%08x\n",
			cn->dma_addr, cn->thread_id, cn->cmpx, cn->n_flag, cn->ncu_addr);

	return r;
}
