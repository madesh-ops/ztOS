/* The Files app's photo preview (included by ztfiles.c, after its list).
 *
 * A photo tapped in the list opens full screen on a page of its own, drawn
 * on the GPU from an .overlay: swipe sideways for the folder's other photos
 * (the next one slides in beside it), pinch to zoom, drag to look round a
 * zoomed photo, double tap to zoom in there or back out, tap to show or hide
 * the bar with its name and place in the folder. Back returns to the list.
 *
 * Photos are decoded with gdk-pixbuf (JPEG, PNG, GIF, WebP, BMP, TIFF, SVG,
 * whatever loaders are installed), turned the way their EXIF says, at up to
 * 1.5 times the screen's pixels so a zoom stays sharp; this one and its two
 * neighbours are kept, nothing more. Decoding is a thread's: the camera's
 * PNGs take about a second each on the M20, and decoded on the main thread
 * a swipe stood still and then jumped. A photo not decoded yet is drawn as
 * nothing (black) until it is. */
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <math.h>

#define PAGE_PHOTO	2
#define PHOTO_CACHE	3
#define PHOTO_ZOOM_MAX	4.0
#define PHOTO_BAR_H	64.0

static int photos[MAX_FILES];	/* files[] indices of the folder's photos, in order */
static int n_photos, photo_i;
static struct { int k; cairo_surface_t *sf; unsigned rev; bool failed; } photo_cache[PHOTO_CACHE] = {
	{ -1 }, { -1 }, { -1 } };
static unsigned photo_rev;
/* the view: a swipe's offset (points), zoom, and where a zoomed photo is moved to */
static double view_dx, view_target, zoom = 1, pan_x, pan_y;
static int view_dir;		/* animating: -1 to the previous, 1 to the next, 0 back in place */
static bool view_anim;
static bool bar_shown = true;
static cairo_surface_t *bar_sf;
static unsigned bar_rev;
static bool bar_stale = true;
static uint32_t tap_at;		/* a single tap waiting to see if it is a double one */
static double tap_x, tap_y;

static bool is_photo(const char *name) {
	static const char *const suffixes[] = { ".jpg", ".jpeg", ".png", ".gif", ".webp", ".bmp", ".tif", ".tiff",
		".svg", ".heic", ".heif", ".avif", ".jxl" };
	for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
		if (has_suffix(name, suffixes[i])) return true;
	}
	return false;
}

/* a GdkPixbuf into a cairo surface (premultiplied, as cairo keeps them) */
static cairo_surface_t *pixbuf_surface(GdkPixbuf *pb) {
	int w = gdk_pixbuf_get_width(pb), h = gdk_pixbuf_get_height(pb), n = gdk_pixbuf_get_n_channels(pb);
	cairo_surface_t *sf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
	if (cairo_surface_status(sf) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(sf);
		return NULL;
	}
	cairo_surface_flush(sf);
	const guchar *src = gdk_pixbuf_read_pixels(pb);
	int ss = gdk_pixbuf_get_rowstride(pb), ds = cairo_image_surface_get_stride(sf);
	unsigned char *dst = cairo_image_surface_get_data(sf);
	for (int y = 0; y < h; y++) {
		const guchar *p = src + (size_t)y * ss;
		uint32_t *d = (uint32_t *)(dst + (size_t)y * ds);
		for (int x = 0; x < w; x++, p += n) {
			uint32_t a = n == 4 ? p[3] : 255;
			d[x] = a << 24 | (uint32_t)(p[0] * a / 255) << 16 | (uint32_t)(p[1] * a / 255) << 8 |
				(uint32_t)(p[2] * a / 255);
		}
	}
	cairo_surface_mark_dirty(sf);
	return sf;
}

/* ---- decoding, on a thread of its own ---- */
struct photo_job {
	int k;
	gint gen;
	char path[PATH_SIZE];
	int bw, bh;
	cairo_surface_t *sf;
};
static GAsyncQueue *photo_requests, *photo_results;
static gint photo_gen = 1;	/* a new folder of photos: older jobs are dropped */
static int photo_inflight[8], n_inflight;
static void photo_step(void);

