#include <stdint.h>
#include <stdio.h>

static const uint64_t kMask48 = 0xffffffffffffull;

uint64_t sddrift_slot_ref(uint64_t rawArray, int64_t slot)
{
    uint64_t* raw = (uint64_t*)(rawArray & kMask48);
    if (raw == NULL) {
        return 0;
    }
    return raw[2 + slot];
}

void sddrift_dump_node(uint64_t rawArray, int64_t slot, int64_t got, int64_t exp, uint64_t lastRef)
{
    uint64_t cur = sddrift_slot_ref(rawArray, slot);
    fprintf(stderr, "SD_DRIFT_NODE slot=%ld got=%ld exp=%ld d=%ld curRef=0x%llx lastRef=0x%llx\n",
            (long)slot, (long)got, (long)exp, (long)(exp - got),
            (unsigned long long)cur, (unsigned long long)lastRef);
    fflush(stderr);
}
