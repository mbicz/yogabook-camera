/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * yb-camerad - the Yoga Book YB1-X90F cameras as two PipeWire cameras.
 *
 * atomisp exposes both sensors as inputs of one /dev/video0 (front OV2740,
 * rear OV8858). PipeWire's own V4L2 node can show only one input, carries no
 * location (so GNOME Camera mirrors the rear like a selfie camera) and never
 * rotates. This daemon replaces it (WirePlumber disables the raw node):
 *
 *   - publishes "Yoga Book front camera" (api.libcamera.location=front) and
 *     "Yoga Book rear camera" (location=back), always both: apps switch
 *     between them themselves (GNOME Camera's camera-switch button);
 *   - opens /dev/video0 only while an app streams, selects that sensor's
 *     input and copies its frames out;
 *   - rotates the frames to follow GNOME's screen rotation: each camera has a
 *     table of clockwise turns, one per transform of the built-in panel in
 *     org.gnome.Mutter.DisplayConfig (--front-rot, --rear-rot). A node offers
 *     only the upright size it had when created; an idle node whose size went
 *     stale is replaced (see create_stream). A turn mid-session never
 *     renegotiates: on_process keeps the image upright and, if the turn changed
 *     the orientation, fits it letterboxed into the negotiated size;
 *   - turns the rear camera a further --rear-unfolded degrees, and with
 *     --rear-unfolded-mirror also mirrors it, while the machine is not folded
 *     flat (the "hinge" IIO sensor, polled only while the rear camera
 *     streams). The OV8858 sits on the keyboard half, so opening the hinge
 *     turns it towards the user;
 *   - stamps every buffer with a monotonic pts (SPA_META_Header). This
 *     stream drives the graph and never advances its clock, so without the
 *     header pipewiresrc gives every frame the same timestamp and a syncing
 *     video sink (GNOME Camera's) stops presenting after a few frames.
 *
 * Output is I420 in GStreamer's default layout (4-byte aligned strides) so
 * pipewiresrc computes the same plane offsets.
 *
 * Build: gcc -O2 -Wall -o yb-camerad yb-camerad.c \
 *            $(pkg-config --cflags --libs libpipewire-0.3 libsystemd)
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <glob.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

#include <linux/media.h>
#include <linux/videodev2.h>

#include <pipewire/pipewire.h>
#include <spa/buffer/meta.h>
#include <spa/param/buffers.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>
#include <systemd/sd-bus.h>

#define NBUF 4
#define TILE 32
#define FPS 30

enum { CAM_FRONT, CAM_REAR, N_CAMS };

/* OV8858 module OTP travel limits, not the WV517's full 0..1023 range. */
#define REAR_FOCUS_MIN 225
#define REAR_FOCUS_MAX 414
#define REAR_FOCUS_DEFAULT 300
#define AF_COARSE_STEP 24
#define AF_FINE_STEP 3
#define AF_COLS 7
#define AF_ROWS 5
#define AF_TILES (AF_COLS * AF_ROWS)
#define AF_PATCH 48
#define AF_SAMPLES 3
#define AF_REACQUIRE_NS (5ULL * SPA_NSEC_PER_SEC)

enum af_phase { AF_WAIT, AF_COARSE, AF_FINE, AF_RETURN, AF_LOCKED };

struct af_measure {
	double score;
	int scene[AF_TILES];
	bool usable;
};

struct autofocus {
	int fd, position, good_position;
	enum af_phase phase;
	int next, end, best_position, baseline_position;
	int fine_center, fine_step;
	unsigned fine_sample;
	double best_score, low_score, baseline_score, locked_score;
	double scores[AF_SAMPLES];
	unsigned samples, settle_frames, invalid, changed, votes;
	uint64_t settle_until, locked_at;
	int scene[AF_TILES];
	int previous_scene[AF_TILES];
	bool observed;
	bool good, locked_usable, converged;
};

struct data;

struct cam {
	struct data *d;
	const char *key;		/* "front" / "rear" */
	const char *sensor;		/* V4L2 input name prefix */
	const char *location;		/* api.libcamera.location */
	const char *description;
	int input;			/* V4L2 input index, -1 if absent */
	uint32_t w, h;			/* sensor frame size */
	int rot[4];			/* degrees cw per screen transform */
	int unfolded_rot;		/* extra degrees cw when not folded flat */
	bool unfolded_mirror;		/* mirror left-right when not folded flat */

	struct pw_stream *stream;
	struct spa_hook listener;
	bool want;			/* an app is streaming from us */
	uint32_t neg_w, neg_h;		/* negotiated size, 0 if none */
	uint32_t seq;			/* buffer sequence number */
	uint32_t fmt_w, fmt_h;		/* upright size last offered first */
};

struct data {
	struct pw_main_loop *main_loop;
	struct pw_loop *loop;
	struct pw_context *context;
	struct pw_core *core;
	struct cam cams[N_CAMS];

	const char *dev_path;
	int fd;				/* -1 while the camera is closed */
	struct cam *active;
	struct spa_source *v4l2_src;
	void *buf[NBUF];
	size_t buf_len[NBUF];
	uint32_t v4l2_w, v4l2_h, v4l2_stride;
	int pending;			/* dequeued V4L2 buffer, -1 if none */
	struct autofocus af;
	/* per camera session, reported when it stops */
	unsigned n_in, n_process, n_out, n_nobuf, n_inactive, n_nopending;

	sd_bus *bus;
	struct spa_source *bus_src;
	struct spa_source *retry;
	int transform;			/* panel transform, -1 unknown */
	bool test_pattern;		/* --test-pattern */
	uint8_t *pattern;		/* replaces sensor frames, NULL if off */
	struct spa_source *sync_timer;
	struct spa_source *start_timer;	/* retries a failed camera_start */
	const char *hinge_path;		/* hinge angle file, NULL if none */
	struct spa_source *hinge_timer;	/* polls the hinge while needed */
	bool folded;			/* keyboard half folded behind the screen */
	int fold_votes;			/* polls in a row disagreeing with folded */
	uint32_t xmap[4096];		/* fit_plane column map */
};

static void msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void msg(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

/* ---- rear lens and contrast autofocus --------------------------------- */

static uint64_t af_now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * SPA_NSEC_PER_SEC + t.tv_nsec;
}

/* Match graph interfaces by dev_t, never by a boot-dependent subdev index. */
static int af_open_node(unsigned maj, unsigned min)
{
	glob_t g = { 0 };
	int fd = -1;

	if (glob("/dev/v4l-subdev*", 0, NULL, &g) == 0) {
		for (size_t i = 0; i < g.gl_pathc; i++) {
			struct stat st;

			if (stat(g.gl_pathv[i], &st) == 0 && S_ISCHR(st.st_mode) &&
			    major(st.st_rdev) == maj && minor(st.st_rdev) == min) {
				fd = open(g.gl_pathv[i], O_RDWR | O_CLOEXEC);
				break;
			}
		}
	}
	globfree(&g);
	return fd;
}

static int af_find_lens(int video_fd)
{
	struct stat video;
	glob_t g = { 0 };
	int result = -1;

	if (fstat(video_fd, &video) < 0)
		return -1;
	if (glob("/dev/media*", 0, NULL, &g) != 0) {
		globfree(&g);
		return -1;
	}
	for (size_t n = 0; n < g.gl_pathc && result < 0; n++) {
		struct media_v2_topology t = { 0 };
		struct media_v2_entity *entities = NULL;
		struct media_v2_interface *interfaces = NULL;
		struct media_v2_link *links = NULL;
		uint32_t ne, ni, nl, sensor = 0;
		bool our_video = false;
		int fd = open(g.gl_pathv[n], O_RDONLY | O_CLOEXEC);

		if (fd < 0)
			continue;
		if (ioctl(fd, MEDIA_IOC_G_TOPOLOGY, &t) < 0)
			goto next;
		ne = t.num_entities;
		ni = t.num_interfaces;
		nl = t.num_links;
		entities = calloc(ne, sizeof(*entities));
		interfaces = calloc(ni, sizeof(*interfaces));
		links = calloc(nl, sizeof(*links));
		if (!entities || !interfaces || !links)
			goto next;
		t.ptr_entities = (uintptr_t)entities;
		t.ptr_interfaces = (uintptr_t)interfaces;
		t.ptr_links = (uintptr_t)links;
		if (ioctl(fd, MEDIA_IOC_G_TOPOLOGY, &t) < 0 ||
		    t.num_entities != ne || t.num_interfaces != ni || t.num_links != nl)
			goto next;
		for (uint32_t i = 0; i < ni; i++)
			if (interfaces[i].intf_type == MEDIA_INTF_T_V4L_VIDEO &&
			    interfaces[i].devnode.major == major(video.st_rdev) &&
			    interfaces[i].devnode.minor == minor(video.st_rdev))
				our_video = true;
		for (uint32_t i = 0; i < ne; i++)
			if (entities[i].function == MEDIA_ENT_F_CAM_SENSOR &&
			    !strncmp(entities[i].name, "ov8858", 6))
				sensor = entities[i].id;
		if (!our_video || !sensor)
			goto next;
		for (uint32_t i = 0; i < ne && result < 0; i++) {
			bool associated = false;

			if (entities[i].function != MEDIA_ENT_F_LENS ||
			    !strstr(entities[i].name, "wv517"))
				continue;
			for (uint32_t j = 0; j < nl; j++)
				if ((links[j].flags & MEDIA_LNK_FL_LINK_TYPE) ==
				    MEDIA_LNK_FL_ANCILLARY_LINK &&
				    ((links[j].source_id == sensor &&
				      links[j].sink_id == entities[i].id) ||
				     (links[j].sink_id == sensor &&
				      links[j].source_id == entities[i].id)))
					associated = true;
			if (!associated)
				continue;
			for (uint32_t j = 0; j < nl && result < 0; j++) {
				uint32_t intf;

				if ((links[j].flags & MEDIA_LNK_FL_LINK_TYPE) !=
				    MEDIA_LNK_FL_INTERFACE_LINK)
					continue;
				if (links[j].source_id == entities[i].id)
					intf = links[j].sink_id;
				else if (links[j].sink_id == entities[i].id)
					intf = links[j].source_id;
				else
					continue;
				for (uint32_t k = 0; k < ni; k++)
					if (interfaces[k].id == intf &&
					    interfaces[k].intf_type == MEDIA_INTF_T_V4L_SUBDEV)
						result = af_open_node(interfaces[k].devnode.major,
								      interfaces[k].devnode.minor);
			}
		}
next:
		free(links);
		free(interfaces);
		free(entities);
		close(fd);
	}
	globfree(&g);
	return result;
}

static void af_stop(struct autofocus *a)
{
	if (a->fd >= 0)
		close(a->fd);	/* The lens driver parks and drops its power gate. */
	a->fd = -1;
}

static bool af_move(struct autofocus *a, int position)
{
	struct v4l2_control ctrl = {
		.id = V4L2_CID_FOCUS_ABSOLUTE,
		.value = position,
	};

	if (ioctl(a->fd, VIDIOC_S_CTRL, &ctrl) < 0) {
		msg("rear AF unavailable: FOCUS_ABSOLUTE %d: %m (video continues)",
		    position);
		af_stop(a);
		return false;
	}
	a->position = position;
	/* Start after the control write, not before a possibly delayed ioctl. */
	a->settle_until = af_now() + 100 * SPA_NSEC_PER_MSEC;
	a->settle_frames = NBUF;
	a->samples = a->invalid = a->changed = 0;
	return true;
}

static void af_start(struct data *d)
{
	struct autofocus *a = &d->af;
	struct v4l2_queryctrl q = { .id = V4L2_CID_FOCUS_ABSOLUTE };

	if (d->test_pattern || d->active != &d->cams[CAM_REAR])
		return;
	a->fd = af_find_lens(d->fd);
	if (a->fd < 0) {
		msg("rear AF unavailable: no accessible WV517 lens associated with OV8858 (video continues)");
		return;
	}
	if (ioctl(a->fd, VIDIOC_QUERYCTRL, &q) < 0 ||
	    q.type != V4L2_CTRL_TYPE_INTEGER || q.minimum > REAR_FOCUS_MIN ||
	    q.maximum < REAR_FOCUS_MAX || q.step != 1 ||
	    (q.flags & (V4L2_CTRL_FLAG_DISABLED | V4L2_CTRL_FLAG_READ_ONLY |
			V4L2_CTRL_FLAG_INACTIVE))) {
		msg("rear AF unavailable: lens has no usable FOCUS_ABSOLUTE control (video continues)");
		af_stop(a);
		return;
	}
	a->phase = AF_WAIT;
	a->votes = 0;
	a->observed = false;
	if (!af_move(a, a->good ? a->good_position : REAR_FOCUS_DEFAULT))
		return;
	/* Let the sensor's initial exposure settle before evaluating contrast. */
	a->settle_until += 500 * SPA_NSEC_PER_MSEC;
	msg("rear AF ready: WV517, calibrated search %d..%d", REAR_FOCUS_MIN,
	    REAR_FOCUS_MAX);
}

static int af_luma(const uint8_t *p, uint32_t stride, unsigned x, unsigned y)
{
	p += (size_t)y * stride + x;
	return (p[0] + p[1] + p[stride] + p[stride + 1] + 2) / 4;
}

/* General center-weighted contrast AF, before geometry. Dense patches span
 * the middle three quarters, including the optical center. A smooth kernel
 * gives the middle 3x3 patches 56% of the weight, the surroundings 44%:
 * objects/faces need not intersect a tiny center spot. Work is bounded at
 * 35 * 48 * 48 pixels, without allocations or a full-frame image pass.
 *
 * Combine fine and wider gradients at equal slope scale. The wider channel
 * retains soft contours; the fine one resolves small detail. Dense 48-pixel
 * patches reduce changes in sampled edge occupancy as focus breathes.
 * Black-corrected normalization removes global AE gain. */
static struct af_measure af_measure(const uint8_t *p, uint32_t w, uint32_t h,
				    uint32_t stride)
{
	static const unsigned wx[AF_COLS] = { 1, 2, 4, 6, 4, 2, 1 };
	static const unsigned wy[AF_ROWS] = { 1, 2, 4, 2, 1 };
	struct af_measure m = { 0 };
	uint64_t sum = 0, sum2 = 0, fine = 0, middle = 0, coarse = 0, noise = 0;
	uint64_t count = 0;
	unsigned tiles[AF_TILES] = { 0 };
	unsigned side;

	if (w < 64 || h < 64 || stride < w)
		return m;
	side = w / 12;
	if (side > h / 8)
		side = h / 8;
	if (side > AF_PATCH)
		side = AF_PATCH;
	unsigned span_x = w * 3 / 4 - 16 - side;
	unsigned span_y = h * 3 / 4 - 16 - side;
	for (unsigned ty = 0; ty < AF_ROWS; ty++)
		for (unsigned tx = 0; tx < AF_COLS; tx++) {
			unsigned x0 = w / 2 - side / 2 +
				((int)tx - 3) * (int64_t)span_x / 6;
			unsigned y0 = h / 2 - side / 2 +
				((int)ty - 2) * (int64_t)span_y / 4;
			unsigned weight = wx[tx] * wy[ty];

			count += side * side * weight;
			for (unsigned y = y0; y < y0 + side; y++)
				for (unsigned x = x0; x < x0 + side; x++) {
					int v = af_luma(p, stride, x, y);
					int dx = af_luma(p, stride, x + 1, y) -
						 af_luma(p, stride, x - 1, y);
					int dy = af_luma(p, stride, x, y + 1) -
						 af_luma(p, stride, x, y - 1);
					int mx = af_luma(p, stride, x + 4, y) -
						 af_luma(p, stride, x - 4, y);
					int my = af_luma(p, stride, x, y + 4) -
						 af_luma(p, stride, x, y - 4);
					int cx = af_luma(p, stride, x + 8, y) -
						 af_luma(p, stride, x - 8, y);
					int cy = af_luma(p, stride, x, y + 8) -
						 af_luma(p, stride, x, y - 8);
					const uint8_t *s = p + (size_t)y * stride + x;
					int n = s[0] - s[1] - s[stride] + s[stride + 1];

					sum += v * weight;
					sum2 += v * v * weight;
					fine += (dx * dx + dy * dy) * weight;
					middle += (mx * mx + my * my) * weight;
					coarse += (cx * cx + cy * cy) * weight;
					noise += n * n * weight;
					tiles[ty * AF_COLS + tx] += v;
				}
		}
	double mean = (double)sum / count, signal = mean - 16;
	double variance = (double)sum2 / count - mean * mean;
	/* Each two-axis averaged gradient has noise/4 noise energy. Wider
	 * gradients are scaled by 1/16, hence the combined 17/64 subtraction. */
	m.usable = mean > 28 && mean < 235 && variance > 4 &&
		   coarse > noise && fine + (double)middle / 16 > noise / 2;
	if (signal > 1) {
		m.score = ((double)fine + (double)middle / 16 -
			   (double)noise * 17 / 64) / (count * signal * signal);
		for (unsigned i = 0; i < AF_TILES; i++)
			m.scene[i] = 256 * ((double)tiles[i] / (side * side) - 16) / signal;
	}
	return m;
}

static unsigned af_scene_change(const int *from, const int *to)
{
	unsigned delta = 0;

	for (unsigned i = 0; i < AF_TILES; i++)
		delta += abs(from[i] - to[i]);
	return delta / AF_TILES;
}

static double af_median(const double *s)
{
	double lo = s[0] < s[1] ? s[0] : s[1];
	double hi = s[0] > s[1] ? s[0] : s[1];

	return s[2] < lo ? lo : (s[2] > hi ? hi : s[2]);
}

static void af_return(struct autofocus *a, bool converged)
{
	int position = converged ? a->best_position :
		       (a->good ? a->good_position : REAR_FOCUS_DEFAULT);

	a->converged = converged;
	a->phase = AF_RETURN;
	/* Still confirm with new frames, but no move means no new lens settling. */
	if (position != a->position)
		af_move(a, position);
}

/* Refine a measured coarse peak without sampling its known center again.
 * Each pair halves the spacing: 12, 6, 3 actuator units. Keep the pair's
 * center fixed even when its first measurement becomes the new best. */
static bool af_refine(struct autofocus *a)
{
	while (a->fine_step >= AF_FINE_STEP) {
		if (a->fine_sample == 2) {
			a->fine_step /= 2;
			a->fine_sample = 0;
			continue;
		}
		if (!a->fine_sample)
			a->fine_center = a->best_position;
		a->next = a->fine_center +
			  (a->fine_sample++ ? a->fine_step : -a->fine_step);
		if (a->next >= REAR_FOCUS_MIN && a->next <= REAR_FOCUS_MAX)
			return true;
	}
	return false;
}

static void af_frame(struct data *d, const uint8_t *p, uint64_t frame_time)
{
	struct autofocus *a = &d->af;
	struct af_measure m;
	uint64_t now;
	double score;
	unsigned change, motion;

	if (a->fd < 0)
		return;
	now = af_now();
	if (a->settle_frames) {
		a->settle_frames--;
		return;
	}
	/* AtomISP timestamps completion, not exposure start. Keep the queued-frame
	 * flush as well: a newly dequeued buffer need not be a newly exposed one. */
	if (now < a->settle_until || (frame_time && frame_time < a->settle_until))
		return;
	m = af_measure(p, d->v4l2_w, d->v4l2_h, d->v4l2_stride);
	change = af_scene_change(a->scene, m.scene);
	motion = a->observed ? af_scene_change(a->previous_scene, m.scene) : 0;
	memcpy(a->previous_scene, m.scene, sizeof(a->previous_scene));
	a->observed = true;
	if (a->phase == AF_LOCKED) {
		bool changed = change >= 24 ||
			       (!a->locked_usable && m.usable) ||
			       (a->locked_usable && m.score < a->locked_score * .65);
		/* An evidently new target need not wait five seconds after a good
		 * lock. Keep the cooldown for score-only drift and failed searches,
		 * and still require the normal eight-frame scene-change vote. */
		bool cooldown = now - a->locked_at < AF_REACQUIRE_NS;
		if (!m.usable || motion >= 14 || (cooldown &&
		    (change < 24 || !a->converged || !a->locked_usable)))
			a->votes = 0;
		else if (changed)
			a->votes++;
		else
			a->votes = 0;
		if (a->votes < 8)
			return;
		a->phase = AF_WAIT;
		a->samples = a->invalid = a->votes = 0;
	}
	/* A new subject must stop moving before its contrast can be compared at
	 * different lens positions. Do not start a scan during a handheld pan. */
	if (a->phase == AF_WAIT && motion >= 14) {
		a->samples = 0;
		return;
	}
	if (a->phase == AF_COARSE || a->phase == AF_FINE ||
	    (a->phase == AF_RETURN && a->converged)) {
		a->changed = change > 32 || motion >= 14 ? a->changed + 1 : 0;
		if (a->changed >= 3) {
			msg("rear AF inconclusive: scene moved during search; restoring focus");
			af_return(a, false);
			return;
		}
	}
	if (!m.usable) {
		a->samples = 0;
		if (a->phase == AF_WAIT)
			return;		/* Never walk the lens on a flat/dark scene. */
		if (++a->invalid < 6)
			return;
		if (a->phase != AF_RETURN || a->converged) {
			msg("rear AF inconclusive: insufficient stable texture; restoring focus");
			af_return(a, false);
			return;
		}
		/* Even a scene which went dark must finish the return and lock. */
	} else {
		a->scores[a->samples++] = m.score;
		if (a->samples < AF_SAMPLES)
			return;
		a->invalid = 0;
		double lo = a->scores[0], hi = lo;
		for (unsigned i = 1; i < AF_SAMPLES; i++) {
			if (a->scores[i] < lo)
				lo = a->scores[i];
			if (a->scores[i] > hi)
				hi = a->scores[i];
		}
		if (hi > lo * 1.35) {
			a->samples = 0;
			if (a->phase == AF_WAIT)
				return;
			if (a->phase != AF_RETURN || a->converged) {
				msg("rear AF inconclusive: unstable contrast; restoring focus");
				af_return(a, false);
				return;
			}
			m.usable = false;
		}
	}
	score = m.usable ? af_median(a->scores) : 0;
	a->samples = 0;
	if (a->phase == AF_RETURN) {
		/* Use the same contrast-variation limit as the sample stability check. */
		if (a->converged && m.usable && a->best_score > score * 1.35) {
			msg("rear AF inconclusive: peak not repeatable; restoring focus");
			af_return(a, false);
			return;
		}
		a->phase = AF_LOCKED;
		a->locked_at = now;
		a->locked_score = score;
		a->locked_usable = m.usable;
		a->votes = 0;
		memcpy(a->scene, m.scene, sizeof(a->scene));
		if (a->converged && m.usable) {
			a->good = true;
			a->good_position = a->position;
			msg("rear AF converged: position %d, normalized score %.7f",
			    a->position, score);
		} else {
			msg("rear AF locked at prior position %d, normalized score %.7f (no reliable improvement)",
			    a->position, score);
		}
		return;
	}
	if (a->phase == AF_WAIT) {
		a->baseline_score = a->best_score = a->low_score = score;
		a->best_position = a->position;
		a->baseline_position = a->position;
		memcpy(a->scene, m.scene, sizeof(a->scene));
		a->phase = AF_COARSE;
		a->next = REAR_FOCUS_MIN;
		a->end = REAR_FOCUS_MAX;
	} else {
		if (score > a->best_score) {
			a->best_score = score;
			a->best_position = a->position;
		}
		if (score < a->low_score)
			a->low_score = score;
		if (a->phase == AF_COARSE) {
			/* Different subjects can produce separate optical peaks.
			 * Cover the complete calibrated span before choosing which
			 * coarse maximum to refine; the first peak need not be best. */
			if (a->position == a->end) {
				a->phase = AF_FINE;
				a->fine_step = AF_COARSE_STEP / 2;
				a->fine_sample = 0;
			} else {
				a->next = a->position + AF_COARSE_STEP;
				if (a->next > a->end)
					a->next = a->end;
			}
		}
		if (a->phase == AF_FINE && !af_refine(a)) {
			/* Don't chase a gain smaller than frame-to-frame variation. */
			bool peak = a->low_score < a->best_score * .9;
			if (a->best_score < a->baseline_score * 1.03)
				a->best_position = a->baseline_position;
			af_return(a, peak);
			return;
		}
	}
	af_move(a, a->next);
}

/* ---- geometry --------------------------------------------------------- */

/* Unknown transform: assume the natural (laptop) orientation. */
static int desired_rot(struct data *d, struct cam *c)
{
	int rot = c->rot[d->transform < 0 ? 0 : d->transform & 3];

	return d->folded ? rot : (rot + c->unfolded_rot) % 360;
}

static bool hinge_dependent(struct cam *c)
{
	return c->unfolded_rot || c->unfolded_mirror;
}

/* Reverse each row of a w x h plane in place. */
static void mirror_plane(uint8_t *p, uint32_t w, uint32_t h, uint32_t stride)
{
	for (uint32_t y = 0; y < h; y++) {
		uint8_t *l = p + (size_t)y * stride, *r = l + w - 1;

		while (l < r) {
			uint8_t t = *l;

			*l++ = *r;
			*r-- = t;
		}
	}
}

static void rot_dims(struct cam *c, int rot, uint32_t *w, uint32_t *h)
{
	*w = rot % 180 ? c->h : c->w;
	*h = rot % 180 ? c->w : c->h;
}

static void i420_layout(uint32_t w, uint32_t h, uint32_t *ys, uint32_t *cs,
			uint32_t *size)
{
	*ys = (w + 3) & ~3u;
	*cs = ((w + 1) / 2 + 3) & ~3u;
	*size = *ys * h + 2 * *cs * ((h + 1) / 2);
}

static inline uint32_t min_u32(uint32_t a, uint32_t b)
{
	return a < b ? a : b;
}

/* Rotate one w x h plane clockwise by rot degrees. */
static void rotate_plane(const uint8_t *s, uint32_t w, uint32_t h, uint32_t ss,
			 uint8_t *dd, uint32_t ds, int rot)
{
	uint32_t x, y, x0, y0;

	switch (rot) {
	case 0:
		for (y = 0; y < h; y++)
			memcpy(dd + y * ds, s + y * ss, w);
		break;
	case 180:
		for (y = 0; y < h; y++) {
			const uint8_t *sr = s + y * ss;
			uint8_t *dr = dd + (h - 1 - y) * ds + (w - 1);

			for (x = 0; x < w; x++)
				dr[-(long)x] = sr[x];
		}
		break;
	case 90:	/* dst is h wide: dst(x, y) = src(y, h - 1 - x) */
		for (y0 = 0; y0 < w; y0 += TILE)
			for (x0 = 0; x0 < h; x0 += TILE)
				for (y = y0; y < min_u32(y0 + TILE, w); y++) {
					uint8_t *dr = dd + y * ds;

					for (x = x0; x < min_u32(x0 + TILE, h); x++)
						dr[x] = s[(h - 1 - x) * ss + y];
				}
		break;
	case 270:	/* dst(x, y) = src(w - 1 - y, x) */
		for (y0 = 0; y0 < w; y0 += TILE)
			for (x0 = 0; x0 < h; x0 += TILE)
				for (y = y0; y < min_u32(y0 + TILE, w); y++) {
					uint8_t *dr = dd + y * ds;

					for (x = x0; x < min_u32(x0 + TILE, h); x++)
						dr[x] = s[x * ss + (w - 1 - y)];
				}
		break;
	}
}

/* Rotate one sw x sh plane clockwise by rot and fit it, aspect ratio kept,
 * centred in a dw x dh plane filled with fill. Nearest neighbour: this runs
 * only after a 90-degree screen turn while an app streams, until it reopens
 * and negotiates the upright size. xmap holds at least dw entries. */
static void fit_plane(const uint8_t *s, uint32_t sw, uint32_t sh, uint32_t ss,
		      uint8_t *dd, uint32_t dw, uint32_t dh, uint32_t ds,
		      int rot, uint8_t fill, uint32_t *xmap)
{
	uint32_t rw = rot % 180 ? sh : sw, rh = rot % 180 ? sw : sh;
	uint32_t ow, oh, ox, oy, x, y;

	if ((uint64_t)dw * rh <= (uint64_t)dh * rw) {
		ow = dw;
		oh = (uint32_t)((uint64_t)rh * dw / rw);
	} else {
		oh = dh;
		ow = (uint32_t)((uint64_t)rw * dh / rh);
	}
	ox = (dw - ow) / 2;
	oy = (dh - oh) / 2;
	for (y = 0; y < dh; y++)
		memset(dd + y * ds, fill, dw);
	for (x = 0; x < ow; x++)
		xmap[x] = (uint32_t)((uint64_t)x * rw / ow);
	for (y = 0; y < oh; y++) {
		uint32_t ry = (uint32_t)((uint64_t)y * rh / oh);
		uint8_t *dr = dd + (oy + y) * ds + ox;

		for (x = 0; x < ow; x++) {
			uint32_t rx = xmap[x], sx, sy;

			switch (rot) {
			case 90:  sx = ry;          sy = sh - 1 - rx; break;
			case 180: sx = sw - 1 - rx; sy = sh - 1 - ry; break;
			case 270: sx = sw - 1 - ry; sy = rx;          break;
			default:  sx = rx;          sy = ry;          break;
			}
			dr[x] = s[sy * ss + sx];
		}
	}
}

static void fill_rect(uint8_t *p, uint32_t stride, uint32_t x0, uint32_t y0,
		      uint32_t x1, uint32_t y1, uint8_t v)
{
	for (uint32_t y = y0; y < y1; y++)
		memset(p + (size_t)y * stride + x0, v, x1 - x0);
}

/* A white "F" on dark grey, in the sensor's YU12 layout. It looks different
 * after every turn and mirror, so --test-pattern lets a test read the image's
 * orientation off the screen without any scene in front of the camera. */
static uint8_t *make_pattern(uint32_t w, uint32_t h, uint32_t stride)
{
	size_t ysize = (size_t)stride * h, csize = (size_t)(stride / 2) * (h / 2);
	uint8_t *p = malloc(ysize + 2 * csize);
	uint32_t s = h / 8, x0 = w / 3, y0 = h / 8;

	if (!p)
		return NULL;
	memset(p, 40, ysize);
	memset(p + ysize, 128, 2 * csize);
	fill_rect(p, stride, x0, y0, x0 + s, h - y0, 235);		/* stem */
	fill_rect(p, stride, x0, y0, x0 + w / 3, y0 + s, 235);		/* top bar */
	fill_rect(p, stride, x0, h / 2 - s / 2, x0 + w / 5, h / 2 + s / 2, 235);
	return p;
}

/* ---- PipeWire params -------------------------------------------------- */

static const struct spa_pod *build_format(struct spa_pod_builder *b,
					  uint32_t w, uint32_t h)
{
	return spa_format_video_raw_build(b, SPA_PARAM_EnumFormat,
		&SPA_VIDEO_INFO_RAW_INIT(.format = SPA_VIDEO_FORMAT_I420,
					 .size = SPA_RECTANGLE(w, h),
					 .framerate = SPA_FRACTION(FPS, 1),
					 .color_range = SPA_VIDEO_COLOR_RANGE_0_255,
					 .color_matrix = SPA_VIDEO_COLOR_MATRIX_BT601,
					 .transfer_function = SPA_VIDEO_TRANSFER_BT709,
					 .color_primaries = SPA_VIDEO_COLOR_PRIMARIES_BT709));
}

/* ---- V4L2 ------------------------------------------------------------- */

static void requeue(struct data *d, int index)
{
	struct v4l2_buffer vb = {
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
		.memory = V4L2_MEMORY_MMAP,
		.index = index,
	};

	if (ioctl(d->fd, VIDIOC_QBUF, &vb) < 0)
		msg("QBUF %d: %m", index);
}

/* ---- hinge ------------------------------------------------------------ */

/* Degrees between the halves: near 0 (or 360) folded flat or closed. Folded
 * flat reads up to ~24 and the reading jumps while the hinge moves, so a
 * change of state needs HINGE_SETTLE polls in a row past the far threshold. */
#define FOLDED_MAX	15	/* <= 15 or >= 345: folded */
#define UNFOLDED_MIN	40	/* 40..300: unfolded */
#define UNFOLDED_MAX	300
#define HINGE_POLL_MS	500
#define HINGE_SETTLE	4

static const char *find_hinge(void)
{
	static char path[256];
	const char *found = NULL;
	glob_t g;

	if (glob("/sys/bus/iio/devices/iio:device*/in_angl0_label", 0, NULL, &g))
		return NULL;
	for (size_t i = 0; i < g.gl_pathc && !found; i++) {
		char label[16] = "";
		FILE *f = fopen(g.gl_pathv[i], "re");

		if (!f)
			continue;
		if (fgets(label, sizeof(label), f) && !strcmp(label, "hinge\n")) {
			int n = (int)(strlen(g.gl_pathv[i]) - strlen("_label"));

			snprintf(path, sizeof(path), "%.*s_raw", n, g.gl_pathv[i]);
			found = path;
		}
		fclose(f);
	}
	globfree(&g);
	return found;
}

static int read_hinge(struct data *d)
{
	FILE *f = fopen(d->hinge_path, "re");
	int a = -1;

	if (!f)
		return -1;
	if (fscanf(f, "%d", &a) == 1)
		a = (a % 360 + 360) % 360;
	else
		a = -1;
	fclose(f);
	return a;
}

static void on_hinge(void *data, uint64_t expirations)
{
	struct data *d = data;
	int a = read_hinge(d);
	bool flip;

	if (a < 0)
		return;
	if (d->folded)
		flip = a >= UNFOLDED_MIN && a <= UNFOLDED_MAX;
	else
		flip = a <= FOLDED_MAX || a >= 360 - FOLDED_MAX;
	if (!flip) {
		d->fold_votes = 0;
		return;
	}
	if (++d->fold_votes < HINGE_SETTLE)
		return;
	d->fold_votes = 0;
	d->folded = !d->folded;
	msg("hinge %d degrees: %s", a, d->folded ? "folded" : "unfolded");
}

/* Read the pose now and keep polling while camera c streams, if its image
 * depends on the pose. Below 40 degrees (folded flat can read 24) or above
 * 300 the machine counts as folded. */
static void hinge_start(struct data *d, struct cam *c)
{
	struct timespec poll = { 0, HINGE_POLL_MS * 1000 * 1000 };
	int a;

	if (!hinge_dependent(c) || !d->hinge_path || (a = read_hinge(d)) < 0)
		return;
	d->folded = a < UNFOLDED_MIN || a > UNFOLDED_MAX;
	d->fold_votes = 0;
	msg("hinge %d degrees: %s", a, d->folded ? "folded" : "unfolded");
	pw_loop_update_timer(d->loop, d->hinge_timer, &poll, &poll, false);
}

static void hinge_stop(struct data *d)
{
	pw_loop_update_timer(d->loop, d->hinge_timer, NULL, NULL, false);
}

static void camera_stop(struct data *d)
{
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	struct v4l2_requestbuffers rb = {
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
		.memory = V4L2_MEMORY_MMAP,
	};

	af_stop(&d->af);
	if (d->fd < 0)
		return;
	if (d->v4l2_src)
		pw_loop_destroy_source(d->loop, d->v4l2_src);
	d->v4l2_src = NULL;
	ioctl(d->fd, VIDIOC_STREAMOFF, &type);
	for (int i = 0; i < NBUF; i++)
		if (d->buf[i])
			munmap(d->buf[i], d->buf_len[i]);
	memset(d->buf, 0, sizeof(d->buf));
	ioctl(d->fd, VIDIOC_REQBUFS, &rb);
	close(d->fd);
	d->fd = -1;
	d->pending = -1;
	free(d->pattern);
	d->pattern = NULL;
	msg("%s camera stopped: %u frames in, %u process calls (%u inactive, %u no frame), %u out, %u without a buffer",
	    d->active ? d->active->key : "?", d->n_in, d->n_process,
	    d->n_inactive, d->n_nopending, d->n_out, d->n_nobuf);
	d->n_in = d->n_process = d->n_out = d->n_nobuf = 0;
	d->n_inactive = d->n_nopending = 0;
	d->active = NULL;
	hinge_stop(d);
}

static void on_v4l2(void *data, int fd, uint32_t mask)
{
	struct data *d = data;
	struct v4l2_buffer vb = {
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
		.memory = V4L2_MEMORY_MMAP,
	};
	bool new_frame = false, valid_frame = false;
	uint64_t frame_time = 0;

	while (ioctl(fd, VIDIOC_DQBUF, &vb) == 0) {
		if (d->pending >= 0)
			requeue(d, d->pending);	/* drop the older frame */
		d->pending = vb.index;
		d->n_in++;
		new_frame = true;
		valid_frame = !(vb.flags & V4L2_BUF_FLAG_ERROR);
		frame_time = (vb.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) ==
			     V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC ?
			     (uint64_t)vb.timestamp.tv_sec * SPA_NSEC_PER_SEC +
			     (uint64_t)vb.timestamp.tv_usec * 1000 : 0;
	}
	if (new_frame && valid_frame && d->pending >= 0)
		af_frame(d, d->buf[d->pending], frame_time);
	if (d->pending >= 0 && d->active)
		pw_stream_trigger_process(d->active->stream);
}

static int camera_start(struct data *d, struct cam *c)
{
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	struct v4l2_requestbuffers rb = {
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
		.memory = V4L2_MEMORY_MMAP,
		.count = NBUF,
	};
	struct v4l2_format f = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
	int input = c->input;

	d->fd = open(d->dev_path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (d->fd < 0) {
		msg("open %s: %m", d->dev_path);
		return -errno;
	}
	d->active = c;
	if (ioctl(d->fd, VIDIOC_S_INPUT, &input) < 0) {
		msg("S_INPUT %d: %m", input);
		goto fail;
	}
	f.fmt.pix.width = c->w;
	f.fmt.pix.height = c->h;
	f.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;
	f.fmt.pix.field = V4L2_FIELD_NONE;
	if (ioctl(d->fd, VIDIOC_S_FMT, &f) < 0) {
		msg("S_FMT: %m");
		goto fail;
	}
	if (f.fmt.pix.width != c->w || f.fmt.pix.height != c->h ||
	    f.fmt.pix.pixelformat != V4L2_PIX_FMT_YUV420) {
		msg("%s camera: driver gave %ux%u, expected %ux%u YU12", c->key,
		     f.fmt.pix.width, f.fmt.pix.height, c->w, c->h);
		goto fail;
	}
	d->v4l2_w = f.fmt.pix.width;
	d->v4l2_h = f.fmt.pix.height;
	d->v4l2_stride = f.fmt.pix.bytesperline;
	if (d->test_pattern &&
	    !(d->pattern = make_pattern(d->v4l2_w, d->v4l2_h, d->v4l2_stride)))
		goto fail;

	if (ioctl(d->fd, VIDIOC_REQBUFS, &rb) < 0 || rb.count < 2) {
		msg("REQBUFS: %m");
		goto fail;
	}
	for (uint32_t i = 0; i < rb.count && i < NBUF; i++) {
		struct v4l2_buffer vb = {
			.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
			.memory = V4L2_MEMORY_MMAP,
			.index = i,
		};

		if (ioctl(d->fd, VIDIOC_QUERYBUF, &vb) < 0) {
			msg("QUERYBUF: %m");
			goto fail;
		}
		d->buf[i] = mmap(NULL, vb.length, PROT_READ, MAP_SHARED, d->fd,
				 vb.m.offset);
		if (d->buf[i] == MAP_FAILED) {
			d->buf[i] = NULL;
			msg("mmap: %m");
			goto fail;
		}
		d->buf_len[i] = vb.length;
		if (ioctl(d->fd, VIDIOC_QBUF, &vb) < 0) {
			msg("QBUF: %m");
			goto fail;
		}
	}
	if (ioctl(d->fd, VIDIOC_STREAMON, &type) < 0) {
		msg("STREAMON: %m");
		goto fail;
	}
	d->v4l2_src = pw_loop_add_io(d->loop, d->fd, SPA_IO_IN, false, on_v4l2, d);
	msg("%s camera streaming %ux%u (stride %u)", c->key, d->v4l2_w,
	     d->v4l2_h, d->v4l2_stride);
	hinge_start(d, c);
	af_start(d);
	return 0;
fail:
	camera_stop(d);
	return -EIO;
}

/* Find each sensor's input and native YU12 size. */
static void probe_sensors(struct data *d)
{
	int fd = open(d->dev_path, O_RDWR | O_CLOEXEC);

	if (fd < 0) {
		msg("probe: open %s: %m (using defaults)", d->dev_path);
		return;
	}
	for (uint32_t i = 0; i < 8; i++) {
		struct v4l2_input in = { .index = i };
		struct v4l2_frmsizeenum fs = { .pixel_format = V4L2_PIX_FMT_YUV420 };
		int idx = i;

		if (ioctl(fd, VIDIOC_ENUMINPUT, &in) < 0)
			break;
		for (int k = 0; k < N_CAMS; k++) {
			struct cam *c = &d->cams[k];

			if (strncmp((char *)in.name, c->sensor, strlen(c->sensor)))
				continue;
			c->input = idx;
			if (ioctl(fd, VIDIOC_S_INPUT, &idx) == 0 &&
			    ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &fs) == 0 &&
			    fs.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
				c->w = fs.discrete.width;
				c->h = fs.discrete.height;
			}
			msg("%s camera: input %d (%s), %ux%u", c->key, idx,
			     (char *)in.name, c->w, c->h);
		}
	}
	close(fd);
}

/* ---- streams ---------------------------------------------------------- */

/* Start the first camera an app is waiting for, if the device is free. When
 * opening fails (STREAMON returned EINVAL on the first stream right after the
 * daemon probed the sensors), try again every second while an app waits:
 * its stream stays connected, and frames start once the device opens. */
static void start_waiting(struct data *d)
{
	struct timespec second = { 1, 0 }, none = { 0, 0 };
	bool waiting = false;

	for (int k = 0; k < N_CAMS && !d->active; k++) {
		if (!d->cams[k].want)
			continue;
		waiting = true;
		camera_start(d, &d->cams[k]);
	}
	if (waiting && !d->active)
		pw_loop_update_timer(d->loop, d->start_timer, &second, &none, false);
}

static void on_start_retry(void *data, uint64_t expirations)
{
	start_waiting(data);
}

/* sync_nodes runs from a timer: it may destroy the stream whose callback
 * asked for it. */
static void schedule_sync(struct data *d)
{
	struct timespec soon = { 0, 50 * 1000 * 1000 }, none = { 0, 0 };

	pw_loop_update_timer(d->loop, d->sync_timer, &soon, &none, false);
}

static void on_state_changed(void *data, enum pw_stream_state old,
			     enum pw_stream_state state, const char *error)
{
	struct cam *c = data;
	struct data *d = c->d;
	bool want = state == PW_STREAM_STATE_STREAMING;

	if (state == PW_STREAM_STATE_ERROR)
		msg("%s stream error: %s", c->key, error ? error : "?");
	if (want == c->want)
		return;
	c->want = want;
	if (want) {
		if (!d->active)
			start_waiting(d);
		else if (d->active != c)
			msg("%s camera wanted while the %s camera streams; waiting",
			     c->key, d->active->key);
	} else {
		if (d->active == c) {
			camera_stop(d);
			start_waiting(d);
		}
		schedule_sync(d);
	}
}

static void on_param_changed(void *data, uint32_t id, const struct spa_pod *param)
{
	struct cam *c = data;
	uint8_t buf[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
	struct spa_video_info_raw info;
	uint32_t mtype, msubtype, ys, cs, size;
	const struct spa_pod *p[2];

	if (id != SPA_PARAM_Format)
		return;
	if (!param) {
		c->neg_w = c->neg_h = 0;
		return;
	}
	if (spa_format_parse(param, &mtype, &msubtype) < 0 ||
	    mtype != SPA_MEDIA_TYPE_video || msubtype != SPA_MEDIA_SUBTYPE_raw ||
	    spa_format_video_raw_parse(param, &info) < 0)
		return;
	c->neg_w = info.size.width;
	c->neg_h = info.size.height;
	msg("%s camera negotiated %ux%u", c->key, c->neg_w, c->neg_h);

	i420_layout(c->neg_w, c->neg_h, &ys, &cs, &size);
	p[0] = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
		SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(NBUF, 2, 8),
		SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
		SPA_PARAM_BUFFERS_size, SPA_POD_Int(size),
		SPA_PARAM_BUFFERS_stride, SPA_POD_Int(ys),
		SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(
			(1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr)));
	/* pts and sequence number: see on_process */
	p[1] = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
		SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
		SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header)));
	pw_stream_update_params(c->stream, p, 2);
}

