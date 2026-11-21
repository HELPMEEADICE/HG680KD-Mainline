#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define CHUNK_MB 16U
#define CHUNK_BYTES ((size_t)CHUNK_MB * 1024U * 1024U)
#define PAGE_SIZE 4096U

static uint64_t read_memfree_kb(void)
{
	FILE *fp = fopen("/proc/meminfo", "r");
	char line[128];
	uint64_t free_kb = 0;

	if (!fp)
		return 0;
	while (fgets(line, sizeof(line), fp)) {
		if (sscanf(line, "MemFree: %llu kB", (unsigned long long *)&free_kb) == 1)
			break;
	}
	fclose(fp);
	return free_kb;
}

static uint32_t pattern_for(size_t chunk_idx, size_t page_idx, size_t word_idx)
{
	uint32_t x = (uint32_t)((chunk_idx << 20) ^ (page_idx << 8) ^ word_idx);
	return x * 0x9e3779b1u ^ 0xa5a55a5au;
}

int main(int argc, char **argv)
{
	uint64_t free_kb = read_memfree_kb();
	size_t target_mb = 0;
	size_t nchunks, i;
	void **chunks;
	int pagemap_fd;
	uint64_t min_paddr = UINT64_MAX;
	uint64_t max_paddr = 0;
	uint64_t pages_above_128m = 0;
	uint64_t pages_above_760m = 0;
	uint64_t pages_above_1536m = 0;
	uint64_t total_mapped = 0;

	if (argc > 1) {
		target_mb = (size_t)strtoul(argv[1], NULL, 0);
	} else if (free_kb > 128 * 1024ULL) {
		target_mb = (size_t)((free_kb / 1024ULL) - 96ULL);
	} else {
		fprintf(stderr, "Insufficient MemFree: %llu kB\n", (unsigned long long)free_kb);
		return 1;
	}

	target_mb = (target_mb / CHUNK_MB) * CHUNK_MB;
	if (target_mb == 0) {
		fprintf(stderr, "Target size is 0 MiB\n");
		return 1;
	}

	nchunks = target_mb / CHUNK_MB;
	chunks = calloc(nchunks, sizeof(*chunks));
	if (!chunks) {
		perror("calloc");
		return 1;
	}

	pagemap_fd = open("/proc/self/pagemap", O_RDONLY);
	printf("memtest-2g: MemFree=%llu MiB, testing %zu MiB (%zu x %u MiB chunks)\n",
	       (unsigned long long)(free_kb / 1024ULL), target_mb, nchunks, CHUNK_MB);

	for (i = 0; i < nchunks; i++) {
		size_t p;
		uint8_t *base = mmap(NULL, CHUNK_BYTES, PROT_READ | PROT_WRITE,
				     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (base == MAP_FAILED) {
			fprintf(stderr, "mmap failed at chunk %zu (%zu MiB): %s\n",
				i, i * CHUNK_MB, strerror(errno));
			nchunks = i;
			break;
		}
		chunks[i] = base;

		for (p = 0; p < CHUNK_BYTES / PAGE_SIZE; p++) {
			volatile uint32_t *w = (volatile uint32_t *)(base + p * PAGE_SIZE);
			w[0] = pattern_for(i, p, 0);
			w[PAGE_SIZE / 8] = pattern_for(i, p, PAGE_SIZE / 8);
			w[PAGE_SIZE / 4 - 1] = pattern_for(i, p, PAGE_SIZE / 4 - 1);

			if (pagemap_fd >= 0) {
				uint64_t entry = 0;
				off_t off = (off_t)(((uintptr_t)w / PAGE_SIZE) * sizeof(entry));
				if (pread(pagemap_fd, &entry, sizeof(entry), off) == sizeof(entry) &&
				    (entry & (1ULL << 63))) {
					uint64_t pfn = entry & ((1ULL << 55) - 1);
					uint64_t paddr = pfn * PAGE_SIZE;
					if (paddr < min_paddr)
						min_paddr = paddr;
					if (paddr > max_paddr)
						max_paddr = paddr;
					if (paddr >= 0x08000000ULL)
						pages_above_128m++;
					if (paddr >= 0x2f800000ULL)
						pages_above_760m++;
					if (paddr >= 0x60000000ULL)
						pages_above_1536m++;
					total_mapped++;
				}
			}
		}
	}

	__asm__ __volatile__("" ::: "memory");

	for (i = 0; i < nchunks; i++) {
		size_t p;
		uint8_t *base = chunks[i];
		for (p = 0; p < CHUNK_BYTES / PAGE_SIZE; p++) {
			volatile uint32_t *w = (volatile uint32_t *)(base + p * PAGE_SIZE);
			uint32_t e0 = pattern_for(i, p, 0);
			uint32_t e1 = pattern_for(i, p, PAGE_SIZE / 8);
			uint32_t e2 = pattern_for(i, p, PAGE_SIZE / 4 - 1);
			if (w[0] != e0 || w[PAGE_SIZE / 8] != e1 || w[PAGE_SIZE / 4 - 1] != e2) {
				fprintf(stderr, "MISMATCH chunk=%zu page=%zu: got %08x/%08x/%08x exp %08x/%08x/%08x\n",
					i, p, w[0], w[PAGE_SIZE / 8], w[PAGE_SIZE / 4 - 1], e0, e1, e2);
				return 2;
			}
		}
	}

	for (i = 0; i < nchunks; i++)
		munmap(chunks[i], CHUNK_BYTES);
	free(chunks);
	if (pagemap_fd >= 0)
		close(pagemap_fd);

	printf("memtest-2g: verified %zu MiB, pages=%llu, paddr=[0x%08llx..0x%08llx]\n",
	       nchunks * CHUNK_MB, (unsigned long long)total_mapped,
	       (unsigned long long)min_paddr, (unsigned long long)max_paddr);
	printf("memtest-2g: >128M=%llu pages, >760M=%llu pages, >1536M=%llu pages\n",
	       (unsigned long long)pages_above_128m,
	       (unsigned long long)pages_above_760m,
	       (unsigned long long)pages_above_1536m);
	printf("MEMTEST-2G: PASS\n");
	return 0;
}
