#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define BITS_PER_LONG (sizeof(unsigned long) * 8)
#define BITS_TO_LONGS(bits) (((bits) + BITS_PER_LONG - 1) / BITS_PER_LONG)

static int test_bit(unsigned int bit, const unsigned long *bits)
{
	return !!(bits[bit / BITS_PER_LONG] & (1UL << (bit % BITS_PER_LONG)));
}

static long long monotonic_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static const char *switch_name(unsigned int code)
{
	switch (code) {
	case SW_HEADPHONE_INSERT:
		return "HEADPHONE";
	case SW_MICROPHONE_INSERT:
		return "MICROPHONE";
	case SW_LINEOUT_INSERT:
		return "LINEOUT";
	default:
		return "SWITCH";
	}
}

static int find_jack_device(char *path, size_t path_size, char *name,
			    size_t name_size)
{
	unsigned long event_bits[BITS_TO_LONGS(EV_MAX + 1)];
	char candidate[64];
	int fd;

	for (int index = 0; index < 64; index++) {
		int length = snprintf(candidate, sizeof(candidate),
				      "/dev/input/event%d", index);

		if (length < 0 || (size_t)length >= sizeof(candidate))
			continue;
		fd = open(candidate, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
			continue;

		memset(name, 0, name_size);
		memset(event_bits, 0, sizeof(event_bits));
		if (ioctl(fd, EVIOCGNAME(name_size), name) >= 0 &&
		    ioctl(fd, EVIOCGBIT(0, sizeof(event_bits)), event_bits) >= 0 &&
		    test_bit(EV_SW, event_bits) && strstr(name, "Headset Jack")) {
			if (strlen(candidate) >= path_size) {
				close(fd);
				errno = ENAMETOOLONG;
				return -1;
			}
			strcpy(path, candidate);
			return fd;
		}
		close(fd);
	}

	errno = ENODEV;
	return -1;
}

static void print_initial_state(int fd)
{
	unsigned long switches[BITS_TO_LONGS(SW_MAX + 1)];

	memset(switches, 0, sizeof(switches));
	if (ioctl(fd, EVIOCGSW(sizeof(switches)), switches) < 0) {
		perror("EVIOCGSW");
		return;
	}
	printf("headphone=%d microphone=%d lineout=%d\n",
	       test_bit(SW_HEADPHONE_INSERT, switches),
	       test_bit(SW_MICROPHONE_INSERT, switches),
	       test_bit(SW_LINEOUT_INSERT, switches));
}

int main(int argc, char **argv)
{
	char path[64];
	char name[256];
	int seconds = 0;
	int fd;

	if (argc > 1) {
		char *end;
		long value = strtol(argv[1], &end, 10);

		if (!argv[1][0] || *end || value < 0 || value > 300) {
			fprintf(stderr, "usage: %s [monitor-seconds]\n", argv[0]);
			return 2;
		}
		seconds = (int)value;
	}

	fd = find_jack_device(path, sizeof(path), name, sizeof(name));
	if (fd < 0) {
		perror("Headset Jack input device");
		return 1;
	}
	printf("device=%s name=%s\n", path, name);
	print_initial_state(fd);

	if (seconds) {
		long long deadline = monotonic_ms() + seconds * 1000LL;
		int events = 0;

		while (monotonic_ms() < deadline) {
			struct pollfd pfd = { .fd = fd, .events = POLLIN };
			struct input_event event;
			int timeout = (int)(deadline - monotonic_ms());

			if (timeout < 0)
				timeout = 0;
			if (poll(&pfd, 1, timeout) <= 0)
				continue;
			while (read(fd, &event, sizeof(event)) == sizeof(event)) {
				if (event.type == EV_SW) {
					printf("%s code=%u value=%d\n",
					       switch_name(event.code), event.code,
					       event.value);
					events++;
				} else if (event.type == EV_KEY) {
					printf("BUTTON code=%u value=%d\n",
					       event.code, event.value);
					events++;
				}
			}
		}
		printf("jack_events=%d\n", events);
	}

	close(fd);
	return 0;
}
