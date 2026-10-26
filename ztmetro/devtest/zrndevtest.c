// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ZrnDevTest: what the phone is doing, for whoever is bringing it up. System
 * state, the sensor readings this kernel exposes as they change, and the
 * systemd journal, the kernel log and the shell's own log as they are written.
 * Built on ZrnMetroCore; nothing here draws a window or handles a finger.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "ztos-hw.h"
#include "zrnmetrocore.h"

enum page { PAGE_HUB, PAGE_SYSTEM, PAGE_SENSORS, PAGE_JOURNAL, PAGE_KERNEL, PAGE_SHELL, PAGE_HARDWARE,
	PAGE_LOGS /* the hub's second tab */, PAGE_ANIM, PAGE_PREVIEW, PAGE_TASKS };
enum action { ACT_OPEN = 1, ACT_PLAY, ACT_TASK, ACT_TASK_KIND, ACT_TASK_END, ACT_TASK_CANCEL };

#define MAX_LINES	70	/* of a log, newest first */
#define LINE_LEN	240

static struct {
	char lines[MAX_LINES][LINE_LEN];
	int n, head;		/* a ring: head is where the next line goes */
	char partial[LINE_LEN];
	size_t partial_len;
	pid_t pid;
	int fd;
} stream = { .fd = -1 };

static void stream_stop(void) {
	if (stream.fd >= 0) {
		zmc_watch_fd(stream.fd, NULL, NULL);
		close(stream.fd);
		stream.fd = -1;
	}
	if (stream.pid > 0) {
		kill(stream.pid, SIGTERM);
		waitpid(stream.pid, NULL, 0);
		stream.pid = 0;
	}
	zmc_busy(false);
}

static void stream_line(const char *line) {
	snprintf(stream.lines[stream.head], LINE_LEN, "%s", line);
	stream.head = (stream.head + 1) % MAX_LINES;
	if (stream.n < MAX_LINES) {
		stream.n++;
	}
}

static void stream_ready(int fd, void *data) {
	char buf[4096];
	ssize_t got = read(fd, buf, sizeof(buf));
	if (got <= 0) {
		stream_stop();
		stream_line("(the log ended)");
		zmc_redraw();
		return;
	}
	for (ssize_t i = 0; i < got; i++) {
		if (buf[i] == '\n') {
			stream.partial[stream.partial_len] = 0;
			stream_line(stream.partial);
			stream.partial_len = 0;
		} else if (stream.partial_len < LINE_LEN - 1) {
			stream.partial[stream.partial_len++] = buf[i];
		}
	}
	zmc_busy(false);	/* the first lines are here */
	zmc_redraw();
}

/* Run a command and collect what it prints, line by line, for as long as
 * its page is open. */
static void stream_start(const char *command) {
	int fds[2];
	stream_stop();
	stream.n = stream.head = 0;
	stream.partial_len = 0;
	if (pipe2(fds, O_CLOEXEC) < 0) {
		return;
	}
	stream.pid = fork();
	if (stream.pid == 0) {
		dup2(fds[1], STDOUT_FILENO);
		dup2(fds[1], STDERR_FILENO);
		execl("/bin/sh", "sh", "-c", command, (char *)NULL);
		_exit(127);
	}
	close(fds[1]);
	stream.fd = fds[0];
	fcntl(stream.fd, F_SETFL, O_NONBLOCK);
	zmc_watch_fd(stream.fd, stream_ready, NULL);
	zmc_busy(true);
}

static double draw_stream(cairo_t *cr, const char *title) {
	double y = zmc_header(cr, title);
	if (stream.n == 0) {
		cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
		zmc_text(cr, "waiting for the first lines", FONT_REGULAR, 13.5, ZMC_MARGIN, y, 0);
		return y + 40;
	}
	for (int i = 0; i < stream.n; i++) {
		const char *line = stream.lines[(stream.head - 1 - i + 2 * MAX_LINES) % MAX_LINES];
		/* the newest is at the top, and brighter than what is behind it */
		cairo_set_source_rgb(cr, i == 0 ? 1.0 : 0.72, i == 0 ? 1.0 : 0.72, i == 0 ? 1.0 : 0.72);
		y += zmc_text(cr, line, FONT_MONO, 9.5, ZMC_MARGIN, y, zmc.width - 2 * ZMC_MARGIN) + 5;
	}
	return y + 20;
}

static int hub_tab = PAGE_HUB;	/* the hub tab last shown */

static double draw_hub(cairo_t *cr) {
	double y = zmc_pivot_next(cr, "devtest", "logs", ACT_OPEN, PAGE_LOGS);
	y = zmc_entry(cr, y, "system", "kernel, memory, storage, load", ACT_OPEN, PAGE_SYSTEM);
	y = zmc_entry(cr, y, "sensors", "battery, charger, heat, clocks: live", ACT_OPEN, PAGE_SENSORS);
	y = zmc_entry(cr, y, "hardware", "what this kernel drives, and what it does not", ACT_OPEN, PAGE_HARDWARE);
	y = zmc_entry(cr, y, "task manager", "everything running, by kind, and ending it", ACT_OPEN, PAGE_TASKS);
	y = zmc_entry(cr, y, "animations", "Windows Phone 8.1's transitions, from the Lumia 530's firmware", ACT_OPEN, PAGE_ANIM);
	return y + 20;
}

/* The hub's second tab: the logs, as they are written. */
static double draw_logs(cairo_t *cr) {
	double y = zmc_pivot_next(cr, "logs", "devtest", ACT_OPEN, PAGE_HUB);
	y = zmc_entry(cr, y, "journal", "systemd's log as it is written", ACT_OPEN, PAGE_JOURNAL);
	y = zmc_entry(cr, y, "kernel log", "the kernel's messages as they come", ACT_OPEN, PAGE_KERNEL);
	y = zmc_entry(cr, y, "shell log", "ztMETRO's own log", ACT_OPEN, PAGE_SHELL);
	return y + 20;
}

/* Memory as one bar: what apps hold, the file cache the kernel gives back
 * when an app needs it, and what is free, with a legend under it. */
