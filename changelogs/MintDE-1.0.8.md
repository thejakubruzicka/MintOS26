# MintDE 1.0.8 — Shut Down, Restart and a precise pointer

- The MintDE menu adds **Restart…** and **Shut Down…**. Both open a macOS-style alert ("Are you sure you want to shut down your computer now?") with Cancel and a default action button. Enter confirms and Esc cancels.
- If the platform ignores every power-off path, the alert says MintOS could not switch the computer off and asks you to hold the power button.
- New Terminal commands:
  - `power` shows the ACPI revision, OEM, `\_S5_` SLP_TYP values, PM1a control port, reset method and whether the FADT reports an 8042.
  - `shutdown`/`poweroff` and `restart`/`reboot` open the same confirmation alert.
- The pointer keeps its direction on fast movements, and slow movements are finer.
- The desktop follows whatever resolution the firmware selected, up to 4K.
- The About MintOS and About MintDE version lists include 26.5.009.0 and 1.0.8.
