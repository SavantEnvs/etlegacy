/*
 * main() for the AFL file-input targets (msg_delta, cm_load, game_session,
 * image_bmp): one input file per process, `fuzz_<target> <file>`.
 *
 * The harness sources keep the libFuzzer entry-point names; build.sh
 * compiles them for these targets with
 *   -DLLVMFuzzerTestOneInput=fz_run -DLLVMFuzzerInitialize=fz_init
 * so the binary is a plain file-input program (the engine's error paths end
 * the process, which a libFuzzer target must not do).
 *
 * fz_init runs before the input is read: a harness that sets up per-process
 * state there may start the AFL fork server after it (__AFL_INIT).
 */
#include <stdio.h>
#include <stdlib.h>

int fz_run(const unsigned char *data, size_t size);
__attribute__((weak)) int fz_init(int *argc, char ***argv);

int main(int argc, char **argv)
{
	FILE          *f;
	long          n;
	unsigned char *buf;

	if (fz_init)
	{
		fz_init(&argc, &argv);
	}
	if (argc < 2)
	{
		fprintf(stderr, "usage: %s <input file>\n", argv[0]);
		return 2;
	}
	f = fopen(argv[1], "rb");
	if (!f)
	{
		perror(argv[1]);
		return 2;
	}
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n < 0)
	{
		fclose(f);
		return 2;
	}
	/* exactly the file's size, so reads past its end are caught */
	buf = (unsigned char *)malloc(n ? (size_t)n : 1);
	if (fread(buf, 1, (size_t)n, f) != (size_t)n)
	{
		fclose(f);
		free(buf);
		return 2;
	}
	fclose(f);
	fz_run(buf, (size_t)n);
	free(buf);
	return 0;
}
