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
#include <dirent.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdarg.h>
#include <sys/time.h>
#include <signal.h>
#include <string.h>
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
	char path[256];
	if (p && p[0]) {
		/* Several processes inherit these atexit hooks; appending from all
		 * of them interleaves the reports, so give each its own file. */
		snprintf(path, sizeof(path), "%s.%d", p, (int)getpid());
		out = fopen(path, "w");
	} else {
		out = fopen("/tmp/vpucap.log", "w");
	}
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
static int g_pc_started;
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

/* --- PC histogram -------------------------------------------------------
 *
 * The ~24% of Encoder-0 that is neither ioctls, nor an intercepted memcpy,
 * nor rate control, nor a spin (the thread is genuinely asleep 71% of the
 * time) is real on-CPU userspace code. /proc gives no userspace PC on this
 * 3.10 kernel, and there is no perf, so sample it from inside the process:
 * a CLOCK_PROCESS_CPUTIME_ID timer aimed at the encoder thread with
 * SIGEV_THREAD_ID, whose handler records the interrupted PC and attributes
 * it with dladdr.
 *
 * Sampling the encoder thread specifically matters: ITIMER_PROF is
 * process-wide and would be delivered to the main thread, which is parked in
 * poll() while the encoder burns the CPU, giving a useless histogram.
 */

#define PC_SAMPLES 20000

static unsigned int  *g_pc;
static int            g_pc_n;
static unsigned long  g_sig_seen;      /* raw deliveries, before filtering */
static unsigned long  g_sig_dropped;   /* seen inside the shim itself */

/* --- memcpy bandwidth calibration ---------------------------------------
 *
 * Signal-based PC sampling is unavailable here (timer_create succeeds but
 * delivers nothing -- this uclibc's sigevent has no named thread-id member
 * and aliasing _pad[0] does not convince the kernel), and there is no perf
 * or gdb. So test the copy hypothesis by arithmetic instead: measure what a
 * memcpy of this frame size actually costs on this core, and check whether
 * the pipeline's ~13.2 MB/s (720p YUV420 at 10 sensor fps) can account for
 * the encoder's ~24%.
 *
 * Uses a buffer far larger than the 128 KB L2 so the copy is DRAM-bound,
 * which is the regime the real frame copies are in.
 */
static void *memcpy_bench(void *unused)
{
	/* 1.5x the frame size, and 4 MB of working set to defeat L2 */
	const size_t chunk = 1382400;          /* 1280*720*1.5 */
	const int    iters = 20;
	char *src, *dst;
	struct timespec t0, t1;
	double secs;
	unsigned long jiffies, j;
	FILE *tf;
	long long hz = sysconf(_SC_CLK_TCK);
	clockid_t ctid = clock_getcpuclockid ? 0 : 0;
	(void)ctid; (void)unused;

	(void)tf;
	src = malloc(4u << 20);
	dst = malloc(4u << 20);
	if (!src || !dst)
		return NULL;
	memset(src, 0xa5, 4u << 20);
	memset(dst, 0, 4u << 20);

	if (clock_gettime(CLOCK_MONOTONIC, &t0) != 0)
		return NULL;
	{
		struct timespec c0, c1;
		clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &c0);
		for (j = 0; j < (unsigned long)iters; j++)
			memcpy(dst, src, chunk);
		clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &c1);
		jiffies = (unsigned long)((c1.tv_sec - c0.tv_sec) * 1000 +
					  (c1.tv_nsec - c0.tv_nsec) / 1000000);
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);

	secs = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
	if (secs > 0) {
		double bytes = (double)chunk * iters;
		double mbs = bytes / secs / 1048576.0;
		double pct = hz > 0 ? (jiffies / hz) / secs * 100.0 : 0.0;
		out_open();
		if (out) {
			fprintf(out, "\n=== memcpy bandwidth calibration ===\n");
			fprintf(out, "chunk=%zu bytes x%d iters, %.3fs wall\n",
					chunk, iters, secs);
			fprintf(out, "throughput: %.1f MiB/s\n", mbs);
			fprintf(out, "cpu used by copy: %.0f ms = %.1f%% of a core\n",
					jiffies * 10.0, pct);
			fprintf(out, "=> 13.2 MiB/s of copying would cost %.1f%% of a core\n",
					13.2 / mbs * pct);
			fflush(out);
		}
	}
	free(src);
	free(dst);
	return NULL;
}

static void pc_handler(int sig, siginfo_t *si, void *uc)
{
	ucontext_t *u = (ucontext_t *)uc;
	Dl_info info;
	unsigned int pc;

	(void)sig; (void)si;
	g_sig_seen++;

#ifdef REG_PC
	pc = (unsigned int)u->uc_mcontext.gregs[REG_PC];
#else
	return;
#endif

	/* Drop samples taken inside the shim/signal trampoline itself. */
	if (dladdr((void *)pc, &info) && info.dli_fname &&
	    strstr(info.dli_fname, "vpu-capture")) {
		g_sig_dropped++;
		return;
	}

	if (g_pc_n < PC_SAMPLES)
		g_pc[g_pc_n++] = pc;
}

static timer_t g_pc_timer;

static int find_encoder_tid(void)
{
	DIR *d = opendir("/proc/self/task");
	struct dirent *e;
	int tid = -1;
	char path[128], buf[64];
	FILE *f;

	if (!d)
		return -1;
	while ((e = readdir(d)) != NULL) {
		if (e->d_name[0] < '0' || e->d_name[0] > '9')
			continue;
		snprintf(path, sizeof(path), "/proc/self/task/%s/comm", e->d_name);
		f = fopen(path, "r");
		if (!f)
			continue;
		if (fgets(buf, sizeof(buf), f) && strncmp(buf, "Encoder-0", 9) == 0)
			tid = atoi(e->d_name);
		fclose(f);
		if (tid > 0)
			break;
	}
	closedir(d);
	return tid;
}

