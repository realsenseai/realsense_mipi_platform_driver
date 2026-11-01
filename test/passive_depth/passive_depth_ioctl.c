/* SPDX-License-Identifier: GPL-2.0 */
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define D500_CAMERA_CID_PASSIVE_DEPTH 0x009a4011U
#define DS5_CAMERA_CID_LASER_POWER 0x009a4001U
#define DS5_CAMERA_CID_MANUAL_LASER_POWER 0x009a4002U

struct laser_snapshot {
	int emitter;
	int power;
};

static volatile sig_atomic_t interrupted;

static void interrupt_test(int sig)
{
	interrupted = sig;
}

static int get_control(int fd, unsigned int id, int *value)
{
	struct v4l2_control ctrl = {.id = id};

	if (ioctl(fd, VIDIOC_G_CTRL, &ctrl))
		return -1;
	*value = ctrl.value;
	return 0;
}

static int get_mode(int fd, int *value)
{
	return get_control(fd, D500_CAMERA_CID_PASSIVE_DEPTH, value);
}

static int set_control(int fd, unsigned int id, int value)
{
	struct v4l2_control ctrl = {.id = id, .value = value};

	/* Do not retry even EINTR: the device may have accepted the write. */
	return ioctl(fd, VIDIOC_S_CTRL, &ctrl);
}

static int set_mode(int fd, int value, int *unknown)
{
	int ret = set_control(fd, D500_CAMERA_CID_PASSIVE_DEPTH, value);
	int error = errno;

	/* Only framework range rejection proves that no transaction ran. */
	if (ret && error != ERANGE) {
		*unknown = 1;
		fprintf(stderr, "FAIL: mode SET %d outcome UNKNOWN (%s); "
			"no further writes or automatic restoration\n",
			value, strerror(error));
	}
	errno = error;
	return ret;
}

static int snapshot_laser_control(int fd, unsigned int id, int *value)
{
	struct v4l2_queryctrl query = {.id = id};
	unsigned int unavailable = V4L2_CTRL_FLAG_DISABLED |
		V4L2_CTRL_FLAG_INACTIVE | V4L2_CTRL_FLAG_READ_ONLY |
		V4L2_CTRL_FLAG_WRITE_ONLY;

	if (ioctl(fd, VIDIOC_QUERYCTRL, &query) ||
	    (query.flags & unavailable) || get_control(fd, id, value)) {
		fprintf(stderr, "FAIL: cannot snapshot writable/readable laser "
			"control 0x%08x; refusing mode writes\n", id);
		return -1;
	}
	return 0;
}

static int restore_laser_control(int fd, unsigned int id, int value)
{
	int applied;

	if (set_control(fd, id, value) || get_control(fd, id, &applied) ||
	    applied != value) {
		fprintf(stderr, "FAIL: could not restore laser control "
			"0x%08x to %d\n", id, value);
		return -1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	struct v4l2_queryctrl query = {.id = D500_CAMERA_CID_PASSIVE_DEPTH};
	struct v4l2_querymenu menu = {.id = D500_CAMERA_CID_PASSIVE_DEPTH};
	const char * const names[] = {
		"Disabled", "Alternating Passive", "Full Passive",
	};
	int fd, original, mode, value, changed = 0, failed = 1;
	int busy, unknown = 0;
	struct laser_snapshot laser;

	if (argc != 3 || (strcmp(argv[1], "--idle") &&
			  strcmp(argv[1], "--busy"))) {
		fprintf(stderr, "Usage: %s --idle|--busy /dev/videoN\n", argv[0]);
		fprintf(stderr, "Run only inside hkr_device_lock; --busy requires "
			"an existing imaging stream.\n");
		return 2;
	}
	busy = !strcmp(argv[1], "--busy");
	fd = open(argv[2], O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		perror("open");
		return 1;
	}
	signal(SIGINT, interrupt_test);
	signal(SIGTERM, interrupt_test);
	if (ioctl(fd, VIDIOC_QUERYCTRL, &query) ||
	    query.type != V4L2_CTRL_TYPE_MENU || query.minimum != 0 ||
	    query.maximum != 2 ||
	    (query.flags & V4L2_CTRL_FLAG_DISABLED) ||
	    !(query.flags & V4L2_CTRL_FLAG_VOLATILE) ||
	    !(query.flags & V4L2_CTRL_FLAG_EXECUTE_ON_WRITE))
		goto out;
	for (mode = 0; mode < 3; mode++) {
		menu.index = mode;
		if (ioctl(fd, VIDIOC_QUERYMENU, &menu) ||
		    strcmp((const char *)menu.name, names[mode]))
			goto out;
	}
	if (get_mode(fd, &original) || original < 0 || original > 2)
		goto out;
	if (busy) {
		errno = 0;
		if (!set_control(fd, D500_CAMERA_CID_PASSIVE_DEPTH, original) ||
		    errno != EBUSY || get_mode(fd, &value) || value != original) {
			fprintf(stderr, "FAIL: busy same-value SET/GET check; "
				"no recovery write attempted\n");
			goto out;
		}
	} else {
		/* Mode writes canonicalize policy even when the value is unchanged. */
		if (snapshot_laser_control(fd, DS5_CAMERA_CID_LASER_POWER,
					   &laser.emitter) ||
		    snapshot_laser_control(fd, DS5_CAMERA_CID_MANUAL_LASER_POWER,
					   &laser.power))
			goto out;
		for (mode = 0; mode < 3 && !interrupted; mode++) {
			/* Mark before ioctl so failures still attempt restoration. */
			changed = 1;
			if (set_mode(fd, mode, &unknown) || get_mode(fd, &value) ||
			    value != mode || set_mode(fd, mode, &unknown))
				goto restore;
		}
		if (interrupted)
			goto restore;
		for (mode = -1; mode <= 3; mode += 4) {
			errno = 0;
			if (!set_mode(fd, mode, &unknown) || errno != ERANGE ||
			    get_mode(fd, &value) || value != 2)
				goto restore;
		}
	}
	failed = 0;
restore:
	if (changed) {
		if (unknown) {
			fprintf(stderr, "FAIL: restoration skipped: unknown mode "
				"outcome; saved mode=%d emitter=%d power=%d. "
				"Reconcile FW completion status before recovery.\n",
				original, laser.emitter, laser.power);
			failed = 1;
		} else if (set_mode(fd, original, &unknown) ||
			   get_mode(fd, &value) || value != original) {
			fprintf(stderr, "FAIL: could not restore original mode "
				"%d; saved emitter=%d power=%d; "
				"no laser writes attempted\n", original,
				laser.emitter, laser.power);
			failed = 1;
		} else {
			/* Restore intensity first and the user's enable state last. */
			if (restore_laser_control(fd,
					DS5_CAMERA_CID_MANUAL_LASER_POWER,
					laser.power))
				failed = 1;
			if (restore_laser_control(fd, DS5_CAMERA_CID_LASER_POWER,
						  laser.emitter))
				failed = 1;
		}
	}
out:
	close(fd);
	puts(failed ? "FAIL: passive-depth ioctl API" :
	     "PASS: passive-depth ioctl API");
	return failed;
}
