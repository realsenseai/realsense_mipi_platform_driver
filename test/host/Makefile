CC ?= gcc

.PHONY: test
test:
	@set -eu; work=$$(mktemp -d); trap 'rm -rf "$$work"' EXIT; \
	awk '/^#define (DS5_(DEPTH_CONTROL_BASE|RGB_CONTROL_BASE|MANUAL_EXPOSURE_LSB|MANUAL_EXPOSURE_MSB|MANUAL_GAIN|AUTO_EXPOSURE_MODE)|MAX_RGB_EXP|MAX_DEPTH_EXP)[[:space:]]/ {print} \
	/^static int ds5_hw_set_auto_exposure/ {emit=1} \
	/^#define DS5_MAX_LOG_WAIT/ {emit=0} \
	/^#define D500_CONTROL_COMPLETION_BASE/ {emit=1} \
	/^\/\* Auto-exposure algorithm types/ {emit=0} emit {print}' \
	../../kernel/realsense/d4xx_main.c > "$$work/production.h"; \
	$(CC) -std=gnu11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
	-I"$$work" test_dual_rgb_controls.c -o "$$work/test_dual_rgb_controls"; \
	"$$work/test_dual_rgb_controls"