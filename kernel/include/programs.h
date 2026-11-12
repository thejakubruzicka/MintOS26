#ifndef MINTOS26_PROGRAMS_H
#define MINTOS26_PROGRAMS_H
/* 26.5.014.0: programs bundled with the kernel image (tools/mkprograms.py)
 * and installed onto MintOS HD: command-line programs into /System/Programs
 * (Terminal's PATH) and examples into /Library/Examples. Installation runs
 * when a MintOS disk is mounted and whenever the bundle id differs from
 * /System/Programs/.bundle-id; it only ever writes to the MintOS disk. */
#include <stdint.h>

#define MINT_PROGRAMS_DIR "/System/Programs"
#define MINT_PROGRAMS_MARKER "/System/Programs/.bundle-id"

typedef struct mint_program_file {
    const char *path;          /* absolute destination on MintOS HD */
    const uint8_t *data;
    uint64_t size;
} MINT_PROGRAM_FILE;

extern const MINT_PROGRAM_FILE mint_program_bundle[];
extern const uint64_t mint_program_bundle_count;
extern const char mint_program_bundle_id[];

/* Returns files written (0 if already current), or a negative MINT_E* value. */
int64_t mint_programs_install(int force);
/* Kernel task: installs once per mounted MintOS disk. */
void mint_programs_task(void *argument);
#endif
