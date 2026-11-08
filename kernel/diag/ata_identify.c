#include <stdint.h>
#include <stddef.h>
#include "../include/storage_diag.h"

static uint64_t sectors48(const uint16_t words[256]) {
    return (uint64_t)words[100] | ((uint64_t)words[101] << 16) |
           ((uint64_t)words[102] << 32) | ((uint64_t)words[103] << 48);
}

int mint_ata_parse_capacity(const uint16_t words[256], MINT_ATA_CAPACITY_INFO *out) {
    if (!words || !out) return 0;
    out->lba28_sectors = (uint64_t)words[60] | ((uint64_t)words[61] << 16);
    out->lba48_sectors = sectors48(words);
    /* ATA words 82..84/85..87 have a validity pattern. Never trust the
       words 100..103 count merely because the reserved fields are non-zero. */
    uint16_t command_set_83 = words[83];
    int word83_valid = (command_set_83 & 0xC000u) == 0x4000u;
    out->lba48_supported = (uint32_t)(word83_valid && (command_set_83 & 0x0400u));
    out->flush_cache_supported = (uint32_t)(word83_valid && (command_set_83 & 0x1000u));
    out->flush_cache_ext_supported = (uint32_t)(word83_valid && (command_set_83 & 0x2000u));
    if (out->lba48_supported && out->lba48_sectors) {
        out->selected_sectors = out->lba48_sectors;
        out->source = MINT_STORAGE_CAPACITY_LBA48;
    } else if (out->lba28_sectors) {
        out->selected_sectors = out->lba28_sectors;
        out->source = MINT_STORAGE_CAPACITY_LBA28;
    } else {
        out->selected_sectors = 0;
        out->source = MINT_STORAGE_CAPACITY_UNKNOWN;
    }
    /* Word 49 bit 9 advertises LBA. Refuse to report a capacity if neither
       LBA addressing nor a non-zero, standards-qualified count is present. */
    if ((words[49] & (1u << 9)) == 0u) return 0;
    return out->selected_sectors != 0;
}

void mint_ata_copy_identify_string(const uint16_t words[256], uint32_t first_word,
                                   uint32_t word_count, char *out, uint32_t out_size) {
    if (!out || !out_size) return;
    uint32_t p = 0;
    if (words && first_word < 256u && word_count <= 256u - first_word) {
        for (uint32_t i = 0; i < word_count && p + 1u < out_size; ++i) {
            uint16_t w = words[first_word + i];
            char hi = (char)(w >> 8), lo = (char)(w & 0xFFu);
            out[p++] = hi >= 0x20 && hi <= 0x7E ? hi : ' ';
            if (p + 1u < out_size) out[p++] = lo >= 0x20 && lo <= 0x7E ? lo : ' ';
        }
    }
    while (p && out[p-1u] == ' ') --p;
    out[p] = '\0';
}
