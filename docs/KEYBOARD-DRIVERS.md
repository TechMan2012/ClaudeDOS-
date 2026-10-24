# Keyboard and pointer drivers

A minimal initramfs does not autoload anything. Modules are `insmod`'d by
hand in `/init` from `/lib/modules/*.ko` (decompressed from the kernel's
`.ko.zst`). Everything else (USB host controllers, SCSI/`sd`, ATA_PIIX,
`mousedev`, `evdev`, `i8042`/`atkbd`) is built into the Ubuntu kernel.

| Module | Why |
|---|---|
| `hid`, `usbhid`, `hid-generic` | USB keyboards and mice. **All three** are needed: `usbhid` is only the transport, `hid-generic` is what binds a device to input. Seeing the keyboard in `dmesg` is not proof it works. |
| `psmouse` | PS/2 trackpoints and touchpads (ThinkPads, many laptops). No dependencies. |
| `libahci`, `ahci`, `usb-storage` | SATA and USB disks (visible to the Terminal's `mount`) |
| `nvme-auth`, `nvme-core`, `nvme` | NVMe disks |

Internal laptop keyboards use `atkbd`, which is built in, so they work with no
module at all. That is why only the *external* USB keyboard was dead until
`hid-generic` was added.