static void on_process(void *data)
{
	struct cam *c = data;
	struct data *d = c->d;
	struct pw_buffer *pb;
	struct spa_data *sd;
	struct spa_meta_header *hdr;
	const uint8_t *sy, *su, *sv;
	uint8_t *dy, *du, *dv;
	uint32_t ys, cs, size, ss = d->v4l2_stride, rw, rh;
	uint32_t sw = d->v4l2_w, sh = d->v4l2_h;
	int rot;

	d->n_process++;
	if (d->active != c) {
		d->n_inactive++;
		return;
	}
	if (d->pending < 0) {
		d->n_nopending++;
		return;
	}
	pb = pw_stream_dequeue_buffer(c->stream);
	if (!pb || !c->neg_w || !c->neg_h) {
		d->n_nobuf++;
		if (pb)
			pw_stream_queue_buffer(c->stream, pb);
		goto out;
	}
	sd = &pb->buffer->datas[0];
	i420_layout(c->neg_w, c->neg_h, &ys, &cs, &size);
	if (!sd->data || sd->maxsize < size) {
		if (!d->n_nobuf++)
			msg("%s camera: unusable buffer (data %p, type %u, maxsize %u, need %u)",
			    c->key, sd->data, sd->type, sd->maxsize, size);
		pw_stream_queue_buffer(c->stream, pb);
		goto out;
	}

	sy = d->pattern ? d->pattern : d->buf[d->pending];
	su = sy + ss * sh;
	sv = su + (ss / 2) * (sh / 2);
	dy = sd->data;
	du = dy + ys * c->neg_h;
	dv = du + cs * ((c->neg_h + 1) / 2);
	rot = desired_rot(d, c);
	rot_dims(c, rot, &rw, &rh);
	if (rw == c->neg_w && rh == c->neg_h) {
		rotate_plane(sy, sw, sh, ss, dy, ys, rot);
		rotate_plane(su, sw / 2, sh / 2, ss / 2, du, cs, rot);
		rotate_plane(sv, sw / 2, sh / 2, ss / 2, dv, cs, rot);
	} else {
		/* The screen turned 90 degrees since the app negotiated: keep
		 * its size, keep the image upright, letterbox it. */
		fit_plane(sy, sw, sh, ss, dy, c->neg_w, c->neg_h, ys, rot, 16,
			  d->xmap);
		fit_plane(su, sw / 2, sh / 2, ss / 2, du, (c->neg_w + 1) / 2,
			  (c->neg_h + 1) / 2, cs, rot, 128, d->xmap);
		fit_plane(sv, sw / 2, sh / 2, ss / 2, dv, (c->neg_w + 1) / 2,
			  (c->neg_h + 1) / 2, cs, rot, 128, d->xmap);
	}
	if (!d->folded && c->unfolded_mirror) {
		mirror_plane(dy, c->neg_w, c->neg_h, ys);
		mirror_plane(du, (c->neg_w + 1) / 2, (c->neg_h + 1) / 2, cs);
		mirror_plane(dv, (c->neg_w + 1) / 2, (c->neg_h + 1) / 2, cs);
	}

	/* This stream drives the graph and never advances its clock: without
	 * a pts every frame reaches pipewiresrc with the same timestamp. */
	hdr = spa_buffer_find_meta_data(pb->buffer, SPA_META_Header, sizeof(*hdr));
	if (hdr) {
		hdr->flags = 0;
		hdr->offset = 0;
		hdr->pts = pw_stream_get_nsec(c->stream);
		hdr->dts_offset = 0;
		hdr->seq = c->seq++;
	}
	sd->chunk->offset = 0;
	sd->chunk->size = size;
	sd->chunk->stride = ys;
	sd->chunk->flags = 0;
	pw_stream_queue_buffer(c->stream, pb);
	d->n_out++;
out:
	requeue(d, d->pending);
	d->pending = -1;
}

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = on_state_changed,
	.param_changed = on_param_changed,
	.process = on_process,
};