static double draw_memory(cairo_t *cr, double y, const struct sysinfo *si) {
	long total = 0, free_kb = 0, avail = -1;
	FILE *f = fopen("/proc/meminfo", "r");
	char line[128];
	while (f && fgets(line, sizeof(line), f)) {
		sscanf(line, "MemTotal: %ld kB", &total);
		sscanf(line, "MemFree: %ld kB", &free_kb);
		sscanf(line, "MemAvailable: %ld kB", &avail);
	}
	if (f) {
		fclose(f);
	}
	if (total <= 0 || avail < 0) {
		return y;
	}
	struct {
		const char *name;
		long kb;
		double r, g, b;		/* r < 0: the accent colour */
	} parts[] = {
		{ "Apps", total - avail, -1, 0, 0 },
		{ "Cache (given back when needed)", avail - free_kb, 0.55, 0.55, 0.55 },
		{ "Free", free_kb, 0.22, 0.22, 0.22 },
	};
	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	zmc_text(cr, "Memory", FONT_REGULAR, 13, ZMC_MARGIN, y, 0);
	char value[96];
	snprintf(value, sizeof(value), "%.0f MB available of %.0f MB", avail / 1024.0, total / 1024.0);
	cairo_set_source_rgb(cr, 1, 1, 1);
	y += 20 + zmc_text(cr, value, FONT_REGULAR, 16, ZMC_MARGIN, y + 18, 0) + 8;
	double x = ZMC_MARGIN, w = zmc.width - 2 * ZMC_MARGIN, h = 16;
	for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
		double pw = w * (double)parts[i].kb / (double)total;
		if (parts[i].r < 0) {
			zmc_accent(cr);
		} else {
			cairo_set_source_rgb(cr, parts[i].r, parts[i].g, parts[i].b);
		}
		cairo_rectangle(cr, x, y, i + 1 < sizeof(parts) / sizeof(parts[0]) ? pw : ZMC_MARGIN + w - x, h);
		cairo_fill(cr);
		x += pw;
	}
	y += h + 10;
	for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
		if (parts[i].r < 0) {
			zmc_accent(cr);
		} else {
			cairo_set_source_rgb(cr, parts[i].r, parts[i].g, parts[i].b);
		}
		cairo_rectangle(cr, ZMC_MARGIN, y + 4, 12, 12);
		cairo_fill(cr);
		snprintf(value, sizeof(value), "%s  %.0f MB", parts[i].name, parts[i].kb / 1024.0);
		cairo_set_source_rgb(cr, 1, 1, 1);
		y += zmc_text(cr, value, FONT_REGULAR, 13.5, ZMC_MARGIN + 20, y, 0) + 4;
	}
	snprintf(value, sizeof(value), "Swap (compressed in RAM): %.0f MB used of %.0f MB",
		(si->totalswap - si->freeswap) * (double)si->mem_unit / 1048576.0,
		si->totalswap * (double)si->mem_unit / 1048576.0);
	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	y += zmc_text(cr, value, FONT_REGULAR, 13, ZMC_MARGIN, y + 2, 0) + 16;
	return y;
}

static double draw_system(cairo_t *cr) {
	char value[256], a[96];
	double y = zmc_header(cr, "system");
	struct utsname uts;
	if (uname(&uts) == 0) {
		snprintf(value, sizeof(value), "%s %s (%s)", uts.sysname, uts.release, uts.machine);
		y = zmc_field(cr, y, "Kernel", value);
	}
	zmc_read_file("/sys/firmware/devicetree/base/model", a, sizeof(a));
	y = zmc_field(cr, y, "Device tree model", a[0] ? a : "unknown");
	struct sysinfo si;
	if (sysinfo(&si) == 0) {
		snprintf(value, sizeof(value), "%ld h %ld min", si.uptime / 3600, (si.uptime % 3600) / 60);
		y = zmc_field(cr, y, "Uptime", value);
		y = draw_memory(cr, y, &si);
		snprintf(value, sizeof(value), "%d processes", si.procs);
		y = zmc_field(cr, y, "Tasks", value);
	}
	zmc_read_file("/proc/loadavg", a, sizeof(a));
	y = zmc_field(cr, y, "Load", a);
	struct statvfs fs;
	if (statvfs("/", &fs) == 0) {
		snprintf(value, sizeof(value), "%.1f GB free of %.1f GB",
			(double)fs.f_bavail * fs.f_frsize / 1e9, (double)fs.f_blocks * fs.f_frsize / 1e9);
		y = zmc_field(cr, y, "Root file system", value);
	}
	zmc_read_file("/proc/cmdline", value, sizeof(value));
	y = zmc_field(cr, y, "Kernel command line", value);
	snprintf(value, sizeof(value), "%s on %s", getenv("XDG_SESSION_TYPE") ? getenv("XDG_SESSION_TYPE") : "?",
		getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "?");
	y = zmc_field(cr, y, "Session", value);
	return y + 10;
}

static long read_long(const char *path) {
	char text[64];
	zmc_read_file(path, text, sizeof(text));
	return text[0] ? atol(text) : 0;
}

