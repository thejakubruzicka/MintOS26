# Worklog — MintOS26 26.5.004.0

- Source basis: 26.5.003.0, without overwriting previous source/releases.
- User COM1 evidence: `use sdb` and `use sda` both fault at the same RIP with PFEC 0x6 and CR2 0x0000007FFFFFA578. Disassembly showed `terminal_select_named_volume` reserving 0x2268 (~8.6 KiB) for an automatic storage snapshot before a syscall call; this exhausted the small user stack.
- Corrective change: device snapshots, PCI inventory, VFS mount table, log rows and several directory arrays are shared static userspace buffers in the cooperative desktop process.
- Hardening: CPL3 exceptions now try a process-local failure/return path; ring-0/unrecoverable faults remain kernel panics.
- Driver groundwork: read-only PCI BAR/IRQ inventory and terminal `resources` command; driver catalogue distinguishes detected hardware from functional drivers.
- Limit: no live VirtualBox runtime here. PCI resource inventory does not provide E1000, VMSVGA, USB or AC97 functional I/O.
