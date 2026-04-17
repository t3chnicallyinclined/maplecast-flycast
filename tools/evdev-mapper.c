/*
	MapleCast Evdev Button Mapper

	Interactive tool: asks you to press each MVC2 button, records the
	evdev key/axis code, saves to a simple map file that the evdev
	input thread reads at startup.

	Usage: sudo ./evdev-mapper [/dev/input/eventN]
	       (sudo needed for EVIOCGRAB)

	Output: ~/.config/flycast/maplecast-evdev-map.cfg

	File format (dead simple, one line per mapping):
	    LP=304
	    HP=305
	    LK=307
	    HK=308
	    A1=axis:2
	    A2=axis:5
	    UP=544
	    DOWN=545
	    LEFT=546
	    RIGHT=547
	    START=315
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <pwd.h>

// out_is_axis: 0=key, 1=trigger axis, 2=hat axis (value stored in out_code as HAT_CODE*10+direction)
static int wait_for_button(int fd, int* out_code, int* out_is_axis)
{
	struct input_event ev;
	// Drain any pending events first
	fcntl(fd, F_SETFL, O_NONBLOCK);
	while (read(fd, &ev, sizeof(ev)) == sizeof(ev)) {}
	fcntl(fd, F_SETFL, 0); // back to blocking

	while (1) {
		ssize_t n = read(fd, &ev, sizeof(ev));
		if (n != sizeof(ev)) return -1;

		// Digital button
		if (ev.type == EV_KEY && ev.value == 1) {
			*out_code = ev.code;
			*out_is_axis = 0;
			// Wait for release
			while (read(fd, &ev, sizeof(ev)) == sizeof(ev)) {
				if (ev.type == EV_KEY && ev.code == *out_code && ev.value == 0)
					break;
			}
			return 0;
		}

		// Hat/dpad axis (ABS_HAT0X, ABS_HAT0Y, ABS_HAT1X, etc.)
		if (ev.type == EV_ABS && ev.code >= ABS_HAT0X && ev.code <= ABS_HAT3Y) {
			if (ev.value != 0) {  // non-neutral
				*out_code = ev.code;
				*out_is_axis = 2;  // hat
				// Wait for return to neutral
				while (read(fd, &ev, sizeof(ev)) == sizeof(ev)) {
					if (ev.type == EV_ABS && ev.code == *out_code && ev.value == 0)
						break;
				}
				return 0;
			}
		}

		// Trigger axis (ABS_Z, ABS_RZ, ABS_BRAKE, ABS_GAS, etc.)
		if (ev.type == EV_ABS && (ev.code == ABS_Z || ev.code == ABS_RZ
		    || ev.code == ABS_BRAKE || ev.code == ABS_GAS
		    || ev.code == ABS_HAT1X || ev.code == ABS_HAT1Y
		    || ev.code == ABS_HAT2X || ev.code == ABS_HAT2Y)) {
			struct input_absinfo ai;
			ioctl(fd, EVIOCGABS(ev.code), &ai);
			int range = ai.maximum - ai.minimum;
			int threshold = range / 3;
			if (ev.value > ai.minimum + threshold) {
				*out_code = ev.code;
				*out_is_axis = 1;
				while (read(fd, &ev, sizeof(ev)) == sizeof(ev)) {
					if (ev.type == EV_ABS && ev.code == *out_code
					    && ev.value < ai.minimum + threshold)
						break;
				}
				return 0;
			}
		}

		// Stick axis used as direction (ABS_X, ABS_Y)
		if (ev.type == EV_ABS && (ev.code == ABS_X || ev.code == ABS_Y)) {
			struct input_absinfo ai;
			ioctl(fd, EVIOCGABS(ev.code), &ai);
			int center = (ai.maximum + ai.minimum) / 2;
			int deadzone = (ai.maximum - ai.minimum) / 4;
			if (abs(ev.value - center) > deadzone) {
				*out_code = ev.code;
				*out_is_axis = 2;  // treat like hat
				while (read(fd, &ev, sizeof(ev)) == sizeof(ev)) {
					if (ev.type == EV_ABS && ev.code == *out_code
					    && abs(ev.value - center) < deadzone)
						break;
				}
				return 0;
			}
		}
	}
}

static int find_joystick()
{
	for (int i = 0; i < 32; i++) {
		char path[64];
		snprintf(path, sizeof(path), "/dev/input/event%d", i);
		int fd = open(path, O_RDONLY);
		if (fd < 0) continue;

		unsigned long keybit[(KEY_MAX / 8 / sizeof(unsigned long)) + 1] = {};
		ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybit)), keybit);

		int bit = BTN_SOUTH / (8 * sizeof(unsigned long));
		int off = BTN_SOUTH % (8 * sizeof(unsigned long));
		if ((keybit[bit] >> off) & 1) {
			char name[256] = "Unknown";
			ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name);
			printf("Found: %s at %s\n", name, path);
			return fd;
		}
		close(fd);
	}
	return -1;
}

int main(int argc, char** argv)
{
	int fd;
	if (argc > 1) {
		fd = open(argv[1], O_RDONLY);
		if (fd < 0) {
			printf("Cannot open %s: %s\n", argv[1], strerror(errno));
			return 1;
		}
	} else {
		fd = find_joystick();
		if (fd < 0) {
			printf("No joystick found. Specify device: %s /dev/input/eventN\n", argv[0]);
			return 1;
		}
	}

	char name[256] = "Unknown";
	ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name);
	printf("\n");
	printf("  MapleCast Button Mapper\n");
	printf("  Device: %s\n", name);
	printf("  ─────────────────────────────\n");
	printf("  MVC2 layout:\n");
	printf("    LP  HP  A1\n");
	printf("    LK  HK  A2\n");
	printf("  ─────────────────────────────\n\n");

	// Grab device
	ioctl(fd, EVIOCGRAB, 1);

	struct { const char* name; const char* prompt; } buttons[] = {
		{"LP",    "Press LP (Light Punch)..."},
		{"HP",    "Press HP (Heavy Punch)..."},
		{"LK",    "Press LK (Light Kick)..."},
		{"HK",    "Press HK (Heavy Kick)..."},
		{"A1",    "Press A1 (Assist 1 / Left Trigger)..."},
		{"A2",    "Press A2 (Assist 2 / Right Trigger)..."},
		{"UP",    "Press UP on stick/dpad..."},
		{"DOWN",  "Press DOWN..."},
		{"LEFT",  "Press LEFT..."},
		{"RIGHT", "Press RIGHT..."},
		{"START", "Press START..."},
	};
	int count = sizeof(buttons) / sizeof(buttons[0]);

	int codes[11];
	int is_axis[11];

	for (int i = 0; i < count; i++) {
		printf("  %s ", buttons[i].prompt);
		fflush(stdout);
		if (wait_for_button(fd, &codes[i], &is_axis[i]) < 0) {
			printf("FAILED\n");
			ioctl(fd, EVIOCGRAB, 0);
			close(fd);
			return 1;
		}
		printf("%s %d\n", is_axis[i] ? "axis" : "key", codes[i]);
	}

	ioctl(fd, EVIOCGRAB, 0);
	close(fd);

	// Save to config
	const char* home = getenv("HOME");
	if (!home) {
		struct passwd* pw = getpwuid(getuid());
		home = pw ? pw->pw_dir : ".";
	}
	char configDir[512], configPath[512];
	snprintf(configDir, sizeof(configDir), "%s/.config/flycast", home);
	snprintf(configPath, sizeof(configPath), "%s/maplecast-evdev-map.cfg", configDir);

	FILE* f = fopen(configPath, "w");
	if (!f) {
		printf("\nCannot write %s: %s\n", configPath, strerror(errno));
		return 1;
	}

	fprintf(f, "# MapleCast evdev button map\n");
	fprintf(f, "# Device: %s\n", name);
	fprintf(f, "# Generated by evdev-mapper\n");
	for (int i = 0; i < count; i++) {
		if (is_axis[i] == 1)
			fprintf(f, "%s=axis:%d\n", buttons[i].name, codes[i]);
		else if (is_axis[i] == 2)
			fprintf(f, "%s=hat:%d\n", buttons[i].name, codes[i]);
		else
			fprintf(f, "%s=%d\n", buttons[i].name, codes[i]);
	}
	fclose(f);

	printf("\n  Saved to: %s\n", configPath);
	printf("  Run flycast with MAPLECAST_EVDEV_INPUT=1 to use this mapping.\n\n");
	return 0;
}