static gpointer photo_worker(gpointer data) {
	(void)data;
	for (;;) {
		struct photo_job *job = g_async_queue_pop(photo_requests);
		if (job->gen == g_atomic_int_get(&photo_gen)) {
			GdkPixbuf *pb = gdk_pixbuf_new_from_file_at_scale(job->path, job->bw, job->bh, TRUE, NULL);
			if (pb) {
				GdkPixbuf *up = gdk_pixbuf_apply_embedded_orientation(pb);
				g_object_unref(pb);
				if (up) {
					job->sf = pixbuf_surface(up);
					g_object_unref(up);
				}
			}
		}
		g_async_queue_push(photo_results, job);
	}
	return NULL;
}

static int photo_slot(int k) {
	for (int i = 0; i < PHOTO_CACHE; i++) {
		if (photo_cache[i].k == k) return i;
	}
	return -1;
}

static bool photo_requested(int k) {
	for (int i = 0; i < n_inflight; i++) {
		if (photo_inflight[i] == k) return true;
	}
	return false;
}

static void photo_request(int k) {
	if (k < 0 || k >= n_photos || photo_slot(k) >= 0 || photo_requested(k) ||
			n_inflight >= (int)(sizeof(photo_inflight) / sizeof(photo_inflight[0]))) {
		return;
	}
	if (!photo_requests) {
		photo_requests = g_async_queue_new();
		photo_results = g_async_queue_new();
		g_thread_unref(g_thread_new("photo-decode", photo_worker, NULL));
	}
	struct photo_job *job = g_new0(struct photo_job, 1);
	job->k = k;
	job->gen = g_atomic_int_get(&photo_gen);
	if (!path_for(files[photos[k]].name, job->path)) {
		g_free(job);
		return;
	}
	int bw = (int)(zmc.width * zmc.scale * 1.5), bh = (int)(zmc.height * zmc.scale * 1.5);
	job->bw = bw > 0 ? bw : 1620;
	job->bh = bh > 0 ? bh : 3510;
	photo_inflight[n_inflight++] = k;
	g_async_queue_push(photo_requests, job);
	if (zmc.running) zmc_on_frame(photo_step);	/* looked for each frame until it comes */
}

/* what the thread has finished, into the cache; true if anything came */
static bool photo_poll(void) {
	bool came = false;
	struct photo_job *job;
	while (photo_results && (job = g_async_queue_try_pop(photo_results))) {
		for (int i = 0; i < n_inflight; i++) {
			if (photo_inflight[i] == job->k && job->gen == g_atomic_int_get(&photo_gen)) {
				photo_inflight[i] = photo_inflight[--n_inflight];
				break;
			}
		}
		bool wanted = job->gen == g_atomic_int_get(&photo_gen) && abs(job->k - photo_i) <= 1 &&
			photo_slot(job->k) < 0;
		if (!wanted) {
			if (job->sf) cairo_surface_destroy(job->sf);
			g_free(job);
			continue;
		}
		/* an empty slot, or the one furthest from the photo in view */
		int slot = 0;
		for (int i = 0; i < PHOTO_CACHE; i++) {
			if (photo_cache[i].k < 0) { slot = i; break; }
			if (abs(photo_cache[i].k - photo_i) > abs(photo_cache[slot].k - photo_i)) slot = i;
		}
		if (photo_cache[slot].sf) cairo_surface_destroy(photo_cache[slot].sf);
		photo_cache[slot].k = job->k;
		photo_cache[slot].sf = job->sf;
		photo_cache[slot].failed = !job->sf;
		photo_cache[slot].rev = ++photo_rev;	/* a new picture: never a texture left from another */
		g_free(job);
		came = true;
	}
	return came;
}

/* the k'th photo if it is decoded, NULL (and asked for) if not */
static cairo_surface_t *photo_get(int k) {
	if (k < 0 || k >= n_photos) return NULL;
	int slot = photo_slot(k);
	if (slot >= 0) return photo_cache[slot].sf;
	photo_request(k);
	return NULL;
}

