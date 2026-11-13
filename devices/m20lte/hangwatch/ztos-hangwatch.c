/* Freezes on the M20 leave no record: no lockup detectors, no hardware
 * watchdog. This stays in RAM at real-time priority, watches three ordinary
 * canaries (scheduling, memory, storage), and when one stops it writes what it
 * saw to the kernel log and crashes the kernel on purpose, so that ramoops
 * keeps the backtraces and the phone restarts by itself. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define DIR_ "/var/lib/ztos-hangwatch"
#define RING 64
#define LINE 360
#define PMU 0x11c80000
#define INFORM3 0x80c

static double now(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}
struct beats { _Atomic double run, mem, disk; } *beat;

static void canary_run(void) {
	for (;;) { beat->run = now(); usleep(250000); }
}
/* Not locked: it faults fresh pages, so it enters reclaim like any app. */
static void canary_mem(void) {
	for (;;) {
		size_t n = 256 << 10;
		char *p = mmap(0, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (p != MAP_FAILED) {
			for (size_t i = 0; i < n; i += 4096) p[i] = 1;
			munmap(p, n);
		}
		beat->mem = now();
		sleep(1);
	}
}
static void canary_disk(void) {
	char b[4096] = {1};
	for (;;) {
		int f = open(DIR_ "/canary", O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (f >= 0) {
			if (write(f, b, sizeof b) > 0) fdatasync(f);
			close(f);
		}
		beat->disk = now();
		sleep(5);
	}
}
static void spawn(void (*fn)(void)) {
	if (!fork()) { fn(); _exit(0); }
}

static char ring[RING][LINE];
static atomic_uint head;

static int slurp(const char *path, char *buf, int n) {
	int f = open(path, O_RDONLY | O_CLOEXEC);
	if (f < 0) return buf[0] = 0, 0;
	int r = read(f, buf, n - 1);
	close(f);
	if (r < 0) r = 0;
	return buf[r] = 0, r;
}
static long field(const char *text, const char *key) {
	const char *p = strstr(text, key);
	return p ? atol(p + strlen(key)) : -1;
}
static double avg10(const char *path, const char *kind) {
	char b[256];
	slurp(path, b, sizeof b);
	const char *p = strstr(b, kind);
	if (!p || !(p = strstr(p, "avg10="))) return -1;
	return atof(p + 6);
}
static void sample(double a, double b, double c) {
	char mi[4096], st[8192], t0[32], t1[32], f0[32], f1[32];
	slurp("/proc/meminfo", mi, sizeof mi);
	slurp("/proc/stat", st, sizeof st);
	slurp("/sys/class/thermal/thermal_zone0/temp", t0, sizeof t0);
	slurp("/sys/class/thermal/thermal_zone1/temp", t1, sizeof t1);
	slurp("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", f0, sizeof f0);
	slurp("/sys/devices/system/cpu/cpu6/cpufreq/scaling_cur_freq", f1, sizeof f1);
	time_t w = time(0);
	char ts[16];
	strftime(ts, sizeof ts, "%H:%M:%S", localtime(&w));
	unsigned i = atomic_load(&head) % RING;
	snprintf(ring[i], LINE,
		"%s up=%.0f psi mem %.1f/%.1f cpu %.1f io %.1f/%.1f | avail %ld free %ld anon %ld shmem %ld unevict %ld file %ld swapfree %ld dirty %ld wb %ld | run %ld blk %ld | %ldC %ldC %ld/%ld MHz | age %.1f %.1f %.1f",
		ts, now(),
		avg10("/proc/pressure/memory", "some"), avg10("/proc/pressure/memory", "full"),
		avg10("/proc/pressure/cpu", "some"),
		avg10("/proc/pressure/io", "some"), avg10("/proc/pressure/io", "full"),
		field(mi, "MemAvailable:") / 1024, field(mi, "MemFree:") / 1024,
		field(mi, "AnonPages:") / 1024, field(mi, "Shmem:") / 1024,
		field(mi, "Unevictable:") / 1024, field(mi, "Cached:") / 1024,
		field(mi, "SwapFree:") / 1024, field(mi, "Dirty:") / 1024, field(mi, "Writeback:") / 1024,
		field(st, "procs_running "), field(st, "procs_blocked "),
		atol(t0) / 1000, atol(t1) / 1000, atol(f0) / 1000, atol(f1) / 1000, a, b, c);
	atomic_fetch_add(&head, 1);
}

/* Ordinary priority: if storage hangs this blocks and the watcher carries on. */
static void *writer(void *arg) {
	int f = *(int *)arg;
	unsigned seen = 0;
	for (;;) {
		unsigned h = atomic_load(&head);
		if (h - seen > RING) seen = h - RING;
		for (; seen != h; seen++) {
			dprintf(f, "%s\n", ring[seen % RING]);
		}
		fdatasync(f);
		sleep(2);
	}
	return 0;
}

static int kmsg, sysrq;
static void say(const char *s) {
	char b[LINE + 40];
	int n = snprintf(b, sizeof b, "<3>hangwatch: %s\n", s);
	if (write(kmsg, b, n) < 0) {}
}
static void key(char c) {
	if (write(sysrq, &c, 1) < 0) {}
	usleep(700000);
}
static void trip(const char *why) {
	char b[120];
	snprintf(b, sizeof b, "FROZE: %s", why);
	say(b);
	unsigned h = atomic_load(&head);
	for (unsigned i = h > 20 ? h - 20 : 0; i != h; i++) say(ring[i % RING]);
	key('m');
	key('w');
	key('l');
	key('s');
	sleep(1);
	say("crashing on purpose to keep this log");
	key('c');
}

/* A panic restarts without touching INFORM3, and sboot obeys whatever is left
 * there, usually "recovery": leave it saying "normal". reboot still sets it. */
static void boot_normally(void) {
	int f = open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC);
	if (f < 0) return;
	volatile unsigned *pmu = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, f, PMU);
	if (pmu != MAP_FAILED) {
		pmu[INFORM3 / 4] = 0x12345670;
		munmap((void *)pmu, 4096);
	}
	close(f);
}

int main(void) {
	mkdir(DIR_, 0755);
	boot_normally();
	beat = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	beat->run = beat->mem = beat->disk = now();
	spawn(canary_run);
	spawn(canary_mem);
	spawn(canary_disk);

	rename(DIR_ "/log", DIR_ "/log.1");
	int log = open(DIR_ "/log", O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
	kmsg = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
	sysrq = open("/proc/sysrq-trigger", O_WRONLY | O_CLOEXEC);
	pthread_t t;
	pthread_create(&t, 0, writer, &log);

	const char *dry = getenv("HANGWATCH_DRY");
	mlockall(MCL_CURRENT | MCL_FUTURE);
	struct sched_param rt = { .sched_priority = 60 };
	pthread_setschedparam(pthread_self(), SCHED_FIFO, &rt);

	double last = now();
	for (;;) {
		sleep(1);
		double t2 = now();
		double a = t2 - beat->run, b = t2 - beat->mem, c = t2 - beat->disk;
		if (t2 - last > 5) {	/* we were stopped too (suspend): start over */
			beat->run = beat->mem = beat->disk = t2;
			last = t2;
			continue;
		}
		last = t2;
		sample(a, b, c);
		const char *why = a > 20 ? "ordinary tasks are not being scheduled"
			: b > 25 ? "memory allocation is stuck"
			: c > 60 ? "storage is stuck" : 0;
		if (why) {
			if (dry) { say(why); sleep(30); }
			else trip(why);
		}
	}
}
