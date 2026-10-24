# Debugging notes

Real problems hit building and testing this on actual hardware (not just
in QEMU), and what actually fixed them. Written up because most of these
are non-obvious and would waste someone else's time rediscovering them.

## `console=ttyS0` hangs real hardware, but works fine in QEMU

**Symptom:** boots fine in QEMU every time. On real hardware (tested on
both a 2012 ThinkPad Edge and a Dell OptiPlex -- different machines,
different chipsets), it hangs indefinitely right after the kernel prints
`random: crng init done`, before ever reaching `/init`.

**Cause:** the kernel command line specified `console=tty0 console=ttyS0`.
`ttyS0` is a *real physical serial port*. In QEMU, that's a virtual serial
port that always behaves perfectly, so testing there never surfaces the
problem. On real hardware, if there's a physical UART with hardware flow
control expected and nothing plugged into it, the kernel can hang
indefinitely writing to that console waiting for a handshake signal
(CTS/DCD) that will never come.

**Fix:** don't put `console=ttyS0` in the shipped kernel command line at
all. Only `console=tty0`. (Add `ttyS0` back temporarily and only for local
QEMU testing where you need serial output for headless debugging.)

## GRUB drops to a bare `grub>` prompt instead of showing the menu

**Symptom:** instead of the boot menu, some machines/VMs show a raw
`grub>` command-line, no menu, no error.

**Cause:** the ISO's GRUB config wasn't being found/parsed by whatever
mechanism that machine's firmware used to auto-locate it. This turned out
to be VM/firmware-dependent (some VirtualBox configurations, some real
BIOS/UEFI implementations).

**Fix:** use plain `grub-mkrescue` rather than hand-rolling a custom
`grub-mkstandalone` boot chain. `grub-mkrescue`'s built-in device-search
logic is what upstream GRUB actually tests against real-world firmware
diversity -- reinventing it (which was tried first, using an embedded
memdisk config + hardcoded device search) turned out less reliable, not
more, despite seeming more "robust" on paper.

If you do land at a bare `grub>` prompt, you can still boot manually:
```
ls
set root=(cd0)          # or whatever ls shows for your boot device
linux /boot/vmlinuz console=tty0
initrd /boot/initramfs.img
boot
```

## Kernel panic: `IO-APIC + timer doesn't work!` (VirtualBox specifically)

**Symptom:** kernel panics almost immediately on boot, specifically in
VirtualBox, with the message quoted above and a call trace mentioning
`setup_IO_APIC`.

**Cause:** VirtualBox's virtual chipset defaults sometimes mismatch what
the kernel expects for IO-APIC/timer calibration -- a known category of VM
compatibility issue, not something wrong with the kernel or the build.

**Fix:** add `noapic nolapic` to the kernel command line. (VirtualBox
Settings -> System -> Motherboard -> "Enable I/O APIC" is an alternative
fix on the VM-config side, but baking the kernel params into the ISO means
it works regardless of how someone's VM happens to be configured.)

## Keyboard detected in `dmesg` but no keystrokes ever register

**Symptom:** `dmesg`/boot log clearly shows the keyboard being found (e.g.
`Product: Dell USB Entry Keyboard`), but nothing typed ever does anything.

**Cause:** USB device *enumeration* and USB device *becoming a usable
input device* are two different kernel layers. `usbhid.ko` only handles
the USB transport. The driver that actually binds a HID device to real
keyboard input (feeding keystrokes into the console) is `hid-generic.ko`
-- a separate module that's easy to miss because the symptom (device
"detected" in logs) looks like success.