static bool photo_failed(int k) {
	int slot = photo_slot(k);
	return slot >= 0 && photo_cache[slot].failed;
}

static unsigned photo_rev_of(int k) {
	int slot = photo_slot(k);
	return slot >= 0 ? photo_cache[slot].rev : 0;
}

static void photo_cache_clear(void) {
	for (int i = 0; i < PHOTO_CACHE; i++) {
		if (photo_cache[i].sf) cairo_surface_destroy(photo_cache[i].sf);
		photo_cache[i].sf = NULL;
		photo_cache[i].k = -1;
	}
	g_atomic_int_inc(&photo_gen);	/* what is still being decoded is for nothing now */
	n_inflight = 0;
}

/* where the k'th photo is drawn, fitted to the screen: its size in points */
static void photo_fit(cairo_surface_t *sf, double *w, double *h) {
	double iw = cairo_image_surface_get_width(sf), ih = cairo_image_surface_get_height(sf);
	double r = fmin(zmc.width / iw, zmc.height / ih);
	*w = iw * r;
	*h = ih * r;
}

/* a zoomed photo cannot be moved off the screen: its edges stop at the screen's */
static void pan_clamp(void) {
	cairo_surface_t *sf = photo_get(photo_i);
	if (!sf) { pan_x = pan_y = 0; return; }
	double w, h;
	photo_fit(sf, &w, &h);
	double mx = fmax(0, (w * zoom - zmc.width) / 2), my = fmax(0, (h * zoom - zmc.height) / 2);
	pan_x = fmax(-mx, fmin(mx, pan_x));
	pan_y = fmax(-my, fmin(my, pan_y));
}

static void bar_draw(void) {
	int pw = zmc.width * zmc.scale, ph = (int)(PHOTO_BAR_H * zmc.scale);
	if (!bar_sf || cairo_image_surface_get_width(bar_sf) != pw) {
		if (bar_sf) cairo_surface_destroy(bar_sf);
		bar_sf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pw, ph);
	}
	cairo_t *cr = cairo_create(bar_sf);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_scale(cr, zmc.scale, zmc.scale);
	cairo_pattern_t *shade = cairo_pattern_create_linear(0, 0, 0, PHOTO_BAR_H);
	cairo_pattern_add_color_stop_rgba(shade, 0, 0, 0, 0, 0.75);
	cairo_pattern_add_color_stop_rgba(shade, 1, 0, 0, 0, 0.0);
	cairo_set_source(cr, shade);
	cairo_paint(cr);
	cairo_pattern_destroy(shade);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	char where[32];
	snprintf(where, sizeof(where), "%d / %d", photo_i + 1, n_photos);
	double ww = zmc_text_width(where, FONT_REGULAR, 14);
	cairo_set_source_rgb(cr, 0.85, 0.85, 0.85);
	zmc_text(cr, where, FONT_REGULAR, 14, zmc.width - 16 - ww, 18, 0);
	char name[96];
	snprintf(name, sizeof(name), "%.90s", photo_i < n_photos ? files[photos[photo_i]].name : "");
	cairo_rectangle(cr, 0, 0, zmc.width - 32 - ww, PHOTO_BAR_H);
	cairo_clip(cr);
	cairo_set_source_rgb(cr, 1, 1, 1);
	zmc_text(cr, name, FONT_SEMIBOLD, 16, 16, 16, 0);
	cairo_destroy(cr);
	cairo_surface_flush(bar_sf);
	bar_rev++;
	bar_stale = false;
}

static void photo_quad(int k, double dx, bool current) {
	cairo_surface_t *sf = photo_get(k);
	if (!sf) {
		return;
	}
	double w, h;
	photo_fit(sf, &w, &h);
	double z = current ? zoom : 1, px = current ? pan_x : 0, py = current ? pan_y : 0;
	double x = (zmc.width - w * z) / 2 + px + dx, y = (zmc.height - h * z) / 2 + py;
	double c[4][3] = { { x, y, 0 }, { x + w * z, y, 0 }, { x, y + h * z, 0 }, { x + w * z, y + h * z, 0 } };
	zmc_gpu_quad(sf, photo_rev_of(k), c, 1);
}