/* A node offers exactly one size, the upright one at its creation, and never
 * changes it: an app's PipeWire device provider reads EnumFormat once per node
 * and keeps those caps for the node's lifetime, so a changed offer could never
 * be negotiated by that app again. A turned screen gets a new node instead. */
static int create_stream(struct data *d, struct cam *c)
{
	uint8_t buf[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
	const struct spa_pod *param;
	char name[64];

	snprintf(name, sizeof(name), "yb-camera-%s", c->key);
	c->stream = pw_stream_new(d->core, name, pw_properties_new(
		PW_KEY_MEDIA_CLASS, "Video/Source",
		PW_KEY_MEDIA_ROLE, "Camera",
		PW_KEY_NODE_NAME, name,
		PW_KEY_NODE_DESCRIPTION, c->description,
		PW_KEY_NODE_NICK, c->description,
		"api.libcamera.location", c->location,
		NULL));
	if (!c->stream)
		return -errno;
	pw_stream_add_listener(c->stream, &c->listener, &stream_events, c);

	rot_dims(c, desired_rot(d, c), &c->fmt_w, &c->fmt_h);
	param = build_format(&b, c->fmt_w, c->fmt_h);
	msg("%s camera: node offers %ux%u", c->key, c->fmt_w, c->fmt_h);
	return pw_stream_connect(c->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
				 PW_STREAM_FLAG_DRIVER | PW_STREAM_FLAG_MAP_BUFFERS,
				 &param, 1);
}

/* Withdraw a camera's node. An app streaming from it loses the camera. */
static void destroy_stream(struct data *d, struct cam *c)
{
	if (!c->stream)
		return;
	spa_hook_remove(&c->listener);
	c->want = false;
	if (d->active == c) {
		camera_stop(d);
		start_waiting(d);
	}
	pw_stream_destroy(c->stream);
	c->stream = NULL;
	c->neg_w = c->neg_h = 0;
	c->fmt_w = c->fmt_h = 0;
}

/* Publish every camera, each offering its upright size. A node whose size
 * went stale is replaced only while nobody streams from it; until then
 * on_process fits the turned image into the negotiated size. */
static void sync_nodes(struct data *d)
{
	for (int k = 0; k < N_CAMS; k++) {
		struct cam *c = &d->cams[k];
		uint32_t w, h;

		if (c->input < 0)
			continue;
		rot_dims(c, desired_rot(d, c), &w, &h);
		if (c->stream && !c->want && (w != c->fmt_w || h != c->fmt_h)) {
			msg("%s camera: screen turned, replacing the %ux%u node",
			    c->key, c->fmt_w, c->fmt_h);
			destroy_stream(d, c);
		}
		if (!c->stream && create_stream(d, c) < 0)
			msg("%s camera: cannot create stream", c->key);
	}
}

static void on_sync(void *data, uint64_t expirations)
{
	sync_nodes(data);
}

/* ---- screen rotation (mutter) ----------------------------------------- */

static int query_transform(struct data *d)
{
	sd_bus_error err = SD_BUS_ERROR_NULL;
	sd_bus_message *m = NULL;
	uint32_t serial;
	int r, found = -1;

	r = sd_bus_call_method(d->bus, "org.gnome.Mutter.DisplayConfig",
			       "/org/gnome/Mutter/DisplayConfig",
			       "org.gnome.Mutter.DisplayConfig", "GetCurrentState",
			       &err, &m, "");
	sd_bus_error_free(&err);
	if (r < 0)
		return r;
	if ((r = sd_bus_message_read(m, "u", &serial)) < 0 ||
	    (r = sd_bus_message_skip(m, "a((ssss)a(siiddada{sv})a{sv})")) < 0 ||
	    (r = sd_bus_message_enter_container(m, 'a', "(iiduba(ssss)a{sv})")) < 0)
		goto out;
	while ((r = sd_bus_message_enter_container(m, 'r', "iiduba(ssss)a{sv}")) > 0) {
		int32_t x, y;
		double scale;
		uint32_t transform;
		int primary;
		const char *conn, *vendor, *product, *ser;

		if ((r = sd_bus_message_read(m, "iidub", &x, &y, &scale, &transform,
					     &primary)) < 0 ||
		    (r = sd_bus_message_enter_container(m, 'a', "(ssss)")) < 0)
			goto out;
		while ((r = sd_bus_message_read(m, "(ssss)", &conn, &vendor,
						&product, &ser)) > 0)
			if (!strncmp(conn, "DSI", 3) || !strncmp(conn, "eDP", 3) ||
			    !strncmp(conn, "LVDS", 4))
				found = transform;
		if (r < 0 ||
		    (r = sd_bus_message_exit_container(m)) < 0 ||
		    (r = sd_bus_message_skip(m, "a{sv}")) < 0 ||
		    (r = sd_bus_message_exit_container(m)) < 0)
			goto out;
	}
	r = found;
out:
	sd_bus_message_unref(m);
	return r;
}

static void apply_transform(struct data *d, int transform)
{
	if (transform == d->transform)
		return;
	msg("screen transform %d -> %d", d->transform, transform);
	d->transform = transform;
	/* A streaming node keeps its size (on_process rotates, and after a
	 * 90-degree turn fits, each frame); an idle one is replaced. */
	schedule_sync(d);
}

static void refresh_transform(struct data *d)
{
	struct timespec value = { 2, 0 }, interval = { 2, 0 };
	int t = query_transform(d);

	if (t >= 0) {
		apply_transform(d, t);
		pw_loop_update_timer(d->loop, d->retry, NULL, NULL, false);
	} else {
		/* mutter not up yet (or restarting): try again */
		pw_loop_update_timer(d->loop, d->retry, &value, &interval, false);
	}
}

static int on_monitors_changed(sd_bus_message *m, void *data, sd_bus_error *e)
{
	refresh_transform(data);
	return 0;
}

static void drain_bus(struct data *d)
{
	while (sd_bus_process(d->bus, NULL) > 0)
		;
}

static void on_bus(void *data, int fd, uint32_t mask)
{
	drain_bus(data);
}

static void on_retry(void *data, uint64_t expirations)
{
	struct data *d = data;

	refresh_transform(d);
	drain_bus(d);
}

static int setup_bus(struct data *d)
{
	int r = sd_bus_open_user(&d->bus);

	if (r < 0)
		return r;
	r = sd_bus_match_signal(d->bus, NULL, NULL, "/org/gnome/Mutter/DisplayConfig",
				"org.gnome.Mutter.DisplayConfig", "MonitorsChanged",
				on_monitors_changed, d);
	if (r < 0)
		return r;
	d->bus_src = pw_loop_add_io(d->loop, sd_bus_get_fd(d->bus), SPA_IO_IN,
				    false, on_bus, d);
	d->retry = pw_loop_add_timer(d->loop, on_retry, d);
	refresh_transform(d);
	drain_bus(d);
	return 0;
}

/* ---- main ------------------------------------------------------------- */

static void on_quit(void *data, int signal)
{
	struct data *d = data;

	pw_main_loop_quit(d->main_loop);
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"usage: %s [--device PATH] [--test-pattern]\n"
		"          [--front-rot R0,R1,R2,R3] [--rear-rot R0,R1,R2,R3]\n"
		"          [--rear-unfolded 0|180] [--rear-unfolded-mirror] [--hinge PATH]\n"
		"          [--default-camera]\n"
		"Rn: clockwise turn in degrees (a multiple of 90) for screen transform n.\n"
		"--rear-unfolded: further turn of the rear camera while not folded flat.\n"
		"--rear-unfolded-mirror: mirror the rear camera while not folded flat.\n"
		"--hinge: read the hinge angle from PATH instead of the IIO sensor (tests).\n"
		"--default-camera: print the preferred camera for the current hinge, then exit.\n"
		"--test-pattern: send an \"F\" instead of the camera image (orientation tests).\n",
		prog);
}

