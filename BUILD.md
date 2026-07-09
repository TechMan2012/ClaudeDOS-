# Building DOS Shell from scratch

Exact, runnable steps. Tested on Ubuntu 24.04. Needs root (for `insmod`-able
kernel module access and `mount --bind`).

## 1. Install build dependencies

```bash
sudo apt-get update
sudo apt-get install -y \
    grub-pc-bin grub-efi-amd64-bin grub-common xorriso mtools \
    busybox-static linux-image-generic
```

`linux-image-generic` is only needed here to get a real `vmlinuz` +
matching `/lib/modules/<version>/` tree to pull the keyboard driver modules
from. If you already have a kernel + modules you trust, you can skip this
and point the paths below at those instead.

## 2. Set up the initramfs skeleton

```bash
mkdir -p build/initramfs
cd build/initramfs
mkdir -p bin sbin etc proc sys dev tmp root lib lib64

cp /bin/busybox bin/busybox
chmod +x bin/busybox

# Every command available at runtime is one of these symlinks.
# See docs/BUSYBOX-APPLETS.md for why each one is here.
for cmd in sh ls cat echo printf mount umount mkdir ps mknod ln clear \
           uname free df dmesg poweroff reboot halt stty vi cp mv rm \
           date sleep cd wc setsid cttyhack insmod grep cut hostname \
           head awk sed kill dd basename dirname; do
    ln -sf busybox "bin/$cmd"
done
```

## 3. Pull the keyboard driver modules

USB keyboards need three kernel modules that a minimal initramfs doesn't
load automatically. Find your kernel version first:

```bash
KVER=$(uname -r)   # or ls /lib/modules/ if building for a different kernel
mkdir -p lib/modules

unzstd -k "/lib/modules/$KVER/kernel/drivers/hid/hid.ko.zst" \
    -o lib/modules/hid.ko
unzstd -k "/lib/modules/$KVER/kernel/drivers/hid/usbhid/usbhid.ko.zst" \
    -o lib/modules/usbhid.ko
unzstd -k "/lib/modules/$KVER/kernel/drivers/hid/hid-generic.ko.zst" \
    -o lib/modules/hid-generic.ko
```

(If your kernel's modules aren't zstd-compressed, drop the `unzstd`/`.zst`
and just `cp` them directly.)

## 4. Add `init` and `dosshell`

Copy this repo's [`init`](../init) to `build/initramfs/init` and
[`dosshell`](../dosshell) to `build/initramfs/bin/dosshell`, then:

```bash
chmod +x init bin/dosshell
```

## 5. Package the initramfs

```bash
find . | cpio -o -H newc 2>/dev/null | gzip -9 > ../initramfs.img
cd ..
```

## 6. Stage the ISO contents

```bash
mkdir -p iso/boot/grub
cp /boot/vmlinuz-$(uname -r) iso/boot/vmlinuz
cp initramfs.img iso/boot/initramfs.img
```

Copy this repo's [`grub.cfg`](../grub.cfg) to `iso/boot/grub/grub.cfg`.

**Important:** the kernel command line must be `console=tty0` only -- do
**not** add `console=ttyS0`. See `docs/DEBUGGING-NOTES.md` for why this
specifically hangs real hardware (it doesn't affect QEMU testing, which is
exactly why it's an easy trap).

## 7. Build the ISO

```bash
grub-mkrescue -o dos-shell.iso iso/
```

That's it -- `dos-shell.iso` is a hybrid BIOS+UEFI bootable image, roughly
28MB, ready to `dd` to a USB drive or mount in a VM.

## Testing before you burn a USB drive

```bash
qemu-system-x86_64 -m 512 -cdrom dos-shell.iso -nographic -serial stdio
```

Note: for interactive testing via QEMU's serial console specifically (not
needed for real hardware or a normal VM window), you'd temporarily add
`console=ttyS0` back to `grub.cfg` -- just remember to remove it again
before shipping, per the warning in step 6.
