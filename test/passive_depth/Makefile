CC ?= cc
CFLAGS ?= -O2 -g
WARNINGS = -std=gnu99 -Wall -Wextra -Werror

.PHONY: all check clean
all: passive_depth_ioctl test_ioctl_cleanup

passive_depth_ioctl: passive_depth_ioctl.c
	$(CC) $(CFLAGS) $(WARNINGS) $< -o $@

test_ioctl_cleanup: test_ioctl_cleanup.c passive_depth_ioctl.c
	$(CC) $(CFLAGS) $(WARNINGS) $< -o $@

check: test_ioctl_cleanup
	python3 test_passive_depth.py
	./test_ioctl_cleanup

clean:
	$(RM) passive_depth_ioctl test_ioctl_cleanup
