/* M6.2 fuzz smoke: deterministic mutation harness.
 *
 * Loads the FITS corpus as seed, generates N mutated variants by random
 * single-byte flips inside the first 16 KiB, calls adapter open() on each,
 * and ensures the process doesn't crash. Run under ASan for full coverage.
 *
 * For real fuzzing use AFL++ or libFuzzer (compile with -fsanitize=fuzzer).
 * This is the "no obvious crashes" baseline that runs in under a minute.
 *
 * Args: <corpus-dir> [n_iterations] [rng_seed]
 */

#define _DEFAULT_SOURCE
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "fits_hdf5/adapter.h"
#include "fits_hdf5/registry.h"

static void mutate(unsigned char *buf, size_t n, unsigned int *rng)
{
    /* 5 random single-byte flips inside the first 16 KiB (or all of buf
     * if smaller). FITS headers live in the first few KB, so this hits
     * the parser hot zone. */
    size_t window = n < 16384 ? n : 16384;
    for (int i = 0; i < 5; ++i) {
        unsigned int r = rand_r(rng);
        buf[r % window] ^= (unsigned char)((r >> 16) | 1);
    }
}

static int load_file(const char *path, unsigned char **out_buf, size_t *out_n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    struct stat st; if (stat(path, &st) != 0) { fclose(f); return -1; }
    *out_n = (size_t)st.st_size;
    *out_buf = malloc(*out_n);
    if (!*out_buf) { fclose(f); return -1; }
    fread(*out_buf, 1, *out_n, f);
    fclose(f);
    return 0;
}

static int try_one(const unsigned char *bytes, size_t n)
{
    char tmp[] = "/tmp/fits_fuzz_XXXXXX.fits";
    int fd = mkstemps(tmp, 5);
    if (fd < 0) return -1;
    write(fd, bytes, n);
    close(fd);

    /* Drive the adapter directly; we're testing the FITS parser, not the
     * HDF5 path. Crash here is a hard failure the harness reports. */
    const fits_adapter_t *a = &fits_adapter;
    adapter_probe_result_t pr = {0};
    a->probe(tmp, &pr);
    if (pr.confidence > 0) {
        adapter_file_t *f = a->open(tmp, 0);
        if (f) a->close(f);   /* malformed-but-FITS-shaped: open may succeed */
    }
    unlink(tmp);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <corpus-dir> [iters=200] [seed=42]\n", argv[0]);
        return 2;
    }
    int iters = (argc >= 3) ? atoi(argv[2]) : 200;
    unsigned int seed = (argc >= 4) ? (unsigned int)atoi(argv[3]) : 42u;

    /* Collect corpus seeds. */
    DIR *d = opendir(argv[1]);
    if (!d) { perror("opendir"); return 1; }
    struct dirent *e;
    char *seeds[64]; int n_seeds = 0;
    while ((e = readdir(d)) && n_seeds < 64) {
        size_t L = strlen(e->d_name);
        if (L < 5 || strcmp(e->d_name + L - 5, ".fits") != 0) continue;
        char p[1024]; snprintf(p, sizeof(p), "%s/%s", argv[1], e->d_name);
        seeds[n_seeds++] = strdup(p);
    }
    closedir(d);
    if (n_seeds == 0) { fprintf(stderr, "no .fits seeds\n"); return 1; }

    int n_passed = 0;
    for (int i = 0; i < iters; ++i) {
        const char *seed_path = seeds[rand_r(&seed) % (unsigned)n_seeds];
        unsigned char *buf = NULL; size_t n = 0;
        if (load_file(seed_path, &buf, &n) != 0) continue;
        mutate(buf, n, &seed);
        if (try_one(buf, n) == 0) ++n_passed;
        free(buf);
    }

    for (int i = 0; i < n_seeds; ++i) free(seeds[i]);
    printf("OK: %d/%d mutation iterations completed without crash\n",
           n_passed, iters);
    return 0;
}