static int parse_rot(const char *s, int rot[4])
{
	char *end;

	for (int t = 0; t < 4; t++) {
		long v = strtol(s, &end, 10);

		if (end == s || v % 90 || *end != (t < 3 ? ',' : '\0'))
			return -1;
		rot[t] = (int)((v % 360 + 360) % 360);
		s = end + 1;
	}
	return 0;
}

int main(int argc, char *argv[])
{
	static const struct option opts[] = {
		{ "device", required_argument, NULL, 'd' },
		{ "front-rot", required_argument, NULL, 'f' },
		{ "rear-rot", required_argument, NULL, 'r' },
		{ "rear-unfolded", required_argument, NULL, 'u' },
		{ "rear-unfolded-mirror", no_argument, NULL, 'M' },
		{ "hinge", required_argument, NULL, 'H' },
		{ "default-camera", no_argument, NULL, 'D' },
		{ "test-pattern", no_argument, NULL, 'T' },
		{ "help", no_argument, NULL, 'h' },
		{ }
	};
	struct data d = {
		.dev_path = "/dev/video0",
		.fd = -1,
		.af = { .fd = -1 },
		.pending = -1,
		.transform = -1,
		.cams = {
			[CAM_FRONT] = { .key = "front", .sensor = "ov2740",
					.location = "front",
					.description = "Yoga Book front camera",
					.input = 0, .w = 1920, .h = 1080 },
			[CAM_REAR] = { .key = "rear", .sensor = "ov8858",
				       .location = "back",
				       .description = "Yoga Book rear camera",
				       .input = 1, .w = 1600, .h = 1212 },
		},
	};
	int opt, n_cams = 0;
	bool default_camera = false;

	while ((opt = getopt_long(argc, argv, "d:f:r:u:MH:TDh", opts, NULL)) != -1) {
		switch (opt) {
		case 'd': d.dev_path = optarg; break;
		case 'T': d.test_pattern = true; break;
		case 'H': d.hinge_path = optarg; break;
		case 'D': default_camera = true; break;
		case 'M': d.cams[CAM_REAR].unfolded_mirror = true; break;
		case 'u':
			/* 0 or 180: a turn that keeps the frame size, so it can
			 * change while an app streams. */
			d.cams[CAM_REAR].unfolded_rot = atoi(optarg) % 360;
			if (d.cams[CAM_REAR].unfolded_rot % 180) {
				usage(argv[0]);
				return 2;
			}
			break;
		case 'f':
		case 'r':
			if (parse_rot(optarg, d.cams[opt == 'f' ? CAM_FRONT : CAM_REAR].rot) < 0) {
				usage(argv[0]);
				return 2;
			}
			break;
		default: usage(argv[0]); return opt == 'h' ? 0 : 2;
		}
	}
	setlinebuf(stderr);
	if (!d.hinge_path)
		d.hinge_path = find_hinge();
	if (default_camera) {
		int angle = d.hinge_path ? read_hinge(&d) : -1;
		int camera = angle >= 0 &&
			(angle < UNFOLDED_MIN || angle > UNFOLDED_MAX) ?
			CAM_REAR : CAM_FRONT;

		puts(d.cams[camera].description);
		return 0;
	}

	probe_sensors(&d);
	d.folded = true;
	if (hinge_dependent(&d.cams[CAM_REAR]) && !d.hinge_path)
		msg("no hinge sensor: the rear camera keeps its folded orientation");
	pw_init(&argc, &argv);
	d.main_loop = pw_main_loop_new(NULL);
	d.loop = pw_main_loop_get_loop(d.main_loop);
	pw_loop_add_signal(d.loop, SIGINT, on_quit, &d);
	pw_loop_add_signal(d.loop, SIGTERM, on_quit, &d);
	d.context = pw_context_new(d.loop, NULL, 0);
	d.core = pw_context_connect(d.context, NULL, 0);
	if (!d.core) {
		msg("cannot connect to PipeWire: %m");
		return 1;
	}
	for (int k = 0; k < N_CAMS; k++)
		d.cams[k].d = &d;
	d.sync_timer = pw_loop_add_timer(d.loop, on_sync, &d);
	d.start_timer = pw_loop_add_timer(d.loop, on_start_retry, &d);
	d.hinge_timer = pw_loop_add_timer(d.loop, on_hinge, &d);
	sync_nodes(&d);
	for (int k = 0; k < N_CAMS; k++)
		n_cams += d.cams[k].stream != NULL;
	if (!n_cams)
		return 1;
	if (setup_bus(&d) < 0)
		msg("no session bus: images will not follow the screen rotation");
	pw_main_loop_run(d.main_loop);

	camera_stop(&d);
	for (int k = 0; k < N_CAMS; k++)
		if (d.cams[k].stream)
			pw_stream_destroy(d.cams[k].stream);
	if (d.bus)
		sd_bus_flush_close_unref(d.bus);
	pw_core_disconnect(d.core);
	pw_context_destroy(d.context);
	pw_main_loop_destroy(d.main_loop);
	pw_deinit();
	return 0;
}
