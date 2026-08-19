#include <stdint.h>
#include <stdio.h>

static const uint64_t kMask48 = 0xffffffffffffull;

static void dump_one(const char *tag, uint64_t rawArray, int64_t slot, int64_t got, int64_t exp)
{
    uint64_t *raw = (uint64_t *)(rawArray & kMask48);
    uint64_t ref = 0;
    uint64_t obj = 0;
    uint64_t hdr = 0;
    int64_t idAt = 0;
    if (raw != NULL) {
        ref = raw[2 + slot];
        obj = ref & kMask48;
        if (obj != 0) {
            hdr = *(uint64_t *)obj;
            idAt = (int64_t)((uint64_t *)obj)[1];
        }
    }
    fprintf(stderr,
            "%s slot=%ld got=%ld exp=%ld d=%ld addr=0x%llx ref=0x%llx colour=0x%llx "
            "hdr=0x%llx state=%llu ti=0x%llx id_at_obj=%ld page=0x%llx\n",
            tag, (long)slot, (long)got, (long)exp, (long)(exp - got),
            (unsigned long long)obj, (unsigned long long)ref, (unsigned long long)(ref >> 48),
            (unsigned long long)hdr, (unsigned long long)((hdr >> 48) & 3ull),
            (unsigned long long)(hdr & kMask48), (long)idAt, (unsigned long long)(obj & ~0xfffull));
    fflush(stderr);
}

void sddrift_dump_node(uint64_t rawArray, int64_t slot, int64_t got, int64_t exp)
{
    dump_one("SD_DRIFT_NODE", rawArray, slot, got, exp);
}

void nwdrift_dump_node(uint64_t rawArray, int64_t slot, int64_t got, int64_t exp)
{
    dump_one("NW_DRIFT_NODE", rawArray, slot, got, exp);
}