static double draw_sensors(cairo_t *cr) {
	char value[256], a[64], b[64];
	double y = zmc_header(cr, "sensors");
	zmc_read_file(hw_file(hw_battery(), "status"), a, sizeof(a));
	snprintf(value, sizeof(value), "%ld %%, %s", read_long(hw_file(hw_battery(), "capacity")), a);
	y = zmc_field(cr, y, "Battery", value);
	snprintf(value, sizeof(value), "%.3f V, %+.0f mA now, %+.0f mA average",
		read_long(hw_file(hw_battery(), "voltage_now")) / 1e6, read_long(hw_file(hw_battery(), "current_now")) / 1e3,
		read_long(hw_file(hw_battery(), "current_avg")) / 1e3);
	y = zmc_field(cr, y, "Cell", value);
	snprintf(value, sizeof(value), "%.1f C", read_long(hw_file(hw_battery(), "temp")) / 10.0);
	y = zmc_field(cr, y, "Battery temperature", value);
	zmc_read_file(hw_file(hw_charger(), "status"), a, sizeof(a));
	zmc_read_file(hw_file(hw_charger(), "charge_type"), b, sizeof(b));
	snprintf(value, sizeof(value), "%s (%s), input limit %ld mA, charge %ld mA", a, b,
		read_long(hw_file(hw_charger(), "input_current_limit")) / 1000, read_long(hw_file(hw_charger(), "constant_charge_current")) / 1000);
	y = zmc_field(cr, y, "Charger", value);

	for (int zone = 0; zone < 12; zone++) {
		char path[96], caption[96];
		snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone%d/type", zone);
		zmc_read_file(path, a, sizeof(a));
		if (!a[0]) {
			break;
		}
		snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone%d/temp", zone);
		snprintf(caption, sizeof(caption), "Heat: %s", a);
		snprintf(value, sizeof(value), "%.1f C", read_long(path) / 1000.0);
		y = zmc_field(cr, y, caption, value);
	}

	zmc_read_file("/sys/devices/system/cpu/online", a, sizeof(a));
	snprintf(value, sizeof(value), "little %ld MHz, big %ld MHz, cores %s",
		read_long("/sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq") / 1000,
		read_long("/sys/devices/system/cpu/cpufreq/policy4/scaling_cur_freq") / 1000, a);
	y = zmc_field(cr, y, "Processor clocks", value);
	zmc_read_file(hw_file(hw_gpu(), "governor"), a, sizeof(a));
	snprintf(value, sizeof(value), "%ld MHz (%s)", read_long(hw_file(hw_gpu(), "cur_freq")) / 1000000, a);
	y = zmc_field(cr, y, "GPU clock", value);

	/* whatever the kernel has under IIO: light, motion, pressure */
	DIR *dir = opendir("/sys/bus/iio/devices");
	struct dirent *entry;
	int found = 0;
	while (dir && (entry = readdir(dir))) {
		char path[320];
		if (strncmp(entry->d_name, "iio:device", 10) != 0) {
			continue;
		}
		snprintf(path, sizeof(path), "/sys/bus/iio/devices/%s/name", entry->d_name);
		zmc_read_file(path, a, sizeof(a));
		y = zmc_field(cr, y, "IIO sensor", a[0] ? a : entry->d_name);
		found++;
	}
	if (dir) {
		closedir(dir);
	}
	/* the sensor hub, through ztoscsf-sensord; the watches run out by themselves */
	static const char *const hub[][2] = {
		{ "light", "Light: lux, colour K, red green blue clear" }, { "accel", "Accelerometer: x y z" },
		{ "gyro", "Gyroscope: x y z" }, { "pressure", "Pressure (raw)" }, { "prox", "Proximity" },
	};
	int ctl = open("/run/ztoscsf-sensord.ctl", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	for (int i = 0; i < 5; i++) {
		char path[64];
		if (ctl >= 0 && i > 0) {
			dprintf(ctl, "watch %s\n", hub[i][0]);
		}
		snprintf(path, sizeof(path), "/run/ztoscsf-sensors/%s", hub[i][0]);
		zmc_read_file(path, a, sizeof(a));
		if (a[0]) {
			y = zmc_field(cr, y, hub[i][1], a);
			found++;
		}
	}
	if (ctl >= 0) {
		close(ctl);
	}
	if (!found) {
		y = zmc_field(cr, y, "Sensor hub", "no readings: ztoscsf-sensord is not running");
	}
	return y + 10;
}

static bool exists(const char *path) {
	return access(path, F_OK) == 0;
}

static bool check_part(size_t index) {
	switch (index) {
	case 0: /* Display */
		return exists("/sys/class/drm/card0-DSI-1") || exists("/sys/class/drm/card1-DSI-1") || exists("/sys/class/graphics/fb0");
	case 1: /* GPU */
		return exists(hw_gpu()) || exists("/sys/bus/platform/drivers/panfrost");
	case 2: /* Touch */
		return exists("/sys/bus/i2c/drivers/s6sy761") || exists("/sys/class/input/input1") || exists("/sys/class/input/input0");
	case 3: /* Wi-Fi */
		return exists("/sys/class/net/wlp1s0") || exists("/sys/class/net/wlan0") || exists("/sys/module/brcmfmac");
	case 4: /* Battery gauge */
		return hw_has("power") == 1 || exists(hw_battery()) || exists("/sys/class/power_supply/battery");
	case 5: /* Sound card */
		return exists("/proc/asound/ZrnAbox") || exists("/proc/asound/card0");
	case 6: /* Backlight device */
		/* the folder exists even when no driver registered a device */
		return hw_has("display") == 1 || exists(hw_file(hw_backlight(), "brightness"));
	case 7: /* Light and motion sensors */
		return hw_has("sensors") == 1 || exists("/sys/bus/iio/devices/iio:device0") || exists("/sys/module/zrn_bbd") || exists("/run/ztoscsf/sensord.sock");
	case 8: /* Vibrator */
		return hw_has("haptics") == 1 || exists("/sys/module/zrn_haptic") || exists("/sys/bus/platform/drivers/zrn-haptic") || exists("/sys/class/leds/vibrator");
	case 9: /* Modem */
		return hw_has("modem") == 1 || exists("/dev/umts_ipc0") || exists("/sys/class/net/rmnet0") || exists("/sys/module/zrn_modem") || exists("/sys/class/net/wwan0");
	case 10: /* Bluetooth */
		return exists("/sys/class/bluetooth/hci0");
	case 11: /* Camera */
		return exists("/dev/video0");
	case 12: /* Processor idle states */
		return exists("/sys/devices/system/cpu/cpu0/cpuidle") || exists("/sys/module/zrn_cpuidle");
	default:
		return false;
	}
}

static double draw_hardware(cairo_t *cr) {
	static const struct { const char *what, *missing; } parts[] = {
		{ "Display (DECON, DSI)", "no panel" },
		{ "GPU (Mali-G72, Panfrost)", "no GPU" },
		{ "Touch (s6sy761)", "no touch" },
		{ "Wi-Fi (brcmfmac)", "no Wi-Fi interface" },
		{ "Battery gauge (MAX77705)", "no gauge" },
		{ "Sound card", "none: no codec or audio DSP driver" },
		{ "Backlight device", "none: brightness is done by the shell" },
		{ "Light and motion sensors", "none: no sensor hub driver" },
		{ "Vibrator", "none: no haptics driver" },
		{ "Modem", "none: no modem driver" },
		{ "Bluetooth", "none" },
		{ "Camera", "none" },
		{ "Processor idle states", "none: cores only wait for interrupts" },
	};
	double y = zmc_header(cr, "hardware");
	for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
		y = zmc_field(cr, y, parts[i].what, check_part(i) ? "present" : parts[i].missing);
	}
	return y + 10;
}


/* ------------------------------------------------------------ animations
 *
 * Windows Phone 8.1's standard transitions, with the values read out of a
 * Lumia 530's firmware (UIXMobile!Animations.uix and the shell's own, in
 * compiled Iris markup; see the notes in OpensourceResources/lumia530).
 * Angles, durations, the feather and the scale are the firmware's; the
 * curves are Iris's Exp/Log/SCurve rebuilt from their names and weights,
 * which the firmware gives but whose exact formula lives in UIXrender.
 *
 * Played on a stage of four rows; each is drawn in Cairo into its own
 * surface and projected in slices, as ZrnMetroCore turns its rows.
 */

enum curve { C_LINEAR, C_EXP, C_LOG, C_SCURVE };
enum axis { AX_Y, AX_X, AX_Z };

struct anim {
	const char *name, *detail;
	enum axis axis;
	double from_deg, to_deg;	/* rotation */
	double pivot;			/* along the turning edge: 0 left/top .. 1 right/bottom */
	double from_scale, to_scale;
	double from_dy, to_dy;		/* points, downward */
	double from_alpha, to_alpha;
	double ms, feather_ms;		/* each row, and between rows */
	enum curve curve; double weight;
	bool reverse_order;		/* the last row first */
};

static const struct anim anims[] = {
	{ "turnstile forward in", "-80 to 0 about Y, rows 33 ms apart (TurnstileFeatherForwardIn)",
	  AX_Y, -80, 0, 0, 1, 1, 0, 0, 1, 1, 350, 33, C_LOG, 60, false },
	{ "turnstile forward out", "0 to 50 about Y, 128 ms, rows 33 ms apart (TurnstileFeatherForwardOut)",
	  AX_Y, 0, 50, 0, 1, 1, 0, 0, 1, 0, 128, 33, C_EXP, 60, false },
	{ "turnstile backward in", "50 to 0 about Y, the way back",
	  AX_Y, 50, 0, 0, 1, 1, 0, 0, 1, 1, 350, 33, C_LOG, 60, true },
	{ "turnstile backward out", "0 to -80 about Y, the way back",
	  AX_Y, 0, -80, 0, 1, 1, 0, 0, 1, 0, 128, 33, C_EXP, 60, true },
	{ "swivel in", "60 to 0 about X, Log 200: dialogs, menus, the status bar's list",
	  AX_X, -60, 0, 0.5, 1, 1, 0, 0, 0, 1, 300, 0, C_LOG, 200, false },
	{ "swivel out", "0 to 60 about X, 0.199 s: a dialog or a toast going",
	  AX_X, 0, 60, 0.5, 1, 1, 0, 0, 1, 0, 199, 0, C_EXP, 60, false },
	{ "continuum forward hide", "the tapped row tips back 80 and drops, Exp 60, 0.12 s",
	  AX_X, 0, 80, 0, 1, 1, 0, 120, 1, 0, 120, 0, C_EXP, 60, false },
	{ "continuum forward show", "-50 to 0 about X, scale 1.5 to 1, Log 60 (the next page's title)",
	  AX_X, -50, 0, 0, 1.5, 1, -60, 0, 0, 1, 350, 0, C_LOG, 60, false },
	{ "readerboard in", "-45 to 0 about X, 0.166 s per row, rows feathered",
	  AX_X, -45, 0, 0, 1, 1, 0, 0, 0, 1, 166, 33, C_LOG, 60, false },
	{ "readerboard out", "0 to 90 about X, 0.166 s per row, rows feathered",
	  AX_X, 0, 90, 0, 1, 1, 0, 0, 1, 0, 166, 33, C_EXP, 60, false },
	{ "slide up, fade in", "24 points up, Exp 150, 0.24 s (SlideUpFadeIn)",
	  AX_Y, 0, 0, 0, 1, 1, 24, 0, 0, 1, 240, 0, C_LOG, 150, false },
	{ "rotate 90", "90 about Z, 0.167 s (a page turned with the phone)",
	  AX_Z, 90, 0, 0.5, 1, 1, 0, 0, 1, 1, 167, 0, C_SCURVE, 7.5, false },
	{ "fade in", "alpha 0 to 1, Exp 150, 0.35 s",
	  AX_Y, 0, 0, 0, 1, 1, 0, 0, 0, 1, 350, 0, C_LOG, 150, false },
	{ "fade out", "alpha 1 to 0, Log 150, 0.15 s",
	  AX_Y, 0, 0, 0, 1, 1, 0, 0, 1, 0, 150, 0, C_EXP, 150, false },
};
#define N_ANIMS (int)(sizeof(anims) / sizeof(anims[0]))
#define STAGE_ROWS 4
#define STAGE_ROW_H 46
#define STAGE_GAP 8

