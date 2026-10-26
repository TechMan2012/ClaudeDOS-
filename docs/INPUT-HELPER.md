# The input helper (`kbmouse`)

## Why it exists

A Linux text console has no mouse reporting, and a shell script can only wait
on one file at a time. Earlier versions read the keyboard with
`dd bs=1 count=1` under `stty min 0 time 10`. That worked for a keyboard but
could not also watch `/dev/input/mice`, and every keystroke cost a fork.

`kbmouse` (about 300 lines of C, static, ~700KB) sits in front of the desktop:

```
/dev/tty (raw) --+
                 +--> kbmouse --pipe--> dosshell (sh, `read -r` per event)
/dev/input/mice -+
```

`desktop` runs `kbmouse | dosshell` in a loop. The shell now blocks on a plain
`read`, which is a builtin (no forks), and gets keys, mouse, and a clock tick
from one place.

## Event stream

One line per event on stdout:

| Line | Meaning |
|---|---|
| `p<ch>` | printable key (`pa`, `p `, `p*`) |
| `c<N>` | control byte, decimal (`c13` Enter, `c127` Backspace, `c19` Ctrl+S) |
| `nUP` `nDOWN` `nLEFT` `nRIGHT` `nDEL` `nESC` | named keys |
| `d<col> <row> 1` | left button down (1-based text cells) |
| `g<col> <row> 1` | moved while held (rate-limited to ~14/s) |
| `u<col> <row> 1` | left button up |
| `t` | one-second tick (drives the clock) |
| `f<col> <row>` | fast tick, ~10/s, only while the desktop keeps `/tmp/fast` present (Pong). Carries the pointer cell, `0 0` if no pointer yet |

Escape sequences are parsed in C with a 40ms timeout, so a lone `Esc` is
distinguishable from the start of an arrow key. That is the thing the shell
could not do reliably.

## Mouse

`/dev/input/mice` is the kernel's merged mouse node (`mousedev`, built into
the Ubuntu kernel). It speaks 3-byte PS/2 packets for every pointing device.
USB mice arrive through `usbhid`/`hid-generic`; trackpoints and touchpads
need `psmouse.ko`, the only extra module this feature adds. Relative counts
are accumulated and mapped to text cells (7 counts per column, 14 per row,
with mild acceleration).

## The pointer

There is no hardware cursor in text mode, so the pointer is a cell with its
colours inverted (`attr ^ 0x77`), written directly into screen memory through
`/dev/vcsa`. Because the shell redraws windows without telling the helper,
the helper:

- only restores a cell if it still holds exactly the inverted pointer it drew
  (otherwise the desktop already painted over it), and
- re-checks the pointer cell every 100ms and redraws it if it was overwritten.

## Pausing for the full-screen shell

The real shell needs the tty to itself. `dosshell` sends `SIGUSR1`; the helper
erases the pointer, restores the tty modes, and stops itself (`SIGSTOP`). When
the shell exits, `dosshell` sends `SIGCONT` and the helper puts the tty back
in raw mode. Window state lives in the shell process, so nothing is lost.
The pid is in `/tmp/kbm.pid`.

## Failure modes

- No mouse / no `/dev/vcsa`: keyboard-only, no pointer drawn. The helper
  retries opening the mouse every 2 seconds, so a hot-plugged USB mouse works.
- If `dosshell` exits, `kbmouse` notices the closed pipe and quits, and
  `desktop` restarts both.
