#define _GNU_SOURCE

#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <reliefnt/vdso_data.h>

typedef int (*vdso_clock_fn)(int, struct timespec *);

struct mapped_vdso {
	void *reservation;
	size_t reservation_size;
	unsigned char *base;
};

#define CHECK(condition, message) \
	do { \
		if (!(condition)) { \
			fprintf(stderr, "FAIL: %s (line %d)\n", message, __LINE__); \
			return 1; \
		} \
	} while (0)

static int map_vdso(const char *path, struct mapped_vdso *mapped)
{
	struct stat statbuf;
	Elf64_Ehdr ehdr;
	Elf64_Phdr *phdrs = NULL;
	long page_size = sysconf(_SC_PAGESIZE);
	uint64_t max_vaddr = 0;
	int fd = -1;
	int status = -1;

	if (page_size != 4096) {
		fprintf(stderr, "test requires 4096-byte host pages\n");
		return -1;
	}
	fd = open(path, O_RDONLY);
	if (fd < 0 || fstat(fd, &statbuf) != 0 ||
	    pread(fd, &ehdr, sizeof(ehdr), 0) != (ssize_t)sizeof(ehdr)) {
		perror("open/read vDSO");
		goto out;
	}
	if (memcmp(ehdr.e_ident, ELFMAG, SELFMAG) != 0 ||
	    ehdr.e_ident[EI_CLASS] != ELFCLASS64 || ehdr.e_type != ET_DYN ||
	    ehdr.e_machine != EM_X86_64 || ehdr.e_phentsize != sizeof(Elf64_Phdr)) {
		fprintf(stderr, "unexpected vDSO ELF header\n");
		goto out;
	}
	phdrs = calloc(ehdr.e_phnum, sizeof(*phdrs));
	if (!phdrs || pread(fd, phdrs, ehdr.e_phnum * sizeof(*phdrs),
			  (off_t)ehdr.e_phoff) != (ssize_t)(ehdr.e_phnum * sizeof(*phdrs))) {
		perror("read vDSO program headers");
		goto out;
	}
	for (unsigned int i = 0; i < ehdr.e_phnum; ++i) {
		if (phdrs[i].p_type == PT_LOAD && phdrs[i].p_vaddr + phdrs[i].p_memsz > max_vaddr)
			max_vaddr = phdrs[i].p_vaddr + phdrs[i].p_memsz;
	}
	if (!max_vaddr || (max_vaddr + (uint64_t)page_size - 1) / (uint64_t)page_size > SIZE_MAX / (size_t)page_size) {
		fprintf(stderr, "invalid vDSO load range\n");
		goto out;
	}
	mapped->reservation_size = (size_t)((max_vaddr + (uint64_t)page_size - 1) /
					     (uint64_t)page_size) * (size_t)page_size + (size_t)page_size;
	mapped->reservation = mmap(NULL, mapped->reservation_size, PROT_NONE,
				   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mapped->reservation == MAP_FAILED) {
		mapped->reservation = NULL;
		perror("reserve vDSO address range");
		goto out;
	}
	mapped->base = (unsigned char *)mapped->reservation + page_size;
	if (mmap(mapped->base - page_size, (size_t)page_size, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED) {
		perror("map vvar page");
		goto out;
	}
	for (unsigned int i = 0; i < ehdr.e_phnum; ++i) {
		Elf64_Phdr *phdr = &phdrs[i];
		uintptr_t page_vaddr;
		uint64_t page_offset;
		size_t map_length;
		int prot = 0;

		if (phdr->p_type != PT_LOAD || phdr->p_filesz == 0)
			continue;
		if (phdr->p_memsz != phdr->p_filesz ||
		    (phdr->p_offset & ((uint64_t)page_size - 1)) !=
		    (phdr->p_vaddr & ((uint64_t)page_size - 1))) {
			fprintf(stderr, "unsupported vDSO PT_LOAD layout\n");
			goto out;
		}
		page_vaddr = (uintptr_t)(phdr->p_vaddr & ~((uint64_t)page_size - 1));
		page_offset = phdr->p_offset & ~((uint64_t)page_size - 1);
		map_length = (size_t)(((phdr->p_vaddr & ((uint64_t)page_size - 1)) +
				      phdr->p_filesz + (uint64_t)page_size - 1) &
				     ~((uint64_t)page_size - 1));
		if (phdr->p_flags & PF_R)
			prot |= PROT_READ;
		if (phdr->p_flags & PF_W)
			prot |= PROT_WRITE;
		if (phdr->p_flags & PF_X)
			prot |= PROT_EXEC;
		if (mmap(mapped->base + page_vaddr, map_length, prot,
			 MAP_PRIVATE | MAP_FIXED, fd, (off_t)page_offset) == MAP_FAILED) {
			perror("map vDSO PT_LOAD");
			goto out;
		}
	}
	status = 0;
out:
	free(phdrs);
	if (fd >= 0)
		close(fd);
	if (status != 0 && mapped->reservation) {
		munmap(mapped->reservation, mapped->reservation_size);
		mapped->reservation = NULL;
	}
	return status;
}

static uintptr_t find_dynamic_symbol(unsigned char *base, const char *name)
{
	Elf64_Ehdr *ehdr = (Elf64_Ehdr *)base;
	Elf64_Phdr *phdrs = (Elf64_Phdr *)(base + ehdr->e_phoff);
	Elf64_Dyn *dynamic = NULL;
	Elf64_Sym *symbols = NULL;
	const char *strings = NULL;
	uint32_t *hash = NULL;
	size_t string_size = 0;
	size_t symbol_count = 0;

	for (unsigned int i = 0; i < ehdr->e_phnum; ++i) {
		if (phdrs[i].p_type == PT_DYNAMIC) {
			dynamic = (Elf64_Dyn *)(base + phdrs[i].p_vaddr);
			break;
		}
	}
	if (!dynamic)
		return 0;
	for (Elf64_Dyn *entry = dynamic; entry->d_tag != DT_NULL; ++entry) {
		switch (entry->d_tag) {
		case DT_SYMTAB: symbols = (Elf64_Sym *)(base + entry->d_un.d_ptr); break;
		case DT_STRTAB: strings = (const char *)(base + entry->d_un.d_ptr); break;
		case DT_STRSZ: string_size = (size_t)entry->d_un.d_val; break;
		case DT_HASH: hash = (uint32_t *)(base + entry->d_un.d_ptr); break;
		default: break;
		}
	}
	if (!symbols || !strings || !string_size || !hash)
		return 0;
	symbol_count = hash[1];
	for (size_t i = 0; i < symbol_count; ++i) {
		if (symbols[i].st_name < string_size &&
		    strcmp(strings + symbols[i].st_name, name) == 0 &&
		    symbols[i].st_shndx != SHN_UNDEF)
			return (uintptr_t)(base + symbols[i].st_value);
	}
	return 0;
}

static uint64_t read_tsc(void)
{
	uint32_t low;
	uint32_t high;
	__asm__ volatile("rdtsc" : "=a"(low), "=d"(high));
	return ((uint64_t)high << 32) | low;
}

static int between(const struct timespec *value, const struct timespec *low,
		   const struct timespec *high)
{
	if (value->tv_sec < low->tv_sec || value->tv_sec > high->tv_sec)
		return 0;
	if (value->tv_sec == low->tv_sec && value->tv_nsec < low->tv_nsec)
		return 0;
	if (value->tv_sec == high->tv_sec && value->tv_nsec > high->tv_nsec)
		return 0;
	return 1;
}

int main(int argc, char **argv)
{
	void *handle;
	struct mapped_vdso mapped = { 0 };
	struct reliefnt_vdso_data *data;
	vdso_clock_fn version_gettime;
	vdso_clock_fn alias_gettime;
	vdso_clock_fn version_getres;
	vdso_clock_fn alias_getres;
	uintptr_t address;
	struct timespec result;
	struct timespec resolution;
	struct timespec before;
	struct timespec after;
	int (*mapped_gettime)(int, struct timespec *);
	int (*mapped_getres)(int, struct timespec *);

	CHECK(argc == 2, "usage: vdso_runtime_test vdso.so");
	handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
	if (!handle) {
		fprintf(stderr, "FAIL: dlopen vDSO: %s\n", dlerror());
		return 1;
	}
	version_gettime = (vdso_clock_fn)dlvsym(handle, "__vdso_clock_gettime", "LINUX_2.6");
	alias_gettime = (vdso_clock_fn)dlsym(handle, "clock_gettime");
	version_getres = (vdso_clock_fn)dlvsym(handle, "__vdso_clock_getres", "LINUX_2.6");
	alias_getres = (vdso_clock_fn)dlsym(handle, "clock_getres");
	CHECK(version_gettime && alias_gettime && version_getres && alias_getres,
	      "versioned vDSO entry points and default aliases resolve");
	CHECK(version_gettime == alias_gettime && version_getres == alias_getres,
	      "unprefixed aliases resolve to the versioned entry points");
	CHECK(version_gettime(0, NULL) == -EFAULT,
	      "NULL clock_gettime pointer reaches raw syscall and returns -EFAULT");
	CHECK(version_getres(0, NULL) == 0,
	      "valid clock_getres with NULL output succeeds through the syscall");
	CHECK(version_getres(12345, NULL) == -EINVAL,
	      "invalid clock_getres with NULL output preserves -EINVAL");

	CHECK(map_vdso(argv[1], &mapped) == 0, "map actual vDSO and adjacent vvar page");
	address = find_dynamic_symbol(mapped.base, "__vdso_clock_gettime");
	CHECK(address != 0, "find versioned clock_gettime in loaded ELF dynamic table");
	memcpy(&mapped_gettime, &address, sizeof(mapped_gettime));
	address = find_dynamic_symbol(mapped.base, "__vdso_clock_getres");
	CHECK(address != 0, "find versioned clock_getres in loaded ELF dynamic table");
	memcpy(&mapped_getres, &address, sizeof(mapped_getres));

	data = (struct reliefnt_vdso_data *)(mapped.base - 4096);
	memset(data, 0, sizeof(*data));
	data->seq = 0;
	data->clock_mode = RELIEFNT_VDSO_MODE_TSC;
	data->clock_mask = (1u << 0) | (1u << 1) | (1u << 5) | (1u << 6);
	data->hres_mask = (1u << 0) | (1u << 1);
	data->cycle_last = read_tsc();
	data->mult = UINT64_C(1) << RELIEFNT_VDSO_SHIFT;
	data->max_cycles = UINT64_C(1000000000000);
	data->resolution_ns = 1234;
	data->basetime[0].sec = 321;
	data->basetime[0].nsec = 765432100;
	data->basetime[1].sec = 654;
	data->basetime[1].nsec = 123456700;
	data->basetime[5].sec = 123;
	data->basetime[5].nsec = 456;
	data->basetime[6].sec = 789;
	data->basetime[6].nsec = 987654321;
	CHECK(mprotect(mapped.base - 4096, 4096, PROT_READ) == 0,
	      "vvar test mapping is readable and not writable");

	CHECK(mapped_gettime(5, &result) == 0 && result.tv_sec == 123 && result.tv_nsec == 456,
	      "CLOCK_REALTIME_COARSE reads its actual shared snapshot without TSC");
	CHECK(mapped_gettime(6, &result) == 0 && result.tv_sec == 789 && result.tv_nsec == 987654321,
	      "CLOCK_MONOTONIC_COARSE reads its actual shared snapshot without TSC");
	CHECK(mapped_gettime(0, &result) == 0 &&
	      (result.tv_sec > 321 || result.tv_nsec > 765432100) &&
	      result.tv_sec <= 322,
	      "CLOCK_REALTIME interpolates a trusted Q32 TSC snapshot");
	CHECK(mapped_gettime(1, &result) == 0 &&
	      (result.tv_sec > 654 || result.tv_nsec > 123456700) &&
	      result.tv_sec <= 655,
	      "CLOCK_MONOTONIC interpolates a trusted Q32 TSC snapshot");
	CHECK(mapped_getres(0, &resolution) == 0 && resolution.tv_sec == 0 &&
	      resolution.tv_nsec == 1234,
	      "clock_getres reads the high-resolution shared snapshot");

	CHECK(syscall(SYS_clock_gettime, 4, &before) == 0 &&
	      mapped_gettime(4, &result) == 0 &&
	      syscall(SYS_clock_gettime, 4, &after) == 0 && between(&result, &before, &after),
	      "valid unaccelerated clock falls back to raw syscall");
	CHECK(mapped_gettime(12345, &result) == -EINVAL,
	      "invalid clock ID returns the raw syscall's negative errno");

	CHECK(mprotect(mapped.base - 4096, 4096, PROT_READ | PROT_WRITE) == 0,
	      "make vvar test page writable for a controlled sequence update");
	data->seq = 1;
	CHECK(mprotect(mapped.base - 4096, 4096, PROT_READ) == 0,
	      "restore vvar read-only protection");
	CHECK(syscall(SYS_clock_gettime, 5, &before) == 0 &&
	      mapped_gettime(5, &result) == 0 &&
	      syscall(SYS_clock_gettime, 5, &after) == 0 && between(&result, &before, &after),
	      "odd sequence retry exhaustion returns a syscall result");

	CHECK(mprotect(mapped.base - 4096, 4096, PROT_READ | PROT_WRITE) == 0,
	      "make vvar test page writable for an unready snapshot");
	data->seq = 3;
	data->clock_mode = RELIEFNT_VDSO_MODE_NONE;
	data->hres_mask = 0;
	data->seq = 4;
	CHECK(mprotect(mapped.base - 4096, 4096, PROT_READ) == 0,
	      "restore vvar read-only protection after unready snapshot");
	CHECK(syscall(SYS_clock_gettime, 0, &before) == 0 &&
	      mapped_gettime(0, &result) == 0 &&
	      syscall(SYS_clock_gettime, 0, &after) == 0 && between(&result, &before, &after),
	      "unready high-resolution snapshot returns a syscall result");

	munmap(mapped.reservation, mapped.reservation_size);
	dlclose(handle);
	puts("PASS: dlvsym, raw syscall fallback, TSC interpolation, COARSE snapshot, seq retries");
	return 0;
}
