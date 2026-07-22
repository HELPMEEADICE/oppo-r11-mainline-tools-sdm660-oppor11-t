// SPDX-License-Identifier: GPL-2.0-only

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define BITS_PER_LONG (sizeof(unsigned long) * 8)
#define NBITS(x) ((((x) - 1) / BITS_PER_LONG) + 1)
#define TEST_BIT(bit, array) \
	(((array)[(bit) / BITS_PER_LONG] >> ((bit) % BITS_PER_LONG)) & 1UL)

struct key_state {
	unsigned int code;
	const char *name;
	bool pressed;
	bool released;
};

struct input_device {
	int fd;
	char path[64];
	char name[128];
};

static struct key_state keys[] = {
	{ KEY_POWER, "POWER", false, false },
	{ KEY_VOLUMEUP, "VOLUMEUP", false, false },
	{ KEY_VOLUMEDOWN, "VOLUMEDOWN", false, false },
};

static int64_t monotonic_ms(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
		return -1;

	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static struct key_state *find_key(unsigned int code)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(keys); i++)
		if (keys[i].code == code)
			return &keys[i];

	return NULL;
}

static bool supports_test_key(int fd)
{
	unsigned long ev_bits[NBITS(EV_MAX + 1)] = { 0 };
	unsigned long key_bits[NBITS(KEY_MAX + 1)] = { 0 };
	size_t i;

	if (ioctl(fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits) < 0 ||
	    !TEST_BIT(EV_KEY, ev_bits))
		return false;

	if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) < 0)
		return false;

	for (i = 0; i < ARRAY_SIZE(keys); i++)
		if (TEST_BIT(keys[i].code, key_bits))
			return true;

	return false;
}

static int discover_devices(struct input_device *devices, size_t capacity)
{
	struct dirent *entry;
	DIR *dir;
	int count = 0;

	dir = opendir("/dev/input");
	if (!dir) {
		perror("opendir /dev/input");
		return -1;
	}

	while ((entry = readdir(dir)) != NULL && (size_t)count < capacity) {
		struct input_device *device;
		static const char prefix[] = "/dev/input/";
		size_t name_len;
		int fd;

		if (strncmp(entry->d_name, "event", 5) != 0)
			continue;

		device = &devices[count];
		name_len = strlen(entry->d_name);
		if (name_len >= sizeof(device->path) - sizeof(prefix) + 1)
			continue;
		memcpy(device->path, prefix, sizeof(prefix) - 1);
		memcpy(device->path + sizeof(prefix) - 1, entry->d_name,
		       name_len + 1);
		fd = open(device->path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0) {
			fprintf(stderr, "Cannot open %s: %s\n", device->path,
				strerror(errno));
			continue;
		}

		if (!supports_test_key(fd)) {
			close(fd);
			continue;
		}

		device->fd = fd;
		if (ioctl(fd, EVIOCGNAME(sizeof(device->name)), device->name) < 0)
			strcpy(device->name, "unknown");

		printf("Input: %s (%s)\n", device->path, device->name);
		count++;
	}

	closedir(dir);
	return count;
}

static bool all_keys_complete(void)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(keys); i++)
		if (!keys[i].pressed || !keys[i].released)
			return false;

	return true;
}

static void process_events(struct input_device *device)
{
	struct input_event events[16];
	ssize_t bytes;
	size_t count;
	size_t i;

	for (;;) {
		bytes = read(device->fd, events, sizeof(events));
		if (bytes < 0) {
			if (errno != EAGAIN && errno != EWOULDBLOCK)
				fprintf(stderr, "Read %s failed: %s\n", device->path,
					strerror(errno));
			return;
		}
		if (bytes == 0)
			return;
		if (bytes % sizeof(events[0]) != 0) {
			fprintf(stderr, "Short event record from %s\n", device->path);
			return;
		}

		count = bytes / sizeof(events[0]);
		for (i = 0; i < count; i++) {
			struct key_state *key;

			if (events[i].type != EV_KEY)
				continue;

			key = find_key(events[i].code);
			if (!key)
				continue;

			if (events[i].value == 1)
				key->pressed = true;
			else if (events[i].value == 0)
				key->released = true;

			printf("Event: %-10s %-10s %s\n", device->name,
			       key->name, events[i].value == 0 ? "UP" :
			       events[i].value == 1 ? "DOWN" : "REPEAT");
			fflush(stdout);
		}
	}
}

int main(int argc, char **argv)
{
	struct input_device devices[32] = { 0 };
	struct pollfd pollfds[ARRAY_SIZE(devices)];
	int duration = 20;
	int64_t deadline;
	int count;
	int i;
	int rc;

	if (argc > 2) {
		fprintf(stderr, "Usage: %s [seconds]\n", argv[0]);
		return 2;
	}
	if (argc == 2) {
		char *end;
		long value = strtol(argv[1], &end, 10);

		if (*end || value < 1 || value > 300) {
			fprintf(stderr, "Duration must be between 1 and 300 seconds\n");
			return 2;
		}
		duration = value;
	}

	count = discover_devices(devices, ARRAY_SIZE(devices));
	if (count <= 0) {
		fprintf(stderr, "No input device exposes POWER/VOLUMEUP/VOLUMEDOWN\n");
		return 1;
	}

	for (i = 0; i < count; i++) {
		pollfds[i].fd = devices[i].fd;
		pollfds[i].events = POLLIN;
	}

	printf("Quickly press and release POWER, VOLUMEUP, and VOLUMEDOWN once.\n");
	printf("Listening for %d seconds...\n", duration);
	deadline = monotonic_ms() + (int64_t)duration * 1000;

	while (!all_keys_complete()) {
		int64_t remaining = deadline - monotonic_ms();

		if (remaining <= 0)
			break;

		rc = poll(pollfds, count, remaining > 1000 ? 1000 : (int)remaining);
		if (rc < 0) {
			if (errno == EINTR)
				continue;
			perror("poll");
			break;
		}

		for (i = 0; i < count; i++)
			if (pollfds[i].revents & POLLIN)
				process_events(&devices[i]);
	}

	printf("=== key test summary ===\n");
	for (i = 0; i < (int)ARRAY_SIZE(keys); i++)
		printf("%-10s press=%s release=%s %s\n", keys[i].name,
		       keys[i].pressed ? "yes" : "no",
		       keys[i].released ? "yes" : "no",
		       keys[i].pressed && keys[i].released ? "PASS" : "FAIL");

	for (i = 0; i < count; i++)
		close(devices[i].fd);

	return all_keys_complete() ? 0 : 1;
}
