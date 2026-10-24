# BusyBox applets

The initramfs holds one static `busybox` binary. `bin/<name>` symlinks make
each applet a command. Early versions linked a hand-picked list and silently
broke features whenever an applet was missing (`sed`, `basename`, `dirname`,
`dd`, `kill`, `tr`, `tail`). Now the build links **all** of them:

```bash
for a in $(bin/busybox --list); do
    case $a in init|linuxrc) continue;; esac
    [ -e bin/$a ] || ln -s busybox bin/$a
done
```

That is about 270 symlinks and costs almost nothing, and it makes the
Terminal window and full-screen shell far more useful (`vi`, `top`, `fdisk`,
`mount`, `ifconfig`, ...). The desktop itself relies on: `sh` (ash, with its
builtin `printf`/`read`), `cat`, `date`, `ls`, `cut`, `grep`, `sed`, `tr`,
`tail`, `head`, `wc`, `df`, `uname`, `hostname`, `sleep`, `stty`, `kill`,
`poweroff`, `reboot`, `setsid`, `cttyhack`, `insmod`.
