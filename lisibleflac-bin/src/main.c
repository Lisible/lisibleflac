#include <errno.h>
#include <lisibleflac.h>
#include <string.h>

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  if (argc != 2) {
    fprintf(stderr, "Usage: lisibleflac-bin <flac file>\n");
    return 1;
  }

  FILE *flac_file_handle = fopen(argv[1], "r");
  if (!flac_file_handle) {
    fprintf(stderr, "Couldn't open %s:\n\t%s\n", argv[1], strerror(errno));
    return 2;
  }
  lflac_decode(flac_file_handle);
  fclose(flac_file_handle);
  return 0;
}
