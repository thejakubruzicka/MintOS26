#ifndef MINTOS26_PROCESS_H
#define MINTOS26_PROCESS_H
#include <stddef.h>
#include <stdint.h>
#include "address_space.h"
#include "elf_loader.h"

#define MINT_PROCESS_MAX 4u
#define MINT_PROCESS_KERNEL_STACK_BYTES (16u * 1024u)

typedef enum mint_process_state {
    MINT_PROCESS_UNUSED = 0,
    MINT_PROCESS_READY,
    MINT_PROCESS_RUNNING,
    MINT_PROCESS_EXITED,
    MINT_PROCESS_FAILED
} MINT_PROCESS_STATE;

typedef struct mint_process {
    MINT_ADDRESS_SPACE address_space;
    MINT_USER_IMAGE image;
    uint64_t pid;
    uint64_t parent_pid;
    uint64_t exit_code;
    uint64_t prior_privilege_stack_top;
    void *kernel_entry_stack;
    uint32_t state;
    uint32_t allocated;
} MINT_PROCESS;

void mint_process_system_init(void);
/* Create a process owned by the current scheduler task. The loader consumes a
 * fresh address space and maps a static ELF64 image plus private user stack. */
MINT_PROCESS *mint_process_create_current_from_elf(const void *image, size_t image_size);
/* Synchronously run the current task's attached process at CPL3 until SYS_EXIT. */
int mint_process_run_current(MINT_PROCESS *process);
uint64_t mint_process_pid(const MINT_PROCESS *process);
uint64_t mint_process_exit_code(const MINT_PROCESS *process);
MINT_PROCESS_STATE mint_process_state(const MINT_PROCESS *process);

#endif
