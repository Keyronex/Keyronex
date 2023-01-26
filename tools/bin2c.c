#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int
main(int argc, char *argv[])
{
	const char *inname = NULL, *outname = NULL, *strname = NULL;
	FILE *in, *out;
	char *buf;
	size_t size;
	int opt;

	while ((opt = getopt(argc, argv, ":i:o:s:")) != -1) {
		switch (opt) {
		case 'i':
			printf("I %s\n", optarg);
			inname = optarg;
			break;

		case 'o':
			printf("o %s\n", optarg);
			outname = optarg;
			break;

		case 's':
			printf("s %s\n", optarg);
			strname = optarg;
			break;

		default:
			errx(EXIT_FAILURE, "bad arguments\n");
		}
	}

	in = fopen(inname, "rb");
	if (in == NULL) {
		err(EXIT_FAILURE, "failed to open %s to read", inname);
		fprintf(stderr, "%s: can't open %s for reading\n", argv[0],
		    argv[1]);
		return -1;
	}

	fseek(in, 0, SEEK_END);
	size = ftell(in);
	buf = (char *)malloc(size);
	fseek(in, 0, SEEK_SET);
	fread(buf, 1, size, in);
	fclose(in);

	out = fopen(outname, "w");
	if (out == NULL)
		err(EXIT_FAILURE, "failed to open %s for writing", outname);

	fprintf(out, "char %s[%zu] = {", strname, size);
	for (size_t i = 0; i < size; ++i) {
		if (i != 0)
			fprintf(out, ", ");
		if ((i % 4) == 0)
			fprintf(out, "\n\t");
		fprintf(out, "0x%.2x", buf[i] & 0xff);
	}
	fprintf(out, "\n};\n\n");

	fprintf(out, "unsigned int %s_size = %zu;\n", strname, size);
	fclose(out);

	return 0;
}