**Fix:** load all three: `hid.ko`, `usbhid.ko`, `hid-generic.ko` (in that
order doesn't strictly matter, but all three must be present). Verified by
testing with an actual USB input device attached in QEMU
(`-usb -device usb-kbd`) and confirming the specific log line:
```
hid-generic 0003:xxxx:xxxx.0001: input,hidraw0: USB HID v1.11 Keyboard [...]
```
Just checking that `insmod` succeeds without an error is **not** enough --
that only proves the module loaded, not that a real input device would
successfully bind to it.

## `sh: can't access tty; job control turned off`

**Symptom:** boots to a shell, but oddly behaved -- and this exact message
appears at startup.

**Cause:** the shell was started without a properly attached controlling
terminal.

**Fix:** `exec setsid busybox cttyhack /bin/dosshell` instead of just
`exec /bin/busybox sh`. `cttyhack` (a real BusyBox applet) explicitly grabs
the console as a controlling TTY, which is required for `read` and job
control to behave correctly.

## Live clock: two approaches that seemed reasonable and both broke input

Wanted a live-updating clock without redrawing (flickering) the whole
screen every update.

**Attempt 1: `read -t 60`** (a timed read, so the loop could periodically
wake up and refresh just the clock even with no user input). Verified in
isolation with a minimal test script that `read -t` alone works fine. But
in the *full* menu script, combined with `cttyhack`, keystrokes stopped
being delivered to the running `read -t` call reliably -- confirmed by
comparing directly against a plain (non-timed) `read` in an otherwise
identical test, which worked every time.

**Attempt 2: background process** (`clock_updater & ; CLOCK_PID=$!`,
looping `sleep 60; update_clock` independently of the foreground menu
loop). This also broke input -- backgrounding a job under a
single `cttyhack`-controlled session appears to cause terminal/job-control
ownership conflicts in this minimal environment.

**What actually shipped:** neither. The clock only updates when the menu
is freshly redrawn (on boot, and whenever you return from a submenu) --
not a true per-second/per-minute tick while idle. Less flashy, but doesn't
risk breaking keyboard input, which matters more.

**Lesson:** in this specific minimal environment (BusyBox + `cttyhack` +
raw VT console), don't trust that a mechanism "should" work because it's
POSIX-standard or works in isolation -- test it inside the actual full
script under actual real (or QEMU) boot conditions before relying on it.

## `dd`'s own status output leaking into captured variables

**Symptom:** while building the raw-keystroke text editor, a test showed a
captured character mixed with garbage (`[a1+0 records in\n1+0 records
out]` instead of just `[a]`).

**Cause:** `dd` writes its transfer-summary (`1+0 records in` / `1+0
records out`) to **stderr**, not stdout. Redirecting `2>&1` inside a
`$(...)` command substitution merges that stderr summary into the captured
value.

**Fix:** don't redirect `dd`'s stderr into the capture -- use `dd bs=1
count=1 2>/dev/null` (suppress it entirely), not `2>&1` (which was only
added for debugging and should never ship).

## Menu box "staircase" misalignment on real hardware

**Symptom:** the bordered menu box's right edge looked jagged/stepped on
real hardware, even though it looked fine when just eyeballing the source.

**Cause:** menu item lines were manually space-padded by hand-counting
characters -- easy to get wrong by one or two characters per line, and
easy to not notice by eye.

**Fix:** compute padding programmatically with `printf "%-*s"` instead of
manual spaces, and verify by *measuring*, not eyeballing:
```bash
grep '^  |' output.txt | awk '{print length, $0}'
```
Every line should report the identical length. This is worth doing as an
actual automated check, not a visual spot check -- the bug was invisible
in casual review and only obvious once measured.


---

# Added with the windowed desktop

## The live clock, solved properly

The notes above explain why `read -t` and a background process both broke
input. The real fix was to stop making the shell do timing: `kbmouse` sends a
`t` event every second and the shell just reads lines. See
[`INPUT-HELPER.md`](INPUT-HELPER.md). The `dd`-per-keystroke editor from
earlier versions is gone for the same reason.

## Late kernel messages scribble on the desktop

**Symptom:** a few seconds after boot a line like `random: crng init done`
appeared at the bottom of the screen and scrolled the whole desktop up one
row, leaving stray pointer blocks behind.

**Cause:** the kernel keeps printing to the VT console after `/init` starts.

**Fix:** `echo '1 1 1 1' > /proc/sys/kernel/printk` in `/init`, before the
desktop starts. Boot messages still show; later ones do not.

## `grub-mkrescue` will not overwrite an existing ISO

Re-running `grub-mkrescue -o foo.iso dir/` when `foo.iso` exists did **not**
rebuild it, and with the output piped through `tail` it looked like it
worked. A "fixed" ISO shipped that was byte-for-byte the old one. Always
`rm -f foo.iso` first, and check the file's timestamp.

## Testing a VT-mode program without real hardware

The serial console path (`-nographic`) cannot show a `/dev/vcsa` pointer
because the screen being drawn on is the VGA console, not the serial port.
Test the way real hardware runs instead:

```
qemu-system-x86_64 -m 256 -cdrom test.iso -display none -vga std \
    -monitor unix:/tmp/q.sock,server,nowait
```

then over the monitor socket: `sendkey ctrl-n`, `mouse_move dx dy`,
`mouse_button 1` / `mouse_button 0`, and `screendump /tmp/x.ppm`. Keep each
`mouse_move` to about 5 counts per step, or the helper's acceleration makes
the pointer land in the wrong cell. Allow ~30 seconds for boot under TCG.

## Dropped: Install to Disk

An earlier version could partition a disk, make an ext2 filesystem, and
install GRUB with pure `dd`. It worked in QEMU but real-hardware booting of
the result never got confirmed, and the bundled kernel + initramfs pushed the
ISO to 77MB (which also made USB writes less reliable). It was removed in
favour of the desktop. Lessons kept: a superfloppy has no GRUB embedding gap,
`core.img` needs ~2048 sectors of gap, and UUID-based `search` in `core.img`
is not portable (use a filesystem label).
