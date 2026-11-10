VERSION := 26.5.013.0
CC := clang
LINK := lld-link
PYTHON := python3
BUILD := build
EFI := $(BUILD)/BOOTX64.EFI
ESP := $(BUILD)/mintos26-efi-volume.img
DISK := $(BUILD)/mintos26-$(VERSION)-uefi.img
DATA := $(BUILD)/mintos26-data-512m.img
VDI := $(BUILD)/mintos26-data-512m-vbox-dynamic.vdi
VDI_FIXED := $(BUILD)/mintos26-data-512m-fixed.vdi
VDI_BLANK := $(BUILD)/mintos26-data-512m-vbox-blank.vdi
VDI_TEMPLATE := assets/vbox-dynamic-512m-blank-template.vdi
KERNEL := $(BUILD)/mintos26-kernel.elf
USER_START_OBJ := $(BUILD)/user-crt0.obj
USER_RUNTIME_OBJ := $(BUILD)/user-runtime.obj
USER_APP_OBJ := $(BUILD)/user-init.obj
USER_DESKTOP_OBJ := $(BUILD)/user-desktop.obj
MINTDE_HEADERS := kernel/user/mintde/mintde.h kernel/user/mintde/gfx.h kernel/user/mintde/font.h kernel/user/include/mintos/runtime.h $(wildcard kernel/include/*.h kernel/include/uapi/mintos/*.h)
MINTDE_OBJS := $(BUILD)/mintde-gfx.obj $(BUILD)/mintde-font.obj $(BUILD)/mintde-widgets.obj $(BUILD)/mintde-apps.obj $(BUILD)/mintde-web.obj $(BUILD)/mintde-netcore.obj $(BUILD)/mintde-mem.obj $(BUILD)/crypto-sha2.obj $(BUILD)/crypto-aead.obj $(BUILD)/crypto-x25519.obj $(BUILD)/crypto-pubkey.obj $(BUILD)/crypto-x509.obj $(BUILD)/crypto-roots_data.obj $(BUILD)/crypto-tls.obj $(BUILD)/crypto-random.obj
USER_DESKTOP_ELF := $(BUILD)/user-desktop.elf
USER_DESKTOP_BLOB_ASM := $(BUILD)/desktop-elf-blob.S
USER_DESKTOP_BLOB_OBJ := $(BUILD)/desktop-elf-blob.obj
ICON_ASSET_ASM := $(BUILD)/icon-assets.S
ICON_ASSET_OBJ := $(BUILD)/icon-assets.obj
USER_ELF := $(BUILD)/user-init.elf
USER_C_FLAGS := -target x86_64-unknown-none-elf -ffreestanding -fno-stack-protector -fno-pic -fno-pie -fno-builtin -mno-red-zone -mno-sse -mno-mmx -mgeneral-regs-only -mcmodel=large -Wall -Wextra -Werror -O2 -Ikernel/include -Ikernel/user/include
USER_BLOB_ASM := $(BUILD)/user-elf-blob.S
USER_BLOB_OBJ := $(BUILD)/user-elf-blob.obj
KERNEL_OBJS := $(BUILD)/kernel.obj $(BUILD)/arch.obj $(BUILD)/arch-core.obj $(BUILD)/physmem.obj $(BUILD)/serial.obj $(BUILD)/scheduler.obj $(BUILD)/syscall.obj $(BUILD)/paging.obj $(BUILD)/heap.obj $(BUILD)/vmalloc.obj $(BUILD)/address_space.obj $(BUILD)/elf-loader.obj $(BUILD)/process.obj $(BUILD)/user-elf-blob.obj $(USER_DESKTOP_BLOB_OBJ) $(BUILD)/desktop-event.obj $(BUILD)/display-surface.obj $(BUILD)/desktop-services.obj $(BUILD)/ramfs.obj $(BUILD)/ps2-input.obj $(BUILD)/pci.obj $(BUILD)/e1000.obj $(BUILD)/storage-block.obj $(BUILD)/ahci.obj $(BUILD)/storage-diag.obj $(BUILD)/fat16-ro.obj $(BUILD)/driver-catalog.obj $(BUILD)/storage-probe.obj $(BUILD)/ata-identify.obj $(BUILD)/vfs.obj $(BUILD)/log.obj $(BUILD)/acpi-power.obj $(BUILD)/rtc.obj $(BUILD)/xhci.obj $(BUILD)/kmem.obj $(BUILD)/net.obj $(BUILD)/netcore.obj $(BUILD)/mintfs.obj $(BUILD)/mintos-disk.obj $(BUILD)/wallpaper.obj
KERNEL_FLAGS := -target x86_64-unknown-none-elf -ffreestanding -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -mno-sse -mno-mmx -mgeneral-regs-only -mcmodel=small -Wall -Wextra -O2
ISO := $(BUILD)/mintos26-$(VERSION)-uefi.iso
WALL := assets/wallpaper-1920x1080.rgb
ICON_PNG_FILES := $(wildcard assets/icons/*.png)
ICON_ESP_ARGS := $(foreach icon,$(ICON_PNG_FILES),MINTOS26/ICONS/$(notdir $(icon))=$(icon))
ICON_DATA_ARGS := $(foreach icon,$(ICON_PNG_FILES),MINTOS26/DEVELOP/ICONS/$(notdir $(icon))=$(icon))
CHANGELOG_FILES := $(wildcard changelogs/*.md)
CHANGELOG_ESP_ARGS := $(foreach note,$(CHANGELOG_FILES),MINTOS26/CHANGELOGS/$(notdir $(note))=$(note))
CHANGELOG_DATA_ARGS := $(foreach note,$(CHANGELOG_FILES),MINTOS26/DEVELOP/CHANGELOGS/$(notdir $(note))=$(note))
README := $(BUILD)/UEFI-README.TXT
EFI_FLAGS := -target x86_64-pc-win32-coff -ffreestanding -fshort-wchar -fno-stack-protector -fno-builtin -mno-red-zone -Wall -Wextra -Wno-sign-compare -O2

.PHONY: all clean check preview run kernel
all: $(EFI) $(USER_ELF) $(USER_DESKTOP_ELF) $(KERNEL) $(ESP) $(DISK) $(DATA) $(VDI) $(VDI_FIXED) $(VDI_BLANK) $(ISO)
kernel: $(KERNEL)
$(BUILD):
	mkdir -p $(BUILD)
$(BUILD)/main.obj: uefi/main.c uefi/font_aa.h | $(BUILD)
	$(CC) $(EFI_FLAGS) -c $< -o $@
$(BUILD)/kernel.obj: kernel/kernel.c kernel/include/mintos_fs.h kernel/include/net.h kernel/include/usb.h kernel/include/power.h kernel/include/rtc.h kernel/include/e1000.h kernel/include/boot_abi.h kernel/include/arch.h kernel/include/physmem.h kernel/include/serial.h kernel/include/task.h kernel/include/syscall.h kernel/include/paging.h kernel/include/heap.h kernel/include/vmalloc.h kernel/include/address_space.h kernel/include/elf_loader.h kernel/include/process.h kernel/include/desktop_abi.h kernel/include/desktop_services.h kernel/include/ramfs.h kernel/include/input.h kernel/include/pci.h kernel/include/storage_diag.h kernel/include/storage_diag_internal.h kernel/include/log.h kernel/include/uapi/mintos/syscall.h kernel/include/address_space.h kernel/include/process.h kernel/include/elf_loader.h kernel/include/paging.h | $(BUILD)
	$(CC) $(KERNEL_FLAGS) -Ikernel -c $< -o $@
$(BUILD)/arch.obj: kernel/arch/x86_64/arch.S | $(BUILD)
	$(CC) -target x86_64-unknown-none-elf -c $< -o $@
$(BUILD)/arch-core.obj: kernel/arch/x86_64/arch.c kernel/include/arch.h kernel/include/serial.h kernel/include/address_space.h kernel/include/process.h kernel/include/task.h | $(BUILD)
	$(CC) $(KERNEL_FLAGS) -Ikernel -c $< -o $@
$(BUILD)/physmem.obj: kernel/mm/physmem.c kernel/include/physmem.h kernel/include/boot_abi.h kernel/include/serial.h | $(BUILD)
	$(CC) $(KERNEL_FLAGS) -Ikernel -c $< -o $@
$(BUILD)/serial.obj: kernel/console/serial.c kernel/include/serial.h | $(BUILD)
	$(CC) $(KERNEL_FLAGS) -Ikernel -c $< -o $@
$(BUILD)/scheduler.obj: kernel/task/scheduler.c kernel/include/task.h kernel/include/arch.h kernel/include/serial.h kernel/include/address_space.h kernel/include/process.h kernel/include/elf_loader.h kernel/include/paging.h | $(BUILD)
	$(CC) $(KERNEL_FLAGS) -Ikernel -c $< -o $@
$(BUILD)/syscall.obj: kernel/abi/syscall.c kernel/include/mintos_fs.h kernel/include/net.h kernel/include/usb.h kernel/include/power.h kernel/include/rtc.h kernel/include/process.h kernel/include/e1000.h kernel/include/syscall.h kernel/include/task.h kernel/include/arch.h kernel/include/desktop_abi.h kernel/include/input.h kernel/include/desktop_services.h kernel/include/ramfs.h kernel/include/storage_diag.h kernel/include/driver_catalog.h kernel/include/log.h kernel/include/block.h kernel/include/fat16.h kernel/include/vfs.h kernel/include/pci.h kernel/include/uapi/mintos/syscall.h kernel/include/address_space.h kernel/include/process.h kernel/include/elf_loader.h kernel/include/paging.h | $(BUILD)
	$(CC) $(KERNEL_FLAGS) -Ikernel -c $< -o $@
$(BUILD)/paging.obj: kernel/mm/paging.c kernel/include/paging.h kernel/include/arch.h kernel/include/serial.h kernel/include/boot_abi.h | $(BUILD)
	$(CC) $(KERNEL_FLAGS) -Ikernel -c $< -o $@
$(BUILD)/heap.obj: kernel/mm/heap.c kernel/include/heap.h | $(BUILD)
	$(CC) $(KERNEL_FLAGS) -Ikernel -c $< -o $@
$(BUILD)/vmalloc.obj: kernel/mm/vmalloc.c kernel/include/vmalloc.h kernel/include/paging.h kernel/include/physmem.h kernel/include/serial.h | $(BUILD)
	$(CC) $(KERNEL_FLAGS) -Ikernel -c $< -o $@
$(BUILD)/address_space.obj: kernel/mm/address_space.c kernel/include/address_space.h kernel/include/paging.h kernel/include/address_space.h kernel/include/process.h kernel/include/elf_loader.h kernel/include/paging.h | $(BUILD)
	$(CC) $(KERNEL_FLAGS) -Ikernel -c $< -o $@
$(BUILD)/elf-loader.obj: kernel/proc/elf_loader.c kernel/include/elf_loader.h kernel/include/address_space.h kernel/include/paging.h kernel/include/address_space.h kernel/include/process.h kernel/include/elf_loader.h kernel/include/paging.h | $(BUILD)
	$(CC) $(KERNEL_FLAGS) -Ikernel -c $< -o $@
$(BUILD)/process.obj: kernel/proc/process.c kernel/include/mintos_fs.h kernel/include/process.h kernel/include/task.h kernel/include/elf_loader.h kernel/include/vmalloc.h kernel/include/ramfs.h kernel/include/address_space.h kernel/include/process.h kernel/include/elf_loader.h kernel/include/paging.h | $(BUILD)
	$(CC) $(KERNEL_FLAGS) -Ikernel -c $< -o $@
$(USER_START_OBJ): kernel/user/crt0.S | $(BUILD)
	$(CC) -target x86_64-unknown-none-elf -c $< -o $@
$(USER_RUNTIME_OBJ): kernel/user/runtime.c kernel/include/net.h kernel/include/usb.h kernel/include/power.h kernel/include/e1000.h kernel/include/e1000_core.h kernel/user/include/mintos/runtime.h kernel/include/uapi/mintos/syscall.h kernel/include/desktop_abi.h kernel/include/ramfs.h kernel/include/storage_diag.h kernel/include/fat16.h kernel/include/driver_catalog.h kernel/include/log.h kernel/include/block.h kernel/include/vfs.h kernel/include/pci.h | $(BUILD)
	$(CC) $(USER_C_FLAGS) -c $< -o $@
$(USER_APP_OBJ): kernel/user/init.c kernel/user/include/mintos/runtime.h kernel/include/uapi/mintos/syscall.h kernel/include/desktop_abi.h kernel/include/ramfs.h | $(BUILD)
	$(CC) $(USER_C_FLAGS) -c $< -o $@
$(USER_ELF): $(USER_START_OBJ) $(USER_RUNTIME_OBJ) $(USER_APP_OBJ) kernel/user/user.ld
	ld.lld -s -nostdlib -static -z max-page-size=0x1000 -T kernel/user/user.ld -o $@ $(USER_START_OBJ) $(USER_APP_OBJ) $(USER_RUNTIME_OBJ)
$(USER_BLOB_ASM): $(USER_ELF) tools/embed_elf.py
	$(PYTHON) tools/embed_elf.py $(USER_ELF) $@
$(USER_BLOB_OBJ): $(USER_BLOB_ASM)
	$(CC) -target x86_64-unknown-none-elf -c $< -o $@
$(BUILD)/desktop-event.obj: kernel/ui/desktop_event.c kernel/include/desktop_abi.h kernel/include/boot_abi.h | $(BUILD)
	$(CC) $(KERNEL_FLAGS) -Ikernel -c $< -o $@
$(BUILD)/display-surface.obj: kernel/ui/display_surface.c kernel/ui/font_aa.h kernel/ui/cursor_sprites.h kernel/include/desktop_abi.h kernel/include/boot_abi.h | $(BUILD)
	$(CC) $(KERNEL_FLAGS) -Ikernel -c $< -o $@
$(BUILD)/wallpaper.obj: kernel/ui/wallpaper.S assets/wallpaper-1920x1080.rgb assets/wallpaper-dark-1920x1080.rgb | $(BUILD)
	$(CC) -target x86_64-unknown-none-elf -c $< -o $@
$(BUILD)/desktop-services.obj: kernel/ui/desktop_services.c kernel/include/desktop_services.h kernel/include/desktop_abi.h | $(BUILD)
	$(CC) $(KERNEL_FLAGS) -Ikernel -c $< -o $@
$(BUILD)/ramfs.obj: kernel/fs/ramfs.c kernel/include/ramfs.h kernel/include/desktop_abi.h kernel/include/uapi/mintos/syscall.h kernel/include/address_space.h kernel/include/process.h kernel/include/elf_loader.h kernel/include/paging.h | $(BUILD)
	$(CC) $(KERNEL_FLAGS) -Ikernel -c $< -o $@
$(BUILD)/ps2-input.obj: kernel/input/ps2.c kernel/include/input.h kernel/include/arch.h kernel/include/desktop_abi.h | $(BUILD)
	$(CC) $(KERNEL_FLAGS) -Ikernel -c $< -o $@
$(BUILD)/pci.obj: kernel/bus/pci.c kernel/include/pci.h kernel/include/serial.h | $(BUILD)
	$(CC) $(KERNEL_FLAGS) -Ikernel -c $< -o $@
$(BUILD)/e1000.obj: kernel/net/e1000.c kernel/include/vmalloc.h kernel/include/e1000.h kernel/include/e1000_core.h kernel/include/pci.h kernel/include/physmem.h kernel/include/paging.h kernel/include/log.h | $(BUILD)
	$(CC) $(KERNEL_FLAGS) -Ikernel -c $< -o $@
$(BUILD)/storage-block.obj: kernel/block/block.c kernel/include/block.h | $(BUILD)
	$(CC) $(KERNEL_FLAGS) -Ikernel -c $< -o $@
$(BUILD)/ahci.obj: kernel/block/ahci.c kernel/include/block.h kernel/include/pci.h kernel/include/paging.h kernel/include/storage_diag.h kernel/include/storage_diag_internal.h kernel/include/log.h | $(BUILD)
	$(CC) $(KERNEL_FLAGS) -Ikernel -c $< -o $@
$(BUILD)/storage-diag.obj: kernel/diag/storage_diag.c kernel/include/storage_diag.h kernel/include/storage_diag_internal.h kernel/include/block.h kernel/include/fat16.h kernel/include/log.h | $(BUILD)
	$(CC) $(KERNEL_FLAGS) -Ikernel -c $< -o $@
$(BUILD)/fat16-ro.obj: kernel/fs/fat16.c kernel/include/fat16.h kernel/include/block.h kernel/include/storage_diag.h kernel/include/log.h kernel/include/uapi/mintos/syscall.h | $(BUILD)
	$(CC) $(KERNEL_FLAGS) -Ikernel -c $< -o $@
$(BUILD)/vfs.obj: kernel/fs/vfs.c kernel/include/vfs.h kernel/include/fat16.h kernel/include/block.h | $(BUILD)
	$(CC) $(KERNEL_FLAGS) -Ikernel -c $< -o $@
$(BUILD)/driver-catalog.obj: kernel/driver/catalog.c kernel/include/driver_catalog.h kernel/include/e1000.h kernel/include/usb.h kernel/include/net.h kernel/include/pci.h kernel/include/input.h kernel/include/storage_diag.h | $(BUILD)
	$(CC) $(KERNEL_FLAGS) -Ikernel -c $< -o $@
$(BUILD)/storage-probe.obj: kernel/fs/storage_diag.c kernel/include/storage_diag.h | $(BUILD)
	$(CC) $(KERNEL_FLAGS) -Ikernel -c $< -o $@
$(BUILD)/ata-identify.obj: kernel/diag/ata_identify.c kernel/include/storage_diag.h | $(BUILD)
	$(CC) $(KERNEL_FLAGS) -Ikernel -c $< -o $@
$(BUILD)/kmem.obj: kernel/lib/mem.c | $(BUILD)
	$(CC) $(KERNEL_FLAGS) -fno-builtin -Ikernel -c $< -o $@
$(BUILD)/xhci.obj: kernel/usb/xhci.c kernel/include/usb.h kernel/include/pci.h kernel/include/physmem.h kernel/include/paging.h kernel/include/input.h kernel/include/task.h kernel/include/log.h | $(BUILD)
	$(CC) $(KERNEL_FLAGS) -Ikernel -c $< -o $@
$(BUILD)/net.obj: kernel/net/net.c kernel/include/net.h kernel/include/e1000.h kernel/include/arch.h kernel/include/task.h kernel/include/log.h kernel/include/uapi/mintos/syscall.h | $(BUILD)
	$(CC) $(KERNEL_FLAGS) -c $< -o $@
$(BUILD)/netcore.obj: kernel/net/netcore.c kernel/include/net.h | $(BUILD)
	$(CC) $(KERNEL_FLAGS) -c $< -o $@
$(BUILD)/mintfs.obj: kernel/fs/mintfs.c kernel/include/mintfs.h | $(BUILD)
	$(CC) $(KERNEL_FLAGS) -c $< -o $@
$(BUILD)/mintos-disk.obj: kernel/fs/mintos_disk.c kernel/include/mintos_fs.h kernel/include/mintfs.h kernel/include/block.h kernel/include/physmem.h kernel/include/log.h kernel/include/rtc.h kernel/include/task.h kernel/include/uapi/mintos/syscall.h | $(BUILD)
	$(CC) $(KERNEL_FLAGS) -c $< -o $@
$(BUILD)/rtc.obj: kernel/time/rtc.c kernel/include/rtc.h kernel/include/serial.h | $(BUILD)
	$(CC) $(KERNEL_FLAGS) -Ikernel -c $< -o $@
$(BUILD)/acpi-power.obj: kernel/power/acpi.c kernel/include/power.h kernel/include/paging.h kernel/include/serial.h kernel/include/log.h | $(BUILD)
	$(CC) $(KERNEL_FLAGS) -Ikernel -c $< -o $@
$(BUILD)/log.obj: kernel/console/log.c kernel/include/log.h kernel/include/serial.h kernel/include/task.h kernel/include/address_space.h kernel/include/process.h kernel/include/elf_loader.h kernel/include/paging.h | $(BUILD)
	$(CC) $(KERNEL_FLAGS) -Ikernel -c $< -o $@
$(BUILD)/mintde-web.obj: kernel/user/mintde/web.c $(MINTDE_HEADERS) $(wildcard kernel/user/crypto/*.h) | $(BUILD)
	$(CC) $(USER_C_FLAGS) -Ikernel/user/crypto -c $< -o $@
CRYPTO_HEADERS := $(wildcard kernel/user/crypto/*.h)
$(BUILD)/crypto-%.obj: kernel/user/crypto/%.c $(CRYPTO_HEADERS) kernel/user/include/mintos/runtime.h | $(BUILD)
	$(CC) $(USER_C_FLAGS) -Ikernel/user/crypto -c $< -o $@
$(BUILD)/mintde-mem.obj: kernel/lib/mem.c | $(BUILD)
	$(CC) $(USER_C_FLAGS) -c $< -o $@
$(BUILD)/mintde-netcore.obj: kernel/net/netcore.c kernel/include/net.h | $(BUILD)
	$(CC) $(USER_C_FLAGS) -DMINT_NET_USERSPACE=1 -c $< -o $@
$(BUILD)/mintde-gfx.obj: kernel/user/mintde/gfx.c $(MINTDE_HEADERS) | $(BUILD)
	$(CC) $(USER_C_FLAGS) -c $< -o $@
$(BUILD)/mintde-font.obj: kernel/user/mintde/font_data.c kernel/user/mintde/font.h | $(BUILD)
	$(CC) $(USER_C_FLAGS) -c $< -o $@
$(BUILD)/mintde-widgets.obj: kernel/user/mintde/widgets.c $(MINTDE_HEADERS) kernel/user/include/mintos/runtime.h | $(BUILD)
	$(CC) $(USER_C_FLAGS) -c $< -o $@
$(BUILD)/mintde-apps.obj: kernel/user/mintde/apps.c $(MINTDE_HEADERS) $(wildcard kernel/user/crypto/*.h) kernel/user/include/mintos/runtime.h kernel/include/power.h | $(BUILD)
	$(CC) $(USER_C_FLAGS) -Ikernel/user/crypto -c $< -o $@
$(USER_DESKTOP_OBJ): kernel/user/desktop.c $(MINTDE_HEADERS) kernel/include/power.h kernel/include/e1000.h kernel/include/e1000_core.h kernel/user/include/mintos/runtime.h kernel/include/uapi/mintos/syscall.h kernel/include/desktop_abi.h kernel/include/ramfs.h kernel/include/storage_diag.h kernel/include/fat16.h kernel/include/driver_catalog.h kernel/include/log.h kernel/include/block.h kernel/include/vfs.h kernel/include/pci.h | $(BUILD)
	$(CC) $(USER_C_FLAGS) -c $< -o $@
$(USER_DESKTOP_ELF): $(USER_START_OBJ) $(USER_DESKTOP_OBJ) $(MINTDE_OBJS) $(USER_RUNTIME_OBJ) $(ICON_ASSET_OBJ) kernel/user/user.ld
	ld.lld -s -nostdlib -static -z max-page-size=0x1000 -T kernel/user/user.ld -o $@ $(USER_START_OBJ) $(USER_DESKTOP_OBJ) $(MINTDE_OBJS) $(USER_RUNTIME_OBJ) $(ICON_ASSET_OBJ)
$(USER_DESKTOP_BLOB_ASM): $(USER_DESKTOP_ELF) tools/embed_elf.py
	$(PYTHON) tools/embed_elf.py $(USER_DESKTOP_ELF) $@ mint_desktop_elf
$(USER_DESKTOP_BLOB_OBJ): $(USER_DESKTOP_BLOB_ASM)
	$(CC) -target x86_64-unknown-none-elf -c $< -o $@
$(ICON_ASSET_ASM): tools/embed_icons.py $(wildcard assets/icons/*.rgba) assets/icons/mint-leaf-32.a8 | $(BUILD)
	$(PYTHON) tools/embed_icons.py $@
$(ICON_ASSET_OBJ): $(ICON_ASSET_ASM)
	$(CC) -target x86_64-unknown-none-elf -c $< -o $@
$(KERNEL): $(KERNEL_OBJS) kernel/linker.ld
	ld.lld -nostdlib -static -z max-page-size=0x1000 -T kernel/linker.ld -o $@ $(KERNEL_OBJS)
$(BUILD)/desktop-event-uefi.obj: kernel/ui/desktop_event.c kernel/include/desktop_abi.h kernel/include/boot_abi.h | $(BUILD)
	$(CC) $(EFI_FLAGS) -Ikernel -c $< -o $@
$(BUILD)/display-surface-uefi.obj: kernel/ui/display_surface.c kernel/ui/font_aa.h kernel/include/desktop_abi.h kernel/include/boot_abi.h | $(BUILD)
	$(CC) $(EFI_FLAGS) -Ikernel -c $< -o $@
$(EFI): $(BUILD)/main.obj $(BUILD)/desktop-event-uefi.obj $(BUILD)/display-surface-uefi.obj
	$(LINK) /subsystem:efi_application /entry:efi_main /nodefaultlib /machine:x64 /out:$@ /filealign:512 /base:0x100000 $^
$(README): README.md | $(BUILD)
	python3 -c "from pathlib import Path; p=Path('README.md').read_text(); Path('$@').write_text(p,encoding='ascii',errors='replace')"
$(ESP): $(EFI) $(KERNEL) $(USER_ELF) $(USER_DESKTOP_ELF) $(WALL) $(README) assets/DEVELOP.TXT assets/HELLO.C assets/APP.TXT assets/LOCALE.TXT $(ICON_PNG_FILES) $(CHANGELOG_FILES) tools/mkfat16.py
	$(PYTHON) tools/mkfat16.py volume 65536 $@ EFI/BOOT/BOOTX64.EFI=$(EFI) MINTOS26/KERNEL.ELF=$(KERNEL) MINTOS26/USERINIT.ELF=$(USER_ELF) MINTOS26/DESKTOP.ELF=$(USER_DESKTOP_ELF) MINTOS26/WALLPAPE.RGB=$(WALL) MINTOS26/README.TXT=$(README) MINTOS26/DEVELOP.TXT=assets/DEVELOP.TXT MINTOS26/HELLO.C=assets/HELLO.C MINTOS26/APP.TXT=assets/APP.TXT MINTOS26/LOCALE.TXT=assets/LOCALE.TXT MINTOS26/CHANGELOG.TXT=CHANGELOG.md MINTOS26/FILESYS.TXT=docs/FILESYSTEM-ROADMAP.md MINTOS26/DRIVERS.TXT=docs/DRIVER-ROADMAP.md $(CHANGELOG_ESP_ARGS) $(ICON_ESP_ARGS)
$(DISK): $(ESP) tools/mkuefi_disk.py
	$(PYTHON) tools/mkuefi_disk.py $(ESP) $@
$(DATA): tools/mkfat16.py $(USER_ELF) $(USER_DESKTOP_ELF) $(README) assets/WELCOME.TXT assets/NOTES.TXT assets/SETTINGS.DAT assets/DEVELOP.TXT assets/HELLO.C assets/APP.TXT assets/LOCALE.TXT $(ICON_PNG_FILES) $(CHANGELOG_FILES) docs/FILESYSTEM-ROADMAP.md docs/DRIVER-ROADMAP.md
	$(PYTHON) tools/mkfat16.py disk 1048576 $@ MINTOS26/README.TXT=$(README) MINTOS26/DOCUMENTS/WELCOME.TXT=assets/WELCOME.TXT MINTOS26/DOCUMENTS/NOTES.TXT=assets/NOTES.TXT MINTOS26/DOCUMENTS/SETTINGS.DAT=assets/SETTINGS.DAT MINTOS26/DEVELOP/HELLO.C=assets/HELLO.C MINTOS26/DEVELOP/DEVELOP.TXT=assets/DEVELOP.TXT MINTOS26/DEVELOP/APP.TXT=assets/APP.TXT MINTOS26/DEVELOP/LOCALE.TXT=assets/LOCALE.TXT MINTOS26/DEVELOP/USERINIT.ELF=$(USER_ELF) MINTOS26/DEVELOP/DESKTOP.ELF=$(USER_DESKTOP_ELF) MINTOS26/DEVELOP/CHANGELOG.TXT=CHANGELOG.md MINTOS26/DEVELOP/FILESYS.TXT=docs/FILESYSTEM-ROADMAP.md MINTOS26/DEVELOP/DRIVERS.TXT=docs/DRIVER-ROADMAP.md $(CHANGELOG_DATA_ARGS) $(ICON_DATA_ARGS)
$(VDI): $(DATA) $(VDI_TEMPLATE) tools/seed_vbox_vdi.py
	$(PYTHON) tools/seed_vbox_vdi.py $(VDI_TEMPLATE) $(DATA) $@
$(VDI_FIXED): $(DATA) tools/raw_to_vdi.py
	$(PYTHON) tools/raw_to_vdi.py $(DATA) $@
$(VDI_BLANK): $(VDI_TEMPLATE) tools/clone_blank_vdi.py
	$(PYTHON) tools/clone_blank_vdi.py $(VDI_TEMPLATE) $@
$(ISO): $(ESP) $(README) tools/mkiso_uefi.py
	$(PYTHON) tools/mkiso_uefi.py $(ESP) $(README) $@
check: all
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror tools/test_e1000_core.c -o $(BUILD)/test-e1000-core
	$(BUILD)/test-e1000-core
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -DMINT_USB_HOST_TEST -DMINT_NET_USERSPACE tools/test_usb_net.c kernel/usb/xhci.c kernel/net/netcore.c -o $(BUILD)/test-usb-net
	$(BUILD)/test-usb-net
	$(PYTHON) tools/test_usb_net_contracts.py
	$(PYTHON) tools/test_crypto.py
	$(PYTHON) tools/test_x509.py
	$(PYTHON) tools/test_tls.py
	$(PYTHON) tools/test_tls_contracts.py
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror tools/test_mintfs.c kernel/fs/mintfs.c -o $(BUILD)/test-mintfs
	$(BUILD)/test-mintfs
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror tools/test_mintos_disk.c kernel/block/block.c kernel/fs/mintos_disk.c kernel/fs/mintfs.c -o $(BUILD)/test-mintos-disk
	$(BUILD)/test-mintos-disk
	$(PYTHON) tools/test_gpt.py
	$(PYTHON) tools/test_mintfs_contracts.py
	$(PYTHON) tools/test_e1000_driver.py
	$(PYTHON) tools/verify_uefi.py $(EFI) $(ESP) $(DISK) $(DATA) $(ISO) $(KERNEL) $(USER_ELF) $(USER_DESKTOP_ELF)
	$(PYTHON) tools/verify_native_desktop.py $(USER_DESKTOP_ELF)
	$(PYTHON) tools/test_mint26_003_features.py
	$(PYTHON) tools/verify_features.py
	$(PYTHON) tools/test_pointer_math.py
	$(PYTHON) tools/test_mint26_features.py
	$(PYTHON) tools/test_boot_selector.py
	$(PYTHON) tools/verify_kernel.py
	$(PYTHON) tools/test_kernel_timer.py
	$(PYTHON) tools/verify_scheduler.py
	$(PYTHON) tools/test_syscall_dispatch.py
	$(PYTHON) tools/test_user_syscalls.py
	$(PYTHON) tools/verify_userspace.py
	$(PYTHON) tools/verify_elf_loader.py $(USER_ELF)
	$(PYTHON) tools/test_context_switch.py
	$(PYTHON) tools/test_physmem.py
	$(PYTHON) tools/test_paging.py
	$(PYTHON) tools/test_heap.py
	$(PYTHON) tools/test_vmalloc.py
	$(PYTHON) tools/test_address_space.py
	$(PYTHON) tools/verify_address_space.py
	$(PYTHON) tools/test_desktop_boundary.py
	$(PYTHON) tools/test_ramfs_settings.py
	$(PYTHON) tools/test_ps2_input.py
	$(PYTHON) tools/test_hardware_compat.py
	$(PYTHON) tools/test_acpi.py
	$(PYTHON) tools/test_mintde2.py
	$(PYTHON) tools/test_pci.py
	$(PYTHON) tools/test_driver_foundation.py
	$(PYTHON) tools/test_terminal_stack_usage.py
	$(PYTHON) tools/test_storage_diag.py $(DATA)
	$(PYTHON) tools/test_fat16_ro.py $(DATA)
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -Ikernel/include tools/test_fat16_format.c kernel/fs/fat16.c kernel/block/block.c -o $(BUILD)/test-fat16-format
	$(BUILD)/test-fat16-format
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -Ikernel/include tools/test_vfs_mounts.c kernel/fs/vfs.c -o $(BUILD)/test-vfs-mounts
	$(BUILD)/test-vfs-mounts
	$(CC) -D_WIN32 -std=c11 -O2 -Wall -Wextra -Werror -Ikernel/include tools/test_display_bitmap.c kernel/ui/display_surface.c -o $(BUILD)/test-display-bitmap
	$(BUILD)/test-display-bitmap
	$(PYTHON) tools/verify_storage_diag.py $(DATA) $(VDI_FIXED) $(KERNEL) $(USER_DESKTOP_ELF)
	$(PYTHON) tools/test_vbox_vdi.py $(VDI_TEMPLATE) $(DATA) $(VDI)
	$(PYTHON) tools/test_vbox_blank_vdi.py $(VDI_TEMPLATE) $(VDI_BLANK)
	$(PYTHON) tools/verify_arch_hardening.py
	$(PYTHON) tools/verify_vmalloc.py
preview: | $(BUILD)
	mkdir -p previews
	$(PYTHON) tools/render_preview.py previews/mintos26-$(VERSION)-preview.png previews/mintos26-$(VERSION)-launcher-preview.png previews/mintos26-$(VERSION)-monitor-preview.png previews/mintos26-$(VERSION)-settings-preview.png previews/mintos26-$(VERSION)-finder-preview.png
	$(PYTHON) tools/render_native_shell_preview.py previews/mintos26-$(VERSION)-native-shell-preview.png previews/mintos26-$(VERSION)-native-shell-settings-preview.png previews/mintos26-$(VERSION)-storage-preview.png previews/mintos26-$(VERSION)-drivers-preview.png previews/mintos26-$(VERSION)-files-preview.png previews/mintos26-$(VERSION)-disk-utility-preview.png previews/mintos26-$(VERSION)-about-mintos-preview.png previews/mintos26-$(VERSION)-about-mintde-preview.png previews/mintos26-$(VERSION)-menu-dropdown-preview.png
	$(PYTHON) tools/render_boot_selector_preview.py previews/mintos26-$(VERSION)-boot-selector-preview.png
run: all
	tools/run_qemu.sh
clean:
	rm -f $(BUILD)/*
