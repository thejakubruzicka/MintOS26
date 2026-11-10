# MintDE 2.3.0 — Files that stay

- **Files works like Finder on MintOS HD.**
  - Favorites (home, Applications, Desktop, Documents, Downloads) and Locations (MintOS HD, FAT16 volumes, Session RAM).
  - A Date Modified column, folders sorted by name, and the free space shown in the status bar.
  - New Folder (⇧⌘N), inline Rename (Return), with the name selected but not the extension, as in Finder.
  - Duplicate (⌘D), Move to Trash (⌘⌫), Delete Immediately (⌥⌘⌫) and Empty Trash (⇧⌘⌫), each with a confirmation sheet.
  - These commands are also in the File menu.
  - The Trash in the Dock opens the Trash folder, which has an Empty button.
  - Double-clicking a text file opens it in Notes.
- **Notes saves to `~/Documents/Notes.txt`.**
  - Saving happens about a second after you stop typing, and the save is atomic.
  - The session note carries over the first time.
  - Notes can open any text file up to 16 KB, and typing accepts accented letters.
  - Without a MintOS disk, Notes keeps working in session RAM as before.
- **Disk Utility** can erase a blank disk as **MintOS (MintFS)**, now the default, or **MS-DOS (FAT16)**. A MintOS disk shows a GUID partition map and how much space it uses.
- **Settings › Storage** shows the MintOS HD volume with its free space and item count.
- **Terminal** works on MintOS HD with `ls [-l]`, `cd`, `pwd`, `cat`, `open`, `mkdir`, `touch`, `rm [-r]`, `rmdir`, `mv`, `cp`, `echo … > file` / `>>`, `df` and `sync`. `use sda` switches to a read-only FAT16 volume, and `use hd` switches back.
- **Sidebar vibrancy:** Files, Storage, Drivers and Disk Utility sidebars blur and tint what is behind the window, like macOS. The menu bar updates immediately when another window is focused.
