# MintDE 1.0.3

- Fixed terminal volume-selection stack overflow by using shared static snapshot buffers.
- Added a `resources` terminal command to inspect PCI BAR and interrupt metadata without modifying the PCI configuration.
- Expanded `help` and driver status text to distinguish detected VirtualBox devices from drivers with functional I/O.
- If a user-mode CPU exception occurs, the process now exits as failed instead of halting the entire kernel.
