# Uploaded VirtualBox VDI analysis

The supplied `MintOS 26.vdi` is a valid dynamic VDI 1.1, 512 MiB virtual capacity, with a 1 MiB block size and 512 logical blocks. Its block map begins at 1 MiB, the data region begins at 2 MiB, and all 512 block-map entries are `0xFFFFFFFF` (unallocated). A dynamic VDI with every block unallocated reads as zeroes. This matches the attached COM1 log: IDENTIFY reports 1,048,576 sectors, two LBA 0 reads match, the MBR signature bytes are `00 00`, the CRC32 is `0xB2AA7578`, and the disk marker is zero.

The original upload is preserved unchanged. The release build creates a new, separate initialized VDI from this template and the project's deterministic raw FAT16 data image. It assigns new create/modify UUIDs so both disks are not presented as identical media. No kernel disk writes are involved in creating or using the prepared release image.
