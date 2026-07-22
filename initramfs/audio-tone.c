#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "/tmp/r11t-tone.raw";
	FILE *stream;
	unsigned int frame;

	stream = fopen(path, "wb");
	if (!stream) {
		fprintf(stderr, "open %s: %s\n", path, strerror(errno));
		return 1;
	}

	/* Two seconds of low-level 1 kHz, signed 16-bit stereo at 48 kHz. */
	for (frame = 0; frame < 96000; frame++) {
		int16_t sample = (frame % 48) < 24 ? 512 : -512;
		int16_t stereo[2] = { sample, sample };

		if (fwrite(stereo, sizeof(stereo), 1, stream) != 1) {
			fprintf(stderr, "write %s: %s\n", path, strerror(errno));
			fclose(stream);
			return 1;
		}
	}

	if (fclose(stream)) {
		fprintf(stderr, "close %s: %s\n", path, strerror(errno));
		return 1;
	}

	printf("tone=%s frames=96000 rate=48000 channels=2 amplitude=512\n", path);
	return 0;
}