static int shown_anim = -1;	/* the one on the preview page */
static uint32_t loop_start;
#define HOLD_MS 700	/* at rest between two plays */

/* Iris names: Exp gathers speed (things leaving), Log settles (arriving),
 * SCurve both; the weight sets how hard. */
static double shape(enum curve c, double w, double t) {
	double k = w / 30.0;
	switch (c) {
	case C_EXP: return (exp(k * t) - 1) / (exp(k) - 1);
	case C_LOG: return 1 - (exp(k * (1 - t)) - 1) / (exp(k) - 1);
	case C_SCURVE: {
		double f0 = 1 / (1 + exp(w / 2)), f1 = 1 / (1 + exp(-w / 2));
		return (1 / (1 + exp(-w * (t - 0.5))) - f0) / (f1 - f0);
	}
	default: return t;
	}
}

static double lerp(double a, double b, double t) { return a + (b - a) * t; }

/* The four rows, drawn once at the screen's scale and kept as textures. */
static cairo_surface_t *cards[STAGE_ROWS];
static double card_w;

static void cards_make(void) {
	static const char *labels[STAGE_ROWS] = { "wi-fi", "bluetooth", "theme", "display" };
	if (cards[0]) {
		return;
	}
	int sc = zmc.scale > 0 ? zmc.scale : 1;
	card_w = zmc.width - 2 * ZMC_MARGIN;
	for (int r = 0; r < STAGE_ROWS; r++) {
		cards[r] = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, (int)ceil(card_w * sc), STAGE_ROW_H * sc);
		cairo_t *c = cairo_create(cards[r]);
		cairo_scale(c, sc, sc);
		zmc_accent(c);
		cairo_rectangle(c, 0, 0, STAGE_ROW_H, STAGE_ROW_H);
		cairo_fill(c);
		cairo_set_source_rgb(c, 1, 1, 1);
		zmc_text(c, labels[r], FONT_SEMILIGHT, 24, STAGE_ROW_H + 12, 6, 0);
		cairo_destroy(c);
		cairo_surface_flush(cards[r]);
	}
}

/* One card at (x, y), turned `deg` about its axis through the pivot (a share
 * of its width for Y, of its height for X; the middle for Z), scaled about
 * its middle: four corners in points, z toward the viewer. */
static void card_quad(cairo_surface_t *card, double x, double y, double w, double h,
		enum axis axis, double deg, double pivot, double scale, double alpha, unsigned rev) {
	double a = deg * M_PI / 180, c[4][3];
	double lx[4] = { 0, w, 0, w }, ly[4] = { 0, 0, h, h };
	for (int i = 0; i < 4; i++) {
		double px = lx[i] - w / 2, py = ly[i] - h / 2, pz = 0;	/* about the middle */
		if (axis == AX_Y) {
			double u = lx[i] - w * pivot;
			px = w * pivot - w / 2 + u * cos(a); pz = u * sin(a);
		} else if (axis == AX_X) {
			double u = ly[i] - h * pivot;
			py = h * pivot - h / 2 + u * cos(a); pz = u * sin(a);
		} else {
			double rx = px * cos(a) - py * sin(a), ry = px * sin(a) + py * cos(a);
			px = rx; py = ry;
		}
		c[i][0] = x + w / 2 + px * scale;
		c[i][1] = y + h / 2 + py * scale;
		c[i][2] = pz * scale;
	}
	zmc_gpu_quad(card, rev, c, alpha);
}

static double stage_top(void) {
	return ZMC_HEADER_H + 120;
}

/* The preview page's moving part, every frame on the GPU. */
static void overlay(int page) {
	if (page != PAGE_PREVIEW || shown_anim < 0) {
		return;
	}
	cards_make();
	const struct anim *A = &anims[shown_anim];
	double run = A->ms + (STAGE_ROWS - 1) * A->feather_ms;
	double ms = fmod((double)(zmc_now_ms() - loop_start), run + HOLD_MS);
	double y0 = stage_top();
	for (int r = 0; r < STAGE_ROWS; r++) {
		double ry = y0 + r * (STAGE_ROW_H + STAGE_GAP);
		int order = A->reverse_order ? STAGE_ROWS - 1 - r : r;
		bool alone = A->feather_ms == 0 && r != 0;	/* single-element ones: the first row */
		if (alone) {
			card_quad(cards[r], ZMC_MARGIN, ry, card_w, STAGE_ROW_H, AX_Y, 0, 0, 1, 0.25, r + 1);
			continue;
		}
		double t = fmin(1, fmax(0, (ms - order * A->feather_ms) / A->ms));
		double e = shape(A->curve, A->weight, t);
		card_quad(cards[r], ZMC_MARGIN, ry + lerp(A->from_dy, A->to_dy, e), card_w, STAGE_ROW_H,
			A->axis, lerp(A->from_deg, A->to_deg, e), A->pivot,
			lerp(A->from_scale, A->to_scale, e), lerp(A->from_alpha, A->to_alpha, e), r + 1);
	}
}

static void preview_tick(void) {
	zmc_present();	/* a frame: the page stays as it is, the overlay moves */
}

static double draw_preview(cairo_t *cr) {
	if (shown_anim < 0) {
		return zmc_header(cr, "preview");
	}
	const struct anim *A = &anims[shown_anim];
	double y = zmc_header(cr, A->name);
	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	zmc_text(cr, A->detail, FONT_REGULAR, 13.5, ZMC_MARGIN, y, zmc.width - 2 * ZMC_MARGIN);
	return stage_top() + STAGE_ROWS * (STAGE_ROW_H + STAGE_GAP) + 40;
}