static void photo_overlay(int page) {
	if (page != PAGE_PHOTO) return;
	photo_poll();
	zmc_gpu_rect(0, 0, zmc.width, zmc.height, 0, 0, 0);
	photo_quad(photo_i, view_dx, true);
	/* a swipe: the neighbour coming in, a little gap between them */
	double gap = zmc.width + 16;
	if (view_dx < -0.5) photo_quad(photo_i + 1, view_dx + gap, false);
	if (view_dx > 0.5) photo_quad(photo_i - 1, view_dx - gap, false);
	if (bar_shown) {
		if (bar_stale || !bar_sf) bar_draw();
		zmc_gpu_quad(bar_sf, bar_rev, (double[4][3]){ { 0, 0, 0 }, { zmc.width, 0, 0 },
			{ 0, PHOTO_BAR_H, 0 }, { zmc.width, PHOTO_BAR_H, 0 } }, 1);
	}
}

/* the page as drawn for the page turn into it (the overlay takes over at rest) */
static double photo_render(cairo_t *cr) {
	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_paint(cr);
	cairo_surface_t *sf = photo_get(photo_i);
	if (sf) {
		double w, h;
		photo_fit(sf, &w, &h);
		cairo_save(cr);
		cairo_translate(cr, (zmc.width - w) / 2, (zmc.height - h) / 2);
		cairo_scale(cr, w / cairo_image_surface_get_width(sf), h / cairo_image_surface_get_height(sf));
		cairo_set_source_surface(cr, sf, 0, 0);
		cairo_paint(cr);
		cairo_restore(cr);
	} else if (photo_failed(photo_i)) {
		cairo_set_source_rgb(cr, 0.7, 0.7, 0.7);
		zmc_text(cr, "This photo cannot be shown.", FONT_REGULAR, 16, 24, zmc.height / 2 - 10, zmc.width - 48);
	}
	return zmc.height;
}

/* each frame while something moves: the swipe settling, a tap waiting */
static void photo_step(void) {
	bool busy = n_inflight > 0;	/* a photo on its way: looked for each frame */
	photo_poll();
	if (view_anim) {
		double d = view_target - view_dx;
		if (fabs(d) < 1.0) {
			view_dx = 0;
			view_anim = false;
			if (view_dir) {
				photo_i += view_dir;
				view_dir = 0;
				zoom = 1;
				pan_x = pan_y = 0;
				bar_stale = true;
				photo_request(photo_i + 1);	/* the next ones, decoding before they are swiped to */
				photo_request(photo_i - 1);
			}
		} else {
			view_dx += d * 0.28;
			busy = true;
		}
	}
	if (tap_at) {
		if ((int32_t)(zmc_now_ms() - tap_at) >= 280) {
			tap_at = 0;
			bar_shown = !bar_shown;	/* one tap: the bar comes or goes */
		} else {
			busy = true;
		}
	}
	if (!busy) zmc_on_frame(NULL);
}

static void photo_animate(void) {
	zmc_on_frame(photo_step);
	zmc_present();
}

