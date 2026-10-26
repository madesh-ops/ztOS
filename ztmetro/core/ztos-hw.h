/* ztos-hw.h: where this phone's battery, backlight and GPU are, for the apps
 * that read them directly (Settings, the hardware test). ztOSIAL says
 * (Power1.CapacityNode, Display1.Node, Perf1.GpuDevfreq), asked once; without
 * ztos-iald, the Galaxy S9's, which these apps were written on.
 *
 *	char path[256];
 *	snprintf(path, sizeof(path), "%s/capacity", hw_battery());
 *
 * Header only: each app that includes it gets its own copy. */
#ifndef ZTOS_HW_H
#define ZTOS_HW_H

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#ifdef HAVE_ZTOS_IAL
#include <ztos-ial.h>
#endif

#define HW_S9_BATTERY	"/sys/class/power_supply/max170xx_battery"
#define HW_S9_CHARGER	"/sys/class/power_supply/max77705-charger"
#define HW_S9_BACKLIGHT	"/sys/class/backlight/s9p-panel"
#define HW_S9_GPU	"/sys/class/devfreq/17500000.gpu"

/* Neither ztOSIAL nor the S9's node (another phone without ztos-iald running,
 * or one still on ALPHA-2): the first battery among the power supplies, the
 * first backlight, the first devfreq device named for a GPU. Left as it was
 * when there is none. */
static inline void hw_probe(char *cache, size_t size, const char *interface)
{
	const char *dir = !strcmp(interface, "Power1") ? "/sys/class/power_supply" :
			  !strcmp(interface, "Display1") ? "/sys/class/backlight" :
			  !strcmp(interface, "Perf1") ? "/sys/class/devfreq" : NULL;
	DIR *d = dir ? opendir(dir) : NULL;
	struct dirent *e;
	while (d && (e = readdir(d))) {
		char path[160], type[32] = "";
		if (e->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
		if (!strcmp(interface, "Power1")) {
			char type_path[192];
			snprintf(type_path, sizeof(type_path), "%s/type", path);
			FILE *f = fopen(type_path, "r");
			if (!f)
				continue;
			if (!fgets(type, sizeof(type), f))
				type[0] = 0;
			fclose(f);
			if (strncmp(type, "Battery", 7))
				continue;
		} else if (!strcmp(interface, "Perf1") && !strstr(e->d_name, "gpu")) {
			continue;
		}
		snprintf(cache, size, "%s", path);
		break;
	}
	if (d)
		closedir(d);
}

/* a property naming a node, or the S9's; `parent` drops the last component
 * (CapacityNode is the capacity file, the battery is its folder) */
static inline const char *hw_node(char *cache, size_t size, bool *asked, const char *interface,
				  const char *property, bool parent, const char *fallback)
{
	if (!*asked) {
		*asked = true;
		cache[0] = 0;
#ifdef HAVE_ZTOS_IAL
		if (ztos_ial_get_string(NULL, interface, property, cache, size) < 0)
			cache[0] = 0;
		if (parent && cache[0]) {
			char *slash = strrchr(cache, '/');
			if (slash)
				*slash = 0;
		}
#else
		(void)interface; (void)property; (void)parent;
#endif
		if (!cache[0])
			snprintf(cache, size, "%s", fallback);
		if (access(cache, F_OK) != 0)
			hw_probe(cache, size, interface);
	}
	return cache;
}

static inline const char *hw_battery(void)
{
	static char path[192];
	static bool asked;
	return hw_node(path, sizeof(path), &asked, "Power1", "CapacityNode", true, HW_S9_BATTERY);
}

static inline const char *hw_backlight(void)
{
	static char path[192];
	static bool asked;
	return hw_node(path, sizeof(path), &asked, "Display1", "Node", false, HW_S9_BACKLIGHT);
}

static inline const char *hw_gpu(void)
{
	static char path[192];
	static bool asked;
	return hw_node(path, sizeof(path), &asked, "Perf1", "GpuDevfreq", false, HW_S9_GPU);
}

/* the charger: the S9's, or the first supply that is not a battery and has
 * an online state (Power1 names no node for it yet) */
static inline const char *hw_charger(void)
{
	static char path[192];
	static bool asked;
	if (!asked) {
		asked = true;
		snprintf(path, sizeof(path), "%s", HW_S9_CHARGER);
		FILE *f = fopen(HW_S9_CHARGER "/online", "r");
		if (f) {
			fclose(f);
		} else {
			for (int i = 0; i < 8; i++) {
				const char *names[] = { "ac", "usb", "AC", "USB", "charger", "mains", "max77705-charger", "pm8941-charger" };
				char probe[160];
				snprintf(probe, sizeof(probe), "/sys/class/power_supply/%s/online", names[i]);
				if ((f = fopen(probe, "r"))) {
					fclose(f);
					snprintf(path, sizeof(path), "/sys/class/power_supply/%s", names[i]);
					break;
				}
			}
		}
	}
	return path;
}

/* "<node>/<leaf>" in one of four rotating buffers */
static inline const char *hw_file(const char *node, const char *leaf)
{
	static char buf[4][256];
	static int next;
	char *out = buf[next++ % 4];
	snprintf(out, sizeof(buf[0]), "%s/%s", node, leaf);
	return out;
}

/* the phone has this (Device1.Capabilities: "modem", "haptics", "sensors"...);
 * -1 when ztOSIAL cannot say, so the caller looks for itself */
static inline int hw_has(const char *capability)
{
#ifdef HAVE_ZTOS_IAL
	if (ztos_ial_has(NULL, "Device1"))
		return ztos_ial_has_capability(NULL, capability);
#else
	(void)capability;
#endif
	return -1;
}

#endif