static double draw_anims(cairo_t *cr) {
	double y = zmc_header(cr, "animations");
	for (int i = 0; i < N_ANIMS; i++) {
		y = zmc_entry(cr, y, anims[i].name, anims[i].detail, ACT_PLAY, i);
	}
	return y + 20;
}

/* ---- task manager --------------------------------------------------------
 *
 * Everything running, in kinds, and any of it the user may end can be ended
 * after a warning box that says what ending it does. Each kind is a row with
 * its count and memory; tapped, it opens to its processes (the heaviest first).
 *
 *  your apps         the Android apps the user installed (ztOS's list of them:
 *                    one row each, all their processes), and the user's Linux
 *                    and SDK apps with windows (Wayland's client library)
 *  ztOS              its own programs (zt*, zrn*), the SDK's zygote, its apps
 *  Android system    the rest of the container (by its cgroup: Android's own
 *                    system user is uid 1000, as the phone's user is)
 *  services          the user's session and background services (PipeWire,
 *                    D-Bus, the portals), Waydroid's session
 *  shells + sessions shells, sudo, ssh, logins
 *  other programs    the user's, with no window
 *  system            root's and other users': listed, not ended from here
 *
 * Ending: an Android app (or an Android package) is force-stopped in Android
 * through the Waydroid recovery helper, which sudo allows the user; anything
 * else of the user's gets SIGTERM, then SIGKILL 3 s on. Kernel threads are
 * only counted. */

enum task_kind { KIND_APPS, KIND_ZTOS, KIND_ANDROID, KIND_SERVICES, KIND_SESSIONS, KIND_OTHER, KIND_SYSTEM,
	N_KINDS };

static const struct {
	const char *title, *warning;
} kinds[N_KINDS] = {
	[KIND_APPS] = { "your apps",
		"It closes, and anything not saved in it is lost." },
	[KIND_ZTOS] = { "ztOS",
		"This is part of ztOS. Ending it can take away the shell, the app list, sound, "
		"notifications or Settings until ztOS starts it again. Ending ztmetro (the shell) "
		"closes every app and returns to the lock screen." },
	[KIND_ANDROID] = { "Android system",
		"This is part of Android. Ending it can make Android restart, which closes every "
		"Android app; Android starts most of these again by itself." },
	[KIND_SERVICES] = { "services",
		"A background service of your session: sound, D-Bus, file and screen portals, or "
		"Waydroid. What needs it stops working until it is started again, or the phone is "
		"restarted." },
	[KIND_SESSIONS] = { "shells + sessions",
		"Ending it closes that terminal, remote login or sudo, and whatever runs in it." },
	[KIND_OTHER] = { "other programs",
		"A program of yours with no window, usually started by another. Ending it can "
		"break whatever started it." },
	[KIND_SYSTEM] = { "system",
		NULL },
};

#define MAX_TASKS	400
#define MAX_PKGS	64
#define MAX_ROWS_OPEN	140	/* of one kind, open: the page's taps are limited */

struct task {
	pid_t pid;		/* an Android app's: its main process */
	int kind;
	char name[64];
	char package[128];	/* force-stopped in Android when set */
	long rss_kb;
	double cpu;		/* percent of the whole phone */
	bool endable;
	bool android;
};

static struct task tasks[MAX_TASKS];
static int n_tasks, n_kernel;
static bool kind_open[N_KINDS] = { [KIND_APPS] = true };
static pid_t task_asked;		/* the warning box is open for it */
static char task_note[96];		/* what was ended, for a moment */

/* CPU time last seen, per pid, for the percentages */
static struct { pid_t pid; unsigned long long ticks; } cpu_seen[1024], cpu_now[1024];
static int n_seen, n_now;
static double cpu_seen_at;

static bool read_text(const char *path, char *out, size_t size) {
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) return false;
	ssize_t got = read(fd, out, size - 1);
	close(fd);
	if (got < 0) return false;
	out[got] = 0;
	return true;
}

/* A JSON string value after `key` within [from, to), into out */
static bool json_string(const char *from, const char *to, const char *key, char *out, size_t size) {
	char want[48];
	snprintf(want, sizeof(want), "\"%s\":", key);
	const char *p = strstr(from, want);
	if (!p || p >= to) return false;
	p = strchr(p + strlen(want), '"');
	if (!p || p >= to) return false;
	const char *end = strchr(p + 1, '"');
	if (!end || end >= to) return false;
	snprintf(out, size, "%.*s", (int)(end - p - 1), p + 1);
	return true;
}

/* The user's Android apps: package and name, from ztOS's list of them (an
 * object of objects, each with "package" and "name") */
static int android_apps(char pkgs[][128], char names[][64]) {
	static char buf[65536];
	char path[512];
	const char *home = getenv("HOME");
	snprintf(path, sizeof(path), "%s/.local/share/ztos/android/applications.json", home ? home : "");
	if (!read_text(path, buf, sizeof(buf))) return 0;
	int n = 0, depth = 0;
	const char *start = NULL;
	bool in_string = false;
	for (const char *p = buf; *p && n < MAX_PKGS; p++) {
		if (in_string) {
			if (*p == '\\' && p[1]) p++;
			else if (*p == '"') in_string = false;
			continue;
		}
		if (*p == '"') in_string = true;
		else if (*p == '{' && ++depth == 2) start = p;
		else if (*p == '}' && depth-- == 2 && start) {
			if (json_string(start, p, "package", pkgs[n], 128)) {
				if (!json_string(start, p, "name", names[n], 64)) {
					snprintf(names[n], 64, "%s", pkgs[n]);
				}
				bool seen = false;
				for (int i = 0; i < n; i++) seen = seen || strcmp(pkgs[i], pkgs[n]) == 0;
				if (!seen) n++;
			}
			start = NULL;
		}
	}
	return n;
}

static bool name_in(const char *comm, const char *const *names, size_t count) {
	for (size_t i = 0; i < count; i++) {
		if (!strcmp(comm, names[i])) return true;
	}
	return false;
}

/* /proc/PID/stat after "(comm) ": field n (state is 1) */
static const char *stat_field(const char *p, int n) {
	for (int field = 1; *p && field < n; p++) {
		if (*p == ' ') field++;
	}
	return p;
}

/* CPU ticks since the last look, and remembered for the next */
static unsigned long long cpu_since(pid_t pid, unsigned long long ticks) {
	if (n_now < 1024) {
		cpu_now[n_now].pid = pid;
		cpu_now[n_now].ticks = ticks;
		n_now++;
	}
	for (int i = 0; i < n_seen; i++) {
		if (cpu_seen[i].pid == pid) return ticks >= cpu_seen[i].ticks ? ticks - cpu_seen[i].ticks : 0;
	}
	return 0;
}

static bool maps_wayland(pid_t pid) {
	char path[64], line[512];
	snprintf(path, sizeof(path), "/proc/%d/maps", pid);
	FILE *f = fopen(path, "re");
	bool found = false;
	while (f && !found && fgets(line, sizeof(line), f)) {
		found = strstr(line, "libwayland-client") != NULL;
	}
	if (f) fclose(f);
	return found;
}