static void pc_start(void)
{
	struct sigaction sa;
	struct sigevent sev;
	int tid;

	if (g_pc) return;
	g_pc = calloc(PC_SAMPLES, sizeof(unsigned int));
	if (!g_pc) return;

	tid = find_encoder_tid();
	if (tid <= 0) {
		fprintf(out, "[pchist] Encoder-0 thread not found yet\n");
		return;
	}

	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = pc_handler;
	sa.sa_flags = SA_SIGINFO | SA_RESTART;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGPROF, &sa, NULL);

	memset(&sev, 0, sizeof(sev));
	sev.sigev_notify = SIGEV_THREAD_ID;
	sev.sigev_signo = SIGPROF;
#if defined(sigev_notify_thread_id)
	sev.sigev_notify_thread_id = tid;
#else
	/* This uclibc's sigevent union exposes only _pad and _sigev_thread --
	 * no named thread-id member. In glibc's identical layout, SIGEV_THREAD_ID
	 * stores the tid in the first int of that union (aliased as _tid), which
	 * is _pad[0] here. Verified against the preprocessed header. */
	sev._sigev_un._pad[0] = tid;
#endif
	if (timer_create(CLOCK_PROCESS_CPUTIME_ID, &sev, &g_pc_timer) != 0) {
		fprintf(out, "[pchist] timer_create failed\n");
		return;
	}
	/* 1ms of CPU per sample. The encoder is on-CPU ~29% of the time, so this
	 * fills 20k samples in well under a minute of wall time. */
	{
		struct itimerspec its;
		its.it_value.tv_sec = 0;
		its.it_value.tv_nsec = 1000000;
		its.it_interval.tv_sec = 0;
		its.it_interval.tv_nsec = 1000000;
		if (timer_settime(g_pc_timer, 0, &its, NULL) != 0)
			fprintf(out, "[pchist] timer_settime failed\n");
		else
			fprintf(out, "[pchist] sampling Encoder-0 tid=%d at 1ms of CPU\n", tid);
	}
}

static void pc_report(void)
{
#define MAX_UNIQ 512
	static unsigned int uniq[MAX_UNIQ];
	static int          cnt[MAX_UNIQ];
	int i, j, n = 0;
	char tag[256];

	fprintf(out, "\n=== PC sampler: %lu signals delivered, %lu dropped, %d kept ===\n",
			g_sig_seen, g_sig_dropped, g_pc_n);
	if (!g_pc || g_pc_n == 0)
		return;
	for (i = 0; i < g_pc_n; i++) {
		for (j = 0; j < n; j++)
			if (uniq[j] == g_pc[i])
				break;
		if (j == n && n < MAX_UNIQ) { uniq[n] = g_pc[i]; cnt[n] = 0; n++; }
		if (j < n) cnt[j]++;
	}
	fprintf(out, "\n=== PC histogram (%d samples, %d unique) ===\n", g_pc_n, n);
	for (i = 0; i < n; i++) {
		Dl_info info;
		const char *mod = "?";
		const char *sym = "?";
		unsigned long off = 0;
		if (dladdr((void *)uniq[i], &info)) {
			if (info.dli_fname) {
				mod = strrchr(info.dli_fname, '/');
				mod = mod ? mod + 1 : info.dli_fname;
			}
			if (info.dli_sname) sym = info.dli_sname;
			off = (unsigned long)((char *)uniq[i] - (char *)info.dli_fbase);
		}
		snprintf(tag, sizeof(tag), "%s", sym);
		fprintf(out, "  %-14s %-24s +0x%-8lx %6d (%.1f%%)\n",
				mod, tag, off, cnt[i], cnt[i] * 100.0 / g_pc_n);
	}
	fflush(out);
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
	atexit(pc_report);
	{
		pthread_t b;
		pthread_attr_t a;
		pthread_attr_init(&a);
		pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
		pthread_create(&b, &a, memcpy_bench, NULL);
		pthread_attr_destroy(&a);
	}
}

typedef int (*pthread_create_fn)(pthread_t *, const pthread_attr_t *,
				void *(*)(void *), void *);
static pthread_create_fn real_pthread_create;

struct trampoline {
	void *(*fn)(void *);
	void *arg;
};

static void *sigprof_unblock_start(void *p)
{
	struct trampoline *t = p;
	sigset_t set;
	void *(*fn)(void *);
	void *arg;

	sigemptyset(&set);
	sigaddset(&set, SIGPROF);
	pthread_sigmask(SIG_UNBLOCK, &set, NULL);
	fn = t->fn; arg = t->arg;
	return fn(arg);
}

int pthread_create(pthread_t *th, const pthread_attr_t *attr,
		   void *(*fn)(void *), void *arg)
{
	struct trampoline *t;
	if (!real_pthread_create)
		real_pthread_create = dlsym(RTLD_NEXT, "pthread_create");
	t = malloc(sizeof(*t));
	if (!t)
		return real_pthread_create(th, attr, fn, arg);
	t->fn = fn;
	t->arg = arg;
	return real_pthread_create(th, attr, sigprof_unblock_start, t);
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
	if (request == IOCTL_CHANNEL_RUN && !g_pc_started) {
		g_pc_started = 1;
		pc_start();
	}
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
