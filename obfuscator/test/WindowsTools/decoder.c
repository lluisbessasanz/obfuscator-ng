#include <stdint.h>

typedef struct ARR_DEC_INFO {
    void    *ptr;
    uint32_t bits;
    uint64_t mask;
} ARR_DEC_INFO;

void __deobf_data(ARR_DEC_INFO *items, uint32_t count) {
    if (!items)
        return;

    for (uint32_t i = 0; i < count; i++) {
        if (!items[i].ptr)
            continue;

        switch (items[i].bits) {
        case 8:
            *(uint8_t *)items[i].ptr ^= (uint8_t)items[i].mask;
            break;

        case 16:
            *(uint16_t *)items[i].ptr ^= (uint16_t)items[i].mask;
            break;

        case 32:
            *(uint32_t *)items[i].ptr ^= (uint32_t)items[i].mask;
            break;

        case 64:
            *(uint64_t *)items[i].ptr ^= (uint64_t)items[i].mask;
            break;

        default:
            break;
        }
    }
}