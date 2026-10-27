# ClaudeDOS 1.2

> **Experimental / beta.** ClaudeDOS is an experimental project, built mainly to
> demonstrate what modern AI can do, and also as a novelty OS for breathing a
> little life into old laptops. Expect rough edges. It is not a daily-driver
> operating system and has no networking or saved storage, so don't rely on it for
> anything important.

A bare-bones bootable OS with a **windowed text-mode desktop**: a stock Linux
kernel, a BusyBox userland, ~300 lines of C for input, and a POSIX `sh` script
that draws everything. No systemd, no X11, no GNU coreutils. ~28MB, boots on
BIOS and UEFI, and has been run on real hardware (ThinkPad Edge, ThinkPad X220
Tablet, Toshiba C55-A5302, Dell OptiPlex) plus QEMU and VirtualBox.

Every app lives in its own window. Windows overlap, take focus, and can be
moved, resized, maximized and closed with the keyboard **or the mouse**.

```
 ClaudeDOS Desktop | Esc:window mode  ^N:start  ^X:close  | mouse: click/drag

    Notepad - scratch.txt                                  [=][x]
   |Dear diary,                                                 |
   |ClaudeDOS now has windows._    Calculator          [=][x]   |
   |                              | (12+30)*7                 | |
   |                              |    = 294                  | |
   |                              | > _                       | |
   +------------------------------+---------------------------+-+

 Start ^N | [Welcome] [Notepad] <Calculator>                      15:53
```

## Apps

Desktop icons (My Apps, My Computer, My Files), Settings (themes, timezone; reset on reboot), Notepad, Files (click or arrow through directories, open files in Notepad),
Games folder (Pong, Tic-Tac-Toe), About, Terminal (one command at a time), Calculator (`+ - x * / div mod`), System Info, About (neofetch
style, live data from `/proc`), Help, and a full-screen real shell for
anything interactive. Start menu = `Ctrl+N` or click **Start**; it lists the same items as the desktop icons.

## Controls

| Keyboard | |
|---|---|
| `Ctrl+N` | Start menu |
| `Esc` / `Ctrl+W` | window mode: arrows move, `+`/`-` resize, `Tab` next window, `M` maximize, `X` close, `N` Start, `Esc` back |
| `Ctrl+X` | close the focused window |
| `Ctrl+S` | save (Notepad) |

| Mouse | |
|---|---|
| click a window | focus / raise it |
| drag title bar | move |
| `[=]` / `[x]` | maximize / close |
| drag right/bottom edge or corner | resize |
| double-click | open desktop icons, maximize via title bar |
| right-click | context menu |
| wheel | scroll (scrollbar on the right edge) |
| taskbar entry / **Start** | switch window / open menu |
| Files: click, click again | select, open |

PS/2 pointing devices (trackpoint, touchpad) and USB mice work. USB/HID pens and touchscreens (e.g. the X220 Tablet's Wacom digitizer) are read
directly from evdev and scaled to the full screen; serial-attached digitizers
are untested.

## Architecture

| Layer | What it is |
|---|---|
| Kernel | Stock Ubuntu 24.04 `linux-image-generic` (6.8.0-134), unmodified |
| Bootloader | GRUB 2 via `grub-mkrescue` (hybrid BIOS + UEFI) |
| Init | `/init`, a shell script (PID 1) |
| Userland | static BusyBox; every applet symlinked into `/bin` |
| Input | [`kbmouse`](kbmouse.c): static C helper, keyboard + mouse -> one event stream |
| Desktop | [`dosshell`](dosshell): the window manager and all apps, in `sh` |
| Launcher | [`desktop`](desktop): `kbmouse \| dosshell`, restarted if it ever exits |
| Display | Linux VT text console driven by raw ANSI escapes; mouse pointer drawn through `/dev/vcsa` |

Boot: GRUB loads `vmlinuz` + `initramfs.img` -> `/init` mounts `/proc`, `/sys`,
`/dev`, `insmod`s the HID/storage/PS2-mouse modules, silences late kernel
messages, and `exec`s `setsid cttyhack /bin/desktop`.

How the pieces talk is in [`docs/INPUT-HELPER.md`](docs/INPUT-HELPER.md).
Real-hardware war stories are in [`docs/DEBUGGING-NOTES.md`](docs/DEBUGGING-NOTES.md).

## Build it

[`BUILD.md`](BUILD.md) has the exact commands. Short version: Ubuntu 24.04,
`gcc`, `busybox-static`, `grub-mkrescue`; compile `kbmouse`, assemble an
initramfs, run `grub-mkrescue`. Test with
`qemu-system-x86_64 -m 256 -cdrom claudedos-desktop.iso` (no `-nographic`;
the ISO draws on the screen, not the serial port).

## Known limitations

- **RAM only.** Nothing persists across reboots. (An installer existed in an
  earlier version and was removed.)
- Notepad edits only at the end of the text; no cursor movement inside it.
- Terminal window runs one command at a time; use the full-screen shell for
  `vi`, `top`, etc.
- No networking, no package manager. Clock defaults to UTC (change in Settings).
- The mouse pointer is a software cursor on the text grid, so it moves one
  character cell at a time.

## License

MIT for the code in this repo. The Linux kernel and BusyBox are GPLv2.
