# ClaudeDOS
A bare-bones, retro-styled bootable OS built on a real (unmodified) Linux kernel with a from-scratch BusyBox userland -- no systemd, no GNU coreutils, no desktop stack. It boots straight into a keyboard-only, MS-DOS-Shell-style menu interface. ~28MB total, boots on both legacy BIOS and UEFI, tested on real hardware (ThinkPad, Dell OptiPlex). Right now it does not work on VirtualBox, but this may change in th future.

Architecture Layer	

What it actually is


Kernel	Stock linux-image-generic (Ubuntu 24.04's kernel), unmodified
Bootloader	GRUB 2, built with grub-mkrescue -- hybrid BIOS + UEFI El Torito boot from one ISO
Init (PID 1)	A plain shell script at /init inside the initramfs -- no systemd, no sysvinit
Userland	BusyBox (statically linked single binary), symlinked to provide sh, ls, cat, mount, etc.
Menu system	/bin/dosshell -- a POSIX sh script, the entire UI
Console	Linux VT (kernel framebuffer text console), driven via raw ANSI escape codes
There is deliberately no systemd, no NetworkManager, no GNU coreutils, no X11. Every command available at runtime is a BusyBox applet, symlinked individually and on purpose -- see docs/BUSYBOX-APPLETS.md for exactly which ones and why each is there.

How boot actually works


GRUB loads the kernel (vmlinuz) and the initramfs (initramfs.img)
The kernel unpacks the initramfs (a cpio archive) into a RAM-backed root filesystem and execs /init
/init (see init):
mounts /proc, /sys, /dev (devtmpfs)
insmods three kernel modules by hand: hid.ko, usbhid.ko, hid-generic.ko -- USB keyboard support isn't automatic in a minimal initramfs; see docs/KEYBOARD-DRIVERS.md for why all three are actually needed
prints the ANSI banner
execs into setsid busybox cttyhack /bin/dosshell -- this properly attaches a controlling TTY (without it, job control and some input handling silently breaks -- see docs/DEBUGGING-NOTES.md)
/bin/dosshell takes over as the actual interactive menu system and never exits until shutdown
Building it yourself
See BUILD.md for the full, exact command sequence -- this is a real, runnable build script, not a summary. Requires a Debian/Ubuntu machine (or VM) with root access; takes a couple of minutes.

The menu system (dosshell)


Everything under [2] Useful Tools and [3] Misc is implemented as plain shell functions in one file: dosshell. Notable pieces:

Free-typing text editor -- unlike everything else in the menu (which uses line-buffered read), the editor puts the terminal into raw mode (stty raw -echo) and reads one keystroke at a time via dd bs=1 count=1, so you can type naturally with working Backspace/Enter, save with Ctrl+S, exit with Ctrl+X. See docs/RAW-INPUT-EDITOR.md for exactly how that works and the bugs that came up building it.
Live clock -- top-right corner, updates via a save-cursor / reposition / write / restore-cursor ANSI sequence so it doesn't flicker-redraw the whole screen. Two other approaches (read -t polling, a background & process) were tried first and both broke keyboard input -- see the debugging notes for why.
neofetch-style About screen -- pulls real, live data from /proc (/proc/cpuinfo, /proc/meminfo, /proc/uptime) rather than hardcoding anything.
Known limitations
No persistence -- this is a live/RAM-only system. Anything you save (including in the text editor) is gone on reboot unless you extend it with a real writable overlay.
No arrow-key cursor movement in the text editor -- you can type forward and Backspace, but not navigate mid-document. Full cursor movement would need escape-sequence parsing for arrow keys, which isn't implemented.
No networking, no package manager -- this is intentionally bare bones. A separate, much larger build in this project's history adds a real debootstrap-based Ubuntu userland with apt; that's a different tradeoff (much bigger, but a "real" distro).
License
BusyBox is GPLv2. The Linux kernel is GPLv2. Everything written for this project specifically (the init script, dosshell, the build process) -- license it however you want; MIT is a reasonable default if you don't have a preference.