/* an Android package name (what the recovery helper takes), from a process's */
static bool package_of(const char *cmd, char *out, size_t size) {
	size_t n = strcspn(cmd, ":");
	if (n == 0 || n >= size || !strchr(cmd, '.') || strchr(cmd, '/')) return false;
	for (size_t i = 0; i < n; i++) {
		if (!isalnum((unsigned char)cmd[i]) && cmd[i] != '.' && cmd[i] != '_') return false;
	}
	if (!isalpha((unsigned char)cmd[0])) return false;
	snprintf(out, size, "%.*s", (int)n, cmd);
	return true;
}

static struct task *task_add(pid_t pid, int kind, const char *name, long rss_kb, double cpu) {
	if (n_tasks == MAX_TASKS) return NULL;
	struct task *t = &tasks[n_tasks++];
	memset(t, 0, sizeof(*t));
	t->pid = pid;
	t->kind = kind;
	snprintf(t->name, sizeof(t->name), "%s", name);
	t->rss_kb = rss_kb;
	t->cpu = cpu;
	return t;
}

static void tasks_scan(void) {
	static const char *const sessions[] = { "sh", "bash", "dash", "zsh", "fish", "sudo", "su", "login",
		"sshd", "sshd-session", "ssh", "ssh-agent", "agreety", "tmux", "screen" };
	static const char *const services[] = { "systemd", "(sd-pam)", "dbus-daemon", "dbus-broker",
		"dbus-broker-lau", "pipewire", "pipewire-pulse", "wireplumber", "gnome-keyring-d",
		"dconf-service", "hypridle", "Xwayland", "obexd", "mpris-proxy" };
	static char pkgs[MAX_PKGS][128], names[MAX_PKGS][64];
	int n_pkgs = android_apps(pkgs, names);
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	double now = ts.tv_sec + ts.tv_nsec / 1e9, span = cpu_seen_at > 0 ? now - cpu_seen_at : 0;
	double scale = span > 0 ? 100.0 / (span * sysconf(_SC_CLK_TCK) * sysconf(_SC_NPROCESSORS_CONF)) : 0;
	long page_kb = sysconf(_SC_PAGESIZE) / 1024;
	n_now = 0;
	n_tasks = n_kernel = 0;
	uid_t me = getuid();
	pid_t self = getpid();
	DIR *proc = opendir("/proc");
	struct dirent *e;
	while (proc && (e = readdir(proc))) {
		if (!isdigit((unsigned char)e->d_name[0])) continue;
		pid_t pid = atoi(e->d_name);
		char path[64], stat[1024], cmd[512], cgroup[512] = "", status[2048];
		snprintf(path, sizeof(path), "/proc/%d/stat", pid);
		if (!read_text(path, stat, sizeof(stat))) continue;
		char *open_paren = strchr(stat, '('), *close_paren = strrchr(stat, ')');
		if (!open_paren || !close_paren || !close_paren[1]) continue;
		char comm[32];
		snprintf(comm, sizeof(comm), "%.*s", (int)(close_paren - open_paren - 1), open_paren + 1);
		const char *fields = close_paren + 2;
		unsigned long long utime = 0, stime = 0;
		long rss_pages = 0;
		sscanf(stat_field(fields, 12), "%llu %llu", &utime, &stime);
		sscanf(stat_field(fields, 22), "%ld", &rss_pages);
		double cpu = cpu_since(pid, utime + stime) * scale;
		long rss_kb = rss_pages * page_kb;
		snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
		int fd = open(path, O_RDONLY | O_CLOEXEC);
		ssize_t cmd_len = fd >= 0 ? read(fd, cmd, sizeof(cmd) - 1) : -1;
		if (fd >= 0) close(fd);
		if (cmd_len <= 0) {
			n_kernel++;	/* a kernel thread */
			continue;
		}
		cmd[cmd_len] = 0;
		snprintf(path, sizeof(path), "/proc/%d/cgroup", pid);
		read_text(path, cgroup, sizeof(cgroup));
		unsigned uid = 0;
		snprintf(path, sizeof(path), "/proc/%d/status", pid);
		if (!read_text(path, status, sizeof(status))) continue;
		char *uid_line = strstr(status, "\nUid:");
		if (!uid_line || sscanf(uid_line + 5, "%u", &uid) != 1) continue;
		const char *base = strrchr(cmd, '/');
		base = base ? base + 1 : cmd;
		if (strstr(cgroup, "lxc.payload.waydroid")) {
			/* Android: an app the user installed, by its process name */
			int k = -1;
			for (int i = 0; i < n_pkgs && k < 0; i++) {
				size_t len = strlen(pkgs[i]);
				if (!strncmp(cmd, pkgs[i], len) && (cmd[len] == 0 || cmd[len] == ':')) k = i;
			}
			if (k >= 0) {
				int t = 0;
				while (t < n_tasks && !(tasks[t].android && !strcmp(tasks[t].package, pkgs[k]) &&
						tasks[t].kind == KIND_APPS)) {
					t++;
				}
				if (t == n_tasks) {
					struct task *added = task_add(pid, KIND_APPS, names[k], 0, 0);
					if (!added) continue;
					snprintf(added->package, sizeof(added->package), "%s", pkgs[k]);
					added->android = added->endable = true;
				}
				if (cmd[strlen(pkgs[k])] == 0) tasks[t].pid = pid;	/* the main one */
				tasks[t].rss_kb += rss_kb;
				tasks[t].cpu += cpu;
				continue;
			}
			struct task *t = task_add(pid, KIND_ANDROID, base, rss_kb, cpu);
			if (!t) continue;
			t->android = true;
			/* a package is force-stopped; a daemon of the same uid as the
			 * user's (Android's system user) gets a signal; others cannot */
			t->endable = package_of(cmd, t->package, sizeof(t->package)) || uid == me;
			continue;
		}
		if (uid != me) {
			struct task *t = task_add(pid, KIND_SYSTEM, comm, rss_kb, cpu);
			if (t) t->endable = false;
			continue;
		}
		int kind;
		char name[64];
		snprintf(name, sizeof(name), "%s", comm);
		bool python = !strncmp(comm, "python", 6);
		if (pid == self || !strncmp(comm, "zt", 2) || !strncmp(comm, "zrn", 3)) {
			kind = KIND_ZTOS;
		} else if (strstr(cgroup, "/session.slice/") || strstr(cgroup, "/background.slice/") ||
				name_in(comm, services, sizeof(services) / sizeof(services[0])) ||
				!strncmp(comm, "xdg-", 4) || !strncmp(comm, "gvfs", 4) || !strncmp(comm, "at-spi", 6)) {
			kind = KIND_SERVICES;
		} else if (name_in(comm, sessions, sizeof(sessions) / sizeof(sessions[0]))) {
			kind = KIND_SESSIONS;
		} else if (python) {
			/* the SDK's zygote; Waydroid's session; an SDK app (the zygote's
			 * child), named for its folder, ztOS's own under /usr */
			bool zygote = false, waydroid = false;
			for (const char *a = cmd; a < cmd + cmd_len; a += strlen(a) + 1) {
				zygote = zygote || strstr(a, "zygote.py");
				waydroid = waydroid || strstr(a, "waydroid");
			}
			char cwd[512];
			snprintf(path, sizeof(path), "/proc/%d/cwd", pid);
			ssize_t n = readlink(path, cwd, sizeof(cwd) - 1);
			bool system_app = false;
			if (n > 0) {
				cwd[n] = 0;
				const char *folder = strrchr(cwd, '/');
				system_app = !strncmp(cwd, "/usr/", 5);
				if (!zygote && !waydroid) snprintf(name, sizeof(name), "%s", folder ? folder + 1 : cwd);
			}
			if (zygote) snprintf(name, sizeof(name), "SDK zygote");
			if (waydroid) snprintf(name, sizeof(name), "Waydroid session");
			kind = waydroid ? KIND_SERVICES : zygote || system_app ? KIND_ZTOS :
				maps_wayland(pid) ? KIND_APPS : KIND_OTHER;
		} else {
			kind = maps_wayland(pid) ? KIND_APPS : KIND_OTHER;
		}
		struct task *t = task_add(pid, kind, name, rss_kb, cpu);
		if (t) t->endable = pid != self;	/* (this page ends itself with back) */
	}
	if (proc) closedir(proc);
	memcpy(cpu_seen, cpu_now, sizeof(cpu_now[0]) * n_now);
	n_seen = n_now;
	cpu_seen_at = now;
	/* by kind, the heaviest first in each */
	for (int i = 1; i < n_tasks; i++) {
		struct task t = tasks[i];
		int j = i - 1;
		while (j >= 0 && (tasks[j].kind > t.kind || (tasks[j].kind == t.kind && tasks[j].rss_kb < t.rss_kb))) {
			tasks[j + 1] = tasks[j];
			j--;
		}
		tasks[j + 1] = t;
	}
}

