# 26.5.014.0 app platform: FPU/SSE state, frame pool, processes, pipes,
# Linux-compat syscall subset, bundled programs. Included from the Makefile
# right after KERNEL_OBJS so the objects join the kernel link.
APP_KERNEL_HEADERS := $(wildcard kernel/include/*.h kernel/include/uapi/mintos/*.h)
APP_PLATFORM_KERNEL_OBJS := $(BUILD)/fpu.obj $(BUILD)/frame-pool.obj $(BUILD)/proc-files.obj \
	$(BUILD)/proc-memory.obj $(BUILD)/proc-syscalls.obj $(BUILD)/linux-table.obj \
	$(BUILD)/linux-compat.obj $(BUILD)/linux-entry.obj $(BUILD)/programs.obj \
	$(BUILD)/program-bundle.obj $(BUILD)/fputest-elf-blob.obj
KERNEL_OBJS += $(APP_PLATFORM_KERNEL_OBJS)

$(BUILD)/fpu.obj: kernel/arch/x86_64/fpu.c $(APP_KERNEL_HEADERS) | $(BUILD)
	$(CC) $(KERNEL_FLAGS) -Ikernel -c $< -o $@
$(BUILD)/frame-pool.obj: kernel/mm/frame_pool.c $(APP_KERNEL_HEADERS) | $(BUILD)
	$(CC) $(KERNEL_FLAGS) -Ikernel -c $< -o $@
$(BUILD)/proc-files.obj: kernel/proc/proc_files.c $(APP_KERNEL_HEADERS) | $(BUILD)
	$(CC) $(KERNEL_FLAGS) -Ikernel -c $< -o $@
$(BUILD)/proc-memory.obj: kernel/proc/proc_memory.c $(APP_KERNEL_HEADERS) | $(BUILD)
	$(CC) $(KERNEL_FLAGS) -Ikernel -c $< -o $@
$(BUILD)/proc-syscalls.obj: kernel/abi/proc_syscalls.c $(APP_KERNEL_HEADERS) | $(BUILD)
	$(CC) $(KERNEL_FLAGS) -Ikernel -c $< -o $@
$(BUILD)/linux-table.obj: kernel/abi/linux_table.c $(APP_KERNEL_HEADERS) | $(BUILD)
	$(CC) $(KERNEL_FLAGS) -Ikernel -c $< -o $@
$(BUILD)/linux-compat.obj: kernel/abi/linux_compat.c $(APP_KERNEL_HEADERS) | $(BUILD)
	$(CC) $(KERNEL_FLAGS) -Ikernel -c $< -o $@
$(BUILD)/linux-entry.obj: kernel/arch/x86_64/linux_entry.S | $(BUILD)
	$(CC) -target x86_64-unknown-none-elf -c $< -o $@
$(BUILD)/programs.obj: kernel/proc/programs.c $(APP_KERNEL_HEADERS) | $(BUILD)
	$(CC) $(KERNEL_FLAGS) -Ikernel -c $< -o $@

# Boot self-test program for FPU/SSE/AVX state isolation (SSE allowed here).
FPUTEST_C_FLAGS := -target x86_64-unknown-none-elf -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
	-fno-builtin -mno-red-zone -mcmodel=large -Wall -Wextra -Werror -O2 -Ikernel/include -Ikernel/user/include
$(BUILD)/user-fputest.obj: kernel/user/tests/fputest.c kernel/user/include/mintos/runtime.h | $(BUILD)
	$(CC) $(FPUTEST_C_FLAGS) -c $< -o $@
$(BUILD)/user-fputest.elf: $(USER_START_OBJ) $(BUILD)/user-fputest.obj $(USER_RUNTIME_OBJ) kernel/user/user.ld
	ld.lld -s -nostdlib -static -z max-page-size=0x1000 -T kernel/user/user.ld -o $@ $(USER_START_OBJ) $(BUILD)/user-fputest.obj $(USER_RUNTIME_OBJ)
$(BUILD)/fputest-elf-blob.S: $(BUILD)/user-fputest.elf tools/embed_elf.py
	$(PYTHON) tools/embed_elf.py $< $@ mint_fputest_elf
$(BUILD)/fputest-elf-blob.obj: $(BUILD)/fputest-elf-blob.S
	$(CC) -target x86_64-unknown-none-elf -c $< -o $@

# Programs and examples installed onto MintOS HD (see kernel/include/programs.h).
PROGRAM_BUNDLE_FILES ?=
PROGRAM_BUNDLE_ARGS ?=
$(BUILD)/program-bundle.S: tools/mkprograms.py $(PROGRAM_BUNDLE_FILES) | $(BUILD)
	$(PYTHON) tools/mkprograms.py $@ $(PROGRAM_BUNDLE_ARGS)
$(BUILD)/program-bundle.obj: $(BUILD)/program-bundle.S
	$(CC) -target x86_64-unknown-none-elf -c $< -o $@
