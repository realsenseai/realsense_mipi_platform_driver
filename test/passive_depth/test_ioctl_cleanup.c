/* SPDX-License-Identifier: GPL-2.0 */
/* Compile the actual utility against an offline ioctl backend. */
#define open fake_open
#define close fake_close
#define ioctl fake_ioctl
#define main utility_main
#include "passive_depth_ioctl.c"
#undef main
#undef ioctl
#undef close
#undef open

#include <assert.h>
#include <stdarg.h>

static struct {
	int mode, emitter, power;
	int busy, mode_sets, laser_sets, fail_mode_at, mode_error;
	int apply_on_error, fail_snapshot, fail_laser;
	int open_count, close_count;
} camera;

int fake_open(const char *path, int flags, ...)
{
	assert(!strcmp(path, "offline-camera"));
	assert(flags == (O_RDWR | O_CLOEXEC));
	camera.open_count++;
	return 42;
}

int fake_close(int fd)
{
	assert(fd == 42);
	camera.close_count++;
	return 0;
}

static int *control_value(unsigned int id)
{
	switch (id) {
	case D500_CAMERA_CID_PASSIVE_DEPTH:
		return &camera.mode;
	case DS5_CAMERA_CID_LASER_POWER:
		return &camera.emitter;
	case DS5_CAMERA_CID_MANUAL_LASER_POWER:
		return &camera.power;
	default:
		assert(0);
		return NULL;
	}
}

int fake_ioctl(int fd, unsigned long request, ...)
{
	static const char * const names[] = {
		"Disabled", "Alternating Passive", "Full Passive",
	};
	va_list args;
	void *arg;

	assert(fd == 42);
	va_start(args, request);
	arg = va_arg(args, void *);
	va_end(args);
	if (request == VIDIOC_QUERYCTRL) {
		struct v4l2_queryctrl *q = arg;

		if (camera.fail_snapshot && q->id == DS5_CAMERA_CID_LASER_POWER) {
			errno = EIO;
			return -1;
		}
		q->type = V4L2_CTRL_TYPE_MENU;
		q->minimum = 0;
		q->maximum = 2;
		q->flags = V4L2_CTRL_FLAG_VOLATILE |
			V4L2_CTRL_FLAG_EXECUTE_ON_WRITE;
		return 0;
	}
	if (request == VIDIOC_QUERYMENU) {
		struct v4l2_querymenu *menu = arg;

		assert(menu->index < 3);
		strcpy((char *)menu->name, names[menu->index]);
		return 0;
	}
	if (request == VIDIOC_G_CTRL) {
		struct v4l2_control *c = arg;

		c->value = *control_value(c->id);
		return 0;
	}
	if (request == VIDIOC_S_CTRL) {
		struct v4l2_control *c = arg;

		if (c->id == D500_CAMERA_CID_PASSIVE_DEPTH) {
			camera.mode_sets++;
			if (camera.busy) {
				errno = EBUSY;
				return -1;
			}
			if (c->value < 0 || c->value > 2) {
				errno = ERANGE;
				return -1;
			}
			if (camera.mode_sets != camera.fail_mode_at ||
			    camera.apply_on_error) {
				camera.mode = c->value;
				camera.emitter = c->value != 2;
				camera.power = 225;
			}
			if (camera.mode_sets == camera.fail_mode_at) {
				errno = camera.mode_error;
				return -1;
			}
		} else {
			camera.laser_sets++;
			if ((int)c->id == camera.fail_laser) {
				errno = EIO;
				return -1;
			}
			*control_value(c->id) = c->value;
		}
		return 0;
	}
	assert(0);
	return -1;
}

static void reset_camera(int mode)
{
	memset(&camera, 0, sizeof(camera));
	camera.mode = mode;
	camera.emitter = 0;
	camera.power = 135;
	interrupted = 0;
}

static int run_utility(const char *option)
{
	char *argv[] = {"passive_depth_ioctl", (char *)option, "offline-camera"};
	int ret = utility_main(3, argv);

	assert(camera.open_count == 1 && camera.close_count == 1);
	return ret;
}

int main(void)
{
	const int uncertain_errors[] = {ETIMEDOUT, EREMOTEIO, EINTR, EIO,
				       EOPNOTSUPP, EINVAL, EBUSY};
	unsigned int i, applied;
	int mode;

	for (mode = 0; mode < 3; mode++) {
		reset_camera(mode);
		assert(!run_utility("--idle"));
		assert(camera.mode == mode && camera.emitter == 0 &&
		       camera.power == 135 && camera.laser_sets == 2);
	}
	reset_camera(1);
	camera.emitter = 1;
	assert(!run_utility("--idle"));
	assert(camera.mode == 1 && camera.emitter == 1 && camera.power == 135);
	puts("INFO: following FAIL diagnostics are expected fault injections");
	fflush(stdout);
	reset_camera(0);
	camera.fail_snapshot = 1;
	assert(run_utility("--idle") == 1);
	assert(!camera.mode_sets && !camera.laser_sets);
	for (i = 0; i < sizeof(uncertain_errors) / sizeof(uncertain_errors[0]);
	     i++) {
		for (applied = 0; applied < 2; applied++) {
			reset_camera(1);
			camera.fail_mode_at = 1;
			camera.mode_error = uncertain_errors[i];
			camera.apply_on_error = applied;
			assert(run_utility("--idle") == 1);
			assert(camera.mode_sets == 1 && !camera.laser_sets);
		}
	}
	reset_camera(0);
	camera.fail_mode_at = 2;
	camera.mode_error = ETIMEDOUT;
	assert(run_utility("--idle") == 1);
	assert(camera.mode_sets == 2 && !camera.laser_sets);
	reset_camera(0);
	camera.fail_mode_at = 9;
	camera.mode_error = ETIMEDOUT;
	assert(run_utility("--idle") == 1);
	assert(camera.mode_sets == 9 && !camera.laser_sets);
	reset_camera(0);
	camera.fail_laser = DS5_CAMERA_CID_MANUAL_LASER_POWER;
	assert(run_utility("--idle") == 1);
	assert(camera.emitter == 0 && camera.laser_sets == 2);
	reset_camera(0);
	camera.fail_laser = DS5_CAMERA_CID_LASER_POWER;
	assert(run_utility("--idle") == 1);
	assert(camera.power == 135 && camera.laser_sets == 2);
	reset_camera(1);
	camera.busy = 1;
	assert(!run_utility("--busy"));
	assert(camera.mode_sets == 1 && !camera.laser_sets &&
	       camera.emitter == 0 && camera.power == 135);
	puts("PASS: actual ioctl utility cleanup (24 offline scenarios)");
	return 0;
}