static struct task *task_find(pid_t pid) {
	for (int i = 0; i < n_tasks; i++) {
		if (tasks[i].pid == pid) return &tasks[i];
	}
	return NULL;
}

static void task_end(const struct task *t) {
	pid_t child = fork();
	if (child == 0) {
		setsid();
		if (t->package[0]) {
			execl("/usr/bin/sudo", "sudo", "-n", "/usr/local/libexec/ztos-waydroid-recover", "close",
				t->package, (char *)NULL);
		} else {
			char script[128];
			/* asked first, made to 3 s on */
			snprintf(script, sizeof(script), "kill -TERM %d; sleep 3; kill -0 %d 2>/dev/null && kill -KILL %d",
				t->pid, t->pid, t->pid);
			execl("/bin/sh", "sh", "-c", script, (char *)NULL);
		}
		_exit(127);
	}
	snprintf(task_note, sizeof(task_note), "%s ended", t->name);
}

static void task_detail(const struct task *t, char *out, size_t size) {
	if (!t->endable) {
		snprintf(out, size, "%ld MB · pid %d · %s", (t->rss_kb + 512) / 1024, t->pid,
			t->pid == getpid() ? "this task manager: back leaves it" : "not the user's: can't be ended here");
	} else {
		snprintf(out, size, "%s%ld MB · %.0f%% CPU · pid %d",
			t->kind == KIND_APPS ? (t->android ? "Android · " : "Linux · ") : "",
			(t->rss_kb + 512) / 1024, t->cpu, t->pid);
	}
}

/* The warning box over the page: what ending it does, and end or cancel. A
 * tap anywhere else cancels. Drawn last; it takes every tap. */
static void task_box(cairo_t *cr, const struct task *t) {
	cairo_save(cr);
	cairo_set_source_rgba(cr, 0, 0, 0, 0.75);
	cairo_paint(cr);
	cairo_restore(cr);
	zmc.n_hits = 0;
	double w = zmc.width, x = ZMC_MARGIN, y = zmc.scroll + 96, tw = w - 2 * ZMC_MARGIN;
	char title[96], detail[160];
	snprintf(title, sizeof(title), "end %s?", t->name);
	task_detail(t, detail, sizeof(detail));
	/* the box's height first, off screen, then the box */
	cairo_surface_t *probe_s = cairo_image_surface_create(CAIRO_FORMAT_A8, 1, 1);
	cairo_t *probe = cairo_create(probe_s);
	double h = 24 + zmc_text(probe, title, FONT_SEMILIGHT, 26, x, 0, tw) + 8 +
		zmc_text(probe, detail, FONT_REGULAR, 12.5, x, 0, tw) + 14 +
		zmc_text(probe, kinds[t->kind].warning, FONT_REGULAR, 15, x, 0, tw) + 24 + 40 + 24;
	cairo_destroy(probe);
	cairo_surface_destroy(probe_s);
	cairo_set_source_rgb(cr, 0.12, 0.12, 0.12);
	cairo_rectangle(cr, 0, y, w, h);
	cairo_fill(cr);
	zmc_accent(cr);
	cairo_rectangle(cr, 0, y, w, 4);
	cairo_fill(cr);
	y += 24;
	cairo_set_source_rgb(cr, 1, 1, 1);
	y += zmc_text(cr, title, FONT_SEMILIGHT, 26, x, y, tw) + 8;
	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	y += zmc_text(cr, detail, FONT_REGULAR, 12.5, x, y, tw) + 14;
	cairo_set_source_rgb(cr, 1, 1, 1);
	y += zmc_text(cr, kinds[t->kind].warning, FONT_REGULAR, 15, x, y, tw) + 24;
	double bw = (tw - 12) / 2;
	zmc_button(cr, x, y, bw, "end", ACT_TASK_END, t->pid);
	zmc_button(cr, x + bw + 12, y, bw, "cancel", ACT_TASK_CANCEL, 0);
	/* (the first hit under a point is the one taken: the buttons, then the rest) */
	zmc_add_hit(0, 0, w, 100000, ACT_TASK_CANCEL, 0);
}

static double draw_tasks(cairo_t *cr) {
	double y = zmc_header(cr, "task manager");
	char line[160];
	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	snprintf(line, sizeof(line), "%d processes · %d kernel threads not shown · tap a kind to open it",
		n_tasks, n_kernel);
	y += zmc_text(cr, line, FONT_REGULAR, 13, ZMC_MARGIN, y, zmc.width - 2 * ZMC_MARGIN) + 6;
	if (task_note[0]) {
		cairo_set_source_rgb(cr, 1, 1, 1);
		y += zmc_text(cr, task_note, FONT_REGULAR, 13.5, ZMC_MARGIN, y, 0) + 6;
	}
	y += 8;
	double tw = zmc.width - 2 * ZMC_MARGIN;
	for (int k = 0; k < N_KINDS; k++) {
		int count = 0;
		long kb = 0;
		for (int i = 0; i < n_tasks; i++) {
			if (tasks[i].kind == k) {
				count++;
				kb += tasks[i].rss_kb;
			}
		}
		char head[96];
		snprintf(head, sizeof(head), "%s %s", kind_open[k] ? "−" : "+", kinds[k].title);
		cairo_set_source_rgb(cr, 1, 1, 1);
		zmc_text(cr, head, FONT_SEMILIGHT, 22, ZMC_MARGIN, y, 0);
		cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
		snprintf(line, sizeof(line), "%d · %ld MB%s", count, (kb + 512) / 1024,
			k == KIND_SYSTEM ? " · root's and other users': not ended here" : "");
		zmc_text(cr, line, FONT_REGULAR, 12.5, ZMC_MARGIN, y + 30, 0);
		zmc_add_hit(0, y - 4, zmc.width, 54, ACT_TASK_KIND, k);
		y += 58;
		if (!kind_open[k]) continue;
		if (count == 0) {
			cairo_set_source_rgb(cr, 0.45, 0.45, 0.45);
			y += zmc_text(cr, "none running", FONT_REGULAR, 14, ZMC_MARGIN + 14, y, 0) + 12;
			continue;
		}
		int shown = 0;
		for (int i = 0; i < n_tasks && shown < MAX_ROWS_OPEN; i++) {
			const struct task *t = &tasks[i];
			if (t->kind != k) continue;
			shown++;
			char detail[160];
			task_detail(t, detail, sizeof(detail));
			cairo_set_source_rgb(cr, t->endable ? 1 : 0.55, t->endable ? 1 : 0.55, t->endable ? 1 : 0.55);
			zmc_text_line(cr, t->name, FONT_REGULAR, 17, ZMC_MARGIN + 14, y, tw - 14);
			cairo_set_source_rgb(cr, 0.55, 0.55, 0.55);
			zmc_text_line(cr, detail, FONT_REGULAR, 12, ZMC_MARGIN + 14, y + 22, tw - 14);
			if (t->endable) zmc_add_hit(0, y - 3, zmc.width, 42, ACT_TASK, t->pid);
			y += 44;
		}
		if (count > shown) {
			cairo_set_source_rgb(cr, 0.45, 0.45, 0.45);
			snprintf(line, sizeof(line), "and %d more", count - shown);
			y += zmc_text(cr, line, FONT_REGULAR, 13, ZMC_MARGIN + 14, y, 0) + 10;
		}
		y += 8;
	}
	const struct task *asked = task_asked ? task_find(task_asked) : NULL;
	if (asked) {
		task_box(cr, asked);
	} else {
		task_asked = 0;	/* gone in the meantime */
	}
	return y + 20;
}

