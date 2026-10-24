# Building ClaudeDOS

Tested on Ubuntu 24.04, as root (or with `sudo`).

## 1. Dependencies

```bash
sudo apt-get update
sudo apt-get install -y grub-pc-bin grub-efi-amd64-bin grub-common xorriso mtools \
    busybox-static cpio zstd gcc libc6-dev
sudo apt-get download linux-modules-6.8.0-134-generic   # for the .ko files
mkdir mods && dpkg-deb -x linux-modules-6.8.0-134-generic_*.deb mods
```

You also need the matching kernel image, `/boot/vmlinuz-6.8.0-134-generic`
(package `linux-image-6.8.0-134-generic`, or any kernel whose modules you use
below).

## 2. Initramfs skeleton

```bash
mkdir -p build/initramfs && cd build/initramfs
mkdir -p bin sbin etc proc sys dev tmp root lib/modules
cp /bin/busybox bin/busybox
for a in $(bin/busybox --list); do
    case $a in init|linuxrc) continue;; esac
    ln -sf busybox bin/$a
done
```

## 3. Kernel modules

```bash
K=../../mods/lib/modules/6.8.0-134-generic/kernel/drivers
for m in hid/hid hid/usbhid/usbhid hid/hid-generic input/mouse/psmouse \
         ata/libahci ata/ahci usb/storage/usb-storage \
         nvme/host/nvme-auth nvme/host/nvme-core nvme/host/nvme; do
    unzstd -q "$K/$m.ko.zst" -o "lib/modules/$(basename $m).ko"
done
```

Module paths shift slightly between kernel versions; `find mods -name 'hid-generic*'`
is your friend. Everything not listed is built into the kernel.

## 4. The input helper

```bash
gcc -O2 -static -o bin/kbmouse ../../kbmouse.c && strip bin/kbmouse
```

## 5. Scripts

```bash
cp ../../init init
cp ../../dosshell bin/dosshell
cp ../../desktop bin/desktop
chmod +x init bin/dosshell bin/desktop bin/kbmouse
```

## 6. Pack it

```bash
find . | cpio -o -H newc 2>/dev/null | gzip -9 > ../initramfs.img
cd ..
```

## 7. ISO

```bash
mkdir -p iso/boot/grub
cp /boot/vmlinuz-6.8.0-134-generic iso/boot/vmlinuz
cp initramfs.img iso/boot/initramfs.img
cp ../grub.cfg iso/boot/grub/grub.cfg
rm -f claudedos.iso            # grub-mkrescue will NOT overwrite it
grub-mkrescue -o claudedos.iso iso/
```

Keep the kernel command line to `console=tty0`. `console=ttyS0` hangs real
hardware (see `docs/DEBUGGING-NOTES.md`).

## 8. Run it

```bash
qemu-system-x86_64 -m 256 -cdrom claudedos.iso
```

macOS: add `-accel hvf` on Intel Macs, `-accel tcg` on Apple Silicon. Click
inside the window to capture the keyboard and mouse; `Ctrl+Alt+G` (Ctrl+Option+G on
macOS) releases them. For automated tests, see "Testing a VT-mode program
without real hardware" in `docs/DEBUGGING-NOTES.md`.

To write it to a USB stick, use `dd` or Rufus in **DD mode**.