/* fingers on a photo: one swipes (or moves a zoomed photo), two pinch */
static bool photo_touch(int phase, int id, double x, double y, uint32_t time) {
	static struct { int id; double x, y; bool down; } f[2] = { { -1 }, { -1 } };
	static double down_x, down_y, start_dist, start_zoom, start_pan_x, start_pan_y;
	static uint32_t down_time;
	static bool moved, pinched;
	(void)time;
	if (zmc.page != PAGE_PHOTO) return false;
	int slot = -1;
	for (int i = 0; i < 2; i++) if (f[i].down && f[i].id == id) slot = i;
	if (phase == 0) {
		if (slot < 0) for (int i = 0; i < 2; i++) if (!f[i].down) { slot = i; break; }
		if (slot < 0) return true;
		f[slot].id = id; f[slot].x = x; f[slot].y = y; f[slot].down = true;
		int n = f[0].down + f[1].down;
		if (n == 1) {
			down_x = x; down_y = y; down_time = zmc_now_ms();
			moved = pinched = false;
			start_pan_x = pan_x; start_pan_y = pan_y;
			view_anim = false;
		} else {
			pinched = moved = true;
			start_dist = hypot(f[0].x - f[1].x, f[0].y - f[1].y);
			start_zoom = zoom;
			start_pan_x = pan_x; start_pan_y = pan_y;
			view_dx = 0;
		}
		return true;
	}
	if (slot < 0) return true;
	if (phase == 1) {
		f[slot].x = x; f[slot].y = y;
		if (f[0].down && f[1].down) {
			double dist = hypot(f[0].x - f[1].x, f[0].y - f[1].y);
			if (start_dist > 10) {
				double z = fmax(1, fmin(PHOTO_ZOOM_MAX, start_zoom * dist / start_dist));
				pan_x = start_pan_x * z / start_zoom;
				pan_y = start_pan_y * z / start_zoom;
				zoom = z;
				pan_clamp();
			}
		} else if (!pinched) {
			double dx = x - down_x, dy = y - down_y;
			if (hypot(dx, dy) > 10) moved = true;
			if (zoom > 1.01) {
				pan_x = start_pan_x + dx;
				pan_y = start_pan_y + dy;
				pan_clamp();
			} else {
				/* at the first and last photo it gives only a little */
				bool edge = (dx > 0 && photo_i == 0) || (dx < 0 && photo_i == n_photos - 1);
				view_dx = edge ? dx * 0.25 : dx;
			}
		}
		zmc_present();
		return true;
	}
	/* lifted (or the touch was taken away) */
	f[slot].down = false;
	if (f[0].down || f[1].down) return true;	/* one finger of a pinch still down */
	if (phase == 3) { view_target = 0; view_dir = 0; view_anim = true; photo_animate(); return true; }
	if (pinched) {
		if (zoom < 1.05) { zoom = 1; pan_x = pan_y = 0; }
		zmc_present();
		return true;
	}
	if (!moved && zmc_now_ms() - down_time < 400) {
		if (tap_at && hypot(down_x - tap_x, down_y - tap_y) < 40) {
			/* a double tap: zoom in where it was, or back out */
			tap_at = 0;
			if (zoom > 1.01) {
				zoom = 1; pan_x = pan_y = 0;
			} else {
				zoom = 2.5;
				pan_x = (zmc.width / 2 - down_x) * (zoom - 1);
				pan_y = (zmc.height / 2 - down_y) * (zoom - 1);
				pan_clamp();
			}
		} else {
			tap_at = zmc_now_ms();
			tap_x = down_x; tap_y = down_y;
		}
		photo_animate();
		return true;
	}
	if (zoom <= 1.01) {
		double w = zmc.width;
		if (view_dx < -w * 0.18 && photo_i < n_photos - 1) {
			view_target = -(w + 16); view_dir = 1;
		} else if (view_dx > w * 0.18 && photo_i > 0) {
			view_target = w + 16; view_dir = -1;
		} else {
			view_target = 0; view_dir = 0;
		}
		view_anim = true;
		photo_animate();
	}
	return true;
}

/* the list's file `index` (a photo) made the one in view, with the folder's
 * others to swipe to */
static void photo_prepare(int index) {
	photo_cache_clear();
	n_photos = 0;
	photo_i = 0;
	for (int i = 0; i < n_files; i++) {
		if (files[i].directory || !is_photo(files[i].name)) continue;
		if (i == index) photo_i = n_photos;
		photos[n_photos++] = i;
	}
	view_dx = 0; view_anim = false; view_dir = 0;
	zoom = 1; pan_x = pan_y = 0;
	bar_shown = true; bar_stale = true; tap_at = 0;
	photo_get(photo_i);
}

static void photo_open(int index) {
	photo_prepare(index);
	zmc_go(PAGE_PHOTO, 1);
}

static void photo_arrived(void) {
	photo_request(photo_i);
	photo_request(photo_i + 1);
	photo_request(photo_i - 1);
	if (n_inflight) zmc_on_frame(photo_step);	/* (asked for before the app was running) */
}