static void tasks_tick(void) {
	while (waitpid(-1, NULL, WNOHANG) > 0) {
		/* the helpers task_end started, once done */
	}
	tasks_scan();
	zmc_redraw();
}

static void task_action(int action, int arg) {
	if (action == ACT_TASK_KIND && arg >= 0 && arg < N_KINDS) {
		kind_open[arg] = !kind_open[arg];
	} else if (action == ACT_TASK) {
		task_asked = arg;	/* the box asks first */
		task_note[0] = 0;
	} else if (action == ACT_TASK_CANCEL) {
		task_asked = 0;
	} else if (action == ACT_TASK_END) {
		task_asked = 0;
		/* still running, and still the user's to end: looked at again now */
		tasks_scan();
		const struct task *t = task_find(arg);
		if (t && t->endable) task_end(t);
	}
	zmc_redraw();
}

static double render(cairo_t *cr, int page) {
	switch (page) {
	case PAGE_HUB: return draw_hub(cr);
	case PAGE_LOGS: return draw_logs(cr);
	case PAGE_SYSTEM: return draw_system(cr);
	case PAGE_SENSORS: return draw_sensors(cr);
	case PAGE_JOURNAL: return draw_stream(cr, "journal");
	case PAGE_KERNEL: return draw_stream(cr, "kernel log");
	case PAGE_SHELL: return draw_stream(cr, "shell log");
	case PAGE_HARDWARE: return draw_hardware(cr);
	case PAGE_ANIM: return draw_anims(cr);
	case PAGE_PREVIEW: return draw_preview(cr);
	case PAGE_TASKS: return draw_tasks(cr);
	}
	return 0;
}

static void activate(int action, int arg) {
	if (action >= ACT_TASK && action <= ACT_TASK_CANCEL) {
		task_action(action, arg);
		return;
	}
	if (action == ACT_PLAY && arg >= 0 && arg < N_ANIMS) {
		shown_anim = arg;
		zmc_go(PAGE_PREVIEW, 1);
		return;
	}
	if (action == ACT_OPEN) {
		if (arg == PAGE_HUB || arg == PAGE_LOGS) {
			hub_tab = arg;
		}
		zmc_go(arg, 1);
	}
}

static void tick(void) {
	zmc_redraw();
}

static void arrived(int page) {
	if (page == PAGE_HUB || page == PAGE_LOGS) {
		hub_tab = page;	/* swiped to: where back returns to from under it */
	}
	switch (page) {
	case PAGE_SENSORS:
		zmc_every(1000, tick);	/* only while this page is open */
		break;
	case PAGE_JOURNAL:
		stream_start("LC_ALL=C.UTF-8 exec journalctl -f -n 40 -o short --no-pager 2>&1");
		break;
	case PAGE_KERNEL:
		stream_start("LC_ALL=C.UTF-8 exec journalctl -k -f -n 40 -o short-monotonic --no-pager 2>&1");
		break;
	case PAGE_SHELL:
		stream_start("LC_ALL=C.UTF-8 exec tail -n 40 -F \"$HOME/.local/state/ztmetro.log\" 2>&1");
		break;
	case PAGE_TASKS:
		task_asked = 0;
		task_note[0] = 0;
		tasks_scan();
		zmc_every(2000, tasks_tick);	/* only while this page is open */
		break;
	case PAGE_PREVIEW:
		loop_start = zmc_now_ms();
		zmc_every(16, preview_tick);	/* frames only while it is open */
		break;
	default:
		break;
	}
}

static bool back(void) {
	if (zmc.page == PAGE_HUB || zmc.page == PAGE_LOGS) {
		return false;	/* either tab of the hub: back leaves the app */
	}
	if (zmc.page == PAGE_TASKS && task_asked) {
		task_asked = 0;	/* back closes the warning box first */
		zmc_redraw();
		return true;
	}
	zmc_every(0, NULL);
	stream_stop();
	if (zmc.page == PAGE_PREVIEW) {
		zmc_go(PAGE_ANIM, -1);	/* back to the list of them */
		return true;
	}
	zmc_go(hub_tab, -1);
	return true;
}

static const struct zmc_app devtest = {
	.app_id = "zrndevtest", .title = "ZrnDevTest", .caps = "ZRNDEVTEST",
	.render = render, .activate = activate, .back = back, .arrived = arrived, .overlay = overlay,
};

int main(int argc, char **argv) {
	static const char *names[] = { "hub", "system", "sensors", "journal", "kernel", "shell", "hardware", "logs", "animations", "preview", "tasks" };
	if (argc == 4 && strcmp(argv[1], "--render") == 0) {
		/* ZT_ANIM=N ZT_ANIM_MS=T: the animations page with N played T ms in */
		if (getenv("ZT_ANIM")) {
			shown_anim = atoi(getenv("ZT_ANIM"));
		}
		for (int i = 0; i < 11; i++) {
			if (strcmp(names[i], argv[2]) == 0) {
				if (i == PAGE_TASKS) {
					/* ZT_TASKS_OPEN=1: every kind open; ZT_TASKS_ASK=PID: its warning box */
					for (int k = 0; getenv("ZT_TASKS_OPEN") && k < N_KINDS; k++) kind_open[k] = true;
					task_asked = getenv("ZT_TASKS_ASK") ? atoi(getenv("ZT_TASKS_ASK")) : 0;
					tasks_scan();
				}
				return zmc_render_png(&devtest, i, i == PAGE_ANIM ? 1400 : 664, argv[3]);
			}
		}
		return 2;
	}
	int status = zmc_run(&devtest, PAGE_HUB);
	stream_stop();
	return status;
}
