#include "llama-mmap.h"

#include <cstdint>
#include <cstdio>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#else
#include <unistd.h>
#endif

static size_t resident_bytes() {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc;
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        return 0;
    }
    return (size_t) pmc.WorkingSetSize;
#else
    FILE * f = std::fopen("/proc/self/statm", "r");
    if (!f) {
        return 0;
    }
    unsigned long total = 0;
    unsigned long resident = 0;
    const int n = std::fscanf(f, "%lu %lu", &total, &resident);
    std::fclose(f);
    if (n != 2) {
        return 0;
    }
    const long page = sysconf(_SC_PAGESIZE);
    return (size_t) resident * (size_t) page;
#endif
}

int main() {
    if (!llama_mmap::SUPPORTED) {
        std::fprintf(stderr, "skip: mmap not supported\n");
        return 0;
    }

    const size_t nbytes = 32u * 1024u * 1024u;
    char path[512];
#if defined(_WIN32)
    char dir[MAX_PATH];
    if (GetTempPathA(sizeof(dir), dir) == 0) {
        std::fprintf(stderr, "GetTempPathA failed\n");
        return 1;
    }
    std::snprintf(path, sizeof(path), "%sllama-mmap-unmap-test.bin", dir);
#else
    std::snprintf(path, sizeof(path), "/tmp/llama-mmap-unmap-test.bin");
#endif

    {
        std::vector<uint8_t> chunk(1024u * 1024u, 0x5a);
        FILE * out = std::fopen(path, "wb");
        if (!out) {
            std::fprintf(stderr, "fopen write failed: %s\n", path);
            return 1;
        }
        for (size_t wrote = 0; wrote < nbytes; wrote += chunk.size()) {
            if (std::fwrite(chunk.data(), 1, chunk.size(), out) != chunk.size()) {
                std::fclose(out);
                std::remove(path);
                std::fprintf(stderr, "fwrite failed\n");
                return 1;
            }
        }
        std::fclose(out);
    }

    int rc = 0;
    {
        llama_file file(path, "rb");
        llama_mmap mapping(&file, /* prefetch */ 0, /* numa */ false);

        volatile uint8_t sink = 0;
        auto * bytes = (uint8_t *) mapping.addr();
        for (size_t i = 0; i < mapping.size(); i += 4096) {
            sink = (uint8_t) (sink + bytes[i]);
        }

        const size_t before = resident_bytes();
        mapping.unmap_fragment(0, mapping.size());
        const size_t after = resident_bytes();

        if (before == 0) {
            std::fprintf(stderr, "skip: cannot read resident set\n");
        } else if (before <= after || before - after < nbytes / 2) {
            std::fprintf(stderr, "resident set did not drop: before %zu after %zu file %zu\n",
                    before, after, nbytes);
            rc = 1;
        } else {
            std::fprintf(stderr, "resident set dropped: before %zu after %zu\n", before, after);
        }
        (void) sink;
    }

    std::remove(path);
    return rc;
}
