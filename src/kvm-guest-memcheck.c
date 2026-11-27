#include <err.h>
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>

#define MIB (1024UL * 1024)

static unsigned long number(const char *text, unsigned long max)
{
    char *end;
    unsigned long value;

    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno || end == text || *end || !value || value > max)
        errx(1, "invalid number: %s", text);
    return value;
}

static double monotonic_seconds(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now))
        err(1, "clock_gettime");
    return now.tv_sec + now.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
    unsigned long size_mib, seconds, pass = 0;
    double start, elapsed;
    size_t bytes, words;
    volatile uint64_t *memory;

    if (argc != 3)
        errx(1, "usage: guest-memcheck MiB seconds");

    size_mib = number(argv[1], 2048);
    seconds = number(argv[2], 600);
    bytes = size_mib * MIB;
    words = bytes / sizeof(*memory);
    memory = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED)
        err(1, "mmap");

    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("GUEST-MEM start: %lu MiB, minimum %lu s\n", size_mib, seconds);
    start = monotonic_seconds();
    do {
        uint64_t seed = UINT64_C(0x9e3779b97f4a7c15) * ++pass;

        for (size_t i = 0; i < words; i++)
            memory[i] = seed ^ i;

        for (size_t i = 0; i < words; i++) {
            uint64_t value = memory[i];
            if (value != (seed ^ i))
                errx(1, "pass %lu, word %zu: %016" PRIx64, pass, i, value);
        }

        elapsed = monotonic_seconds() - start;
        if (pass == 1 || pass % 5 == 0)
            printf("GUEST-MEM pass=%lu elapsed=%.2f s\n", pass, elapsed);
    } while (pass < 3 || elapsed < seconds);

    if (munmap((void *)memory, bytes))
        err(1, "munmap");
    printf("GUEST-MEM PASS: %lu MiB, %lu full write/verify passes, %.2f s\n",
           size_mib, pass, elapsed);
    return 0;
}
