/*
 * kbmouse -- input helper for ClaudeDOS Desktop.
 *
 * Merges the keyboard (/dev/tty, raw) and the mouse (/dev/input/mice, PS/2
 * protocol) into one line-based event stream on stdout for the sh desktop:
 *
 *   p<ch>        printable key             c<N>   control byte N (decimal)
 *   n<NAME>      UP DOWN LEFT RIGHT DEL ESC
 *   d<col> <row> 1   left button pressed   (1-based text cells)
 *   g<col> <row> 1   moved with button held (rate limited)
 *   u<col> <row> 1   left button released
 *   t            one-second tick
 *
 * The mouse pointer is a software cursor: a cell on the text console whose
 * colours are inverted, drawn straight into screen memory via /dev/vcsa.
 *
 * SIGUSR1 = pause (restore tty, erase pointer, stop); SIGCONT = resume.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define CELL_W 7   /* mouse counts per text column */
#define CELL_H 14  /* mouse counts per text row    */

static int tty = -1, mfd = -1, vfd = -1;
static struct termios orig, rawt;
static int have_orig;
static volatile sig_atomic_t want_pause, want_quit;
static int rows = 25, cols = 80;

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* ---------------- output ---------------- */
static void emit(const char *fmt, ...)
{
    char buf[96];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (write(1, buf, n) < 0 && errno == EPIPE) want_quit = 1;
}

/* ---------------- tty ---------------- */
static void tty_raw(void)
{
    if (!have_orig) {
        if (tcgetattr(tty, &orig) == 0) have_orig = 1;
    }
    if (!have_orig) return;
    rawt = orig;
    cfmakeraw(&rawt);
    rawt.c_cc[VMIN] = 1;
    rawt.c_cc[VTIME] = 0;
    tcsetattr(tty, TCSANOW, &rawt);
}

static void tty_restore(void)
{
    if (have_orig) tcsetattr(tty, TCSANOW, &orig);
}

/* ---------------- software pointer (vcsa) ---------------- */
static int pdrawn, pcol, prow;
static uint8_t pch, pat, pinv;

static int cell_get(int c, int r, uint8_t *ch, uint8_t *at)
{
    uint8_t b[2];
    if (vfd < 0 || c < 0 || r < 0 || c >= cols || r >= rows) return -1;
    if (pread(vfd, b, 2, 4 + 2 * ((off_t)r * cols + c)) != 2) return -1;
    *ch = b[0];
    *at = b[1];
    return 0;
}

static int cell_put(int c, int r, uint8_t ch, uint8_t at)
{
    uint8_t b[2] = { ch, at };
    if (vfd < 0) return -1;
    return pwrite(vfd, b, 2, 4 + 2 * ((off_t)r * cols + c)) == 2 ? 0 : -1;
}

static void pointer_erase(void)
{
    uint8_t ch, at;
    if (!pdrawn) return;
    /* only restore if the cell still holds our inverted pointer */
    if (cell_get(pcol, prow, &ch, &at) == 0 && ch == pch && at == pinv)
        cell_put(pcol, prow, pch, pat);
    pdrawn = 0;
}

static void pointer_show(int c, int r)
{
    uint8_t ch, at;
    if (cell_get(c, r, &ch, &at) != 0) return;
    pch = ch;
    pat = at;
    pinv = at ^ 0x77;
    cell_put(c, r, pch, pinv);
    pcol = c;
    prow = r;
    pdrawn = 1;
}

/* redraw the pointer if the desktop painted over it */
static void pointer_refresh(int c, int r)
{
    uint8_t ch, at;
    if (pdrawn && pcol == c && prow == r &&
        cell_get(c, r, &ch, &at) == 0 && ch == pch && at == pinv)
        return;
    pointer_erase();
    pointer_show(c, r);
}

/* ---------------- keyboard ---------------- */
static uint8_t kbuf[256];
static int klen, kpos;

static int kb_fill(int timeout)
{
    struct pollfd p;
    ssize_t n;
    if (kpos < klen) return 1;
    p.fd = tty; p.events = POLLIN; p.revents = 0;
    if (poll(&p, 1, timeout) <= 0) return 0;
    n = read(tty, kbuf, sizeof kbuf);
    if (n <= 0) return 0;
    klen = (int)n;
    kpos = 0;
    return 1;
}

static void parse_keys(void)
{
    while (kpos < klen) {
        int c = kbuf[kpos++];
        if (c == 27) {
            int c2, c3, fin, pn = 0;
            char par[8];
            if (!kb_fill(40)) { emit("nESC\n"); continue; }
            c2 = kbuf[kpos];
            if (c2 != '[' && c2 != 'O') { emit("nESC\n"); continue; }
            kpos++;
            if (!kb_fill(40)) { emit("nESC\n"); continue; }
            c3 = kbuf[kpos++];
            if (c2 == 'O') continue;               /* ESC O x : ignore */
            if (c3 == '[') { if (kb_fill(40)) kpos++; continue; } /* F1-F5 */
            fin = c3;
            while (fin >= '0' && fin <= ';') {
                if (pn < 7) par[pn++] = (char)fin;
                if (!kb_fill(40)) { fin = 0; break; }
                fin = kbuf[kpos++];
            }
            par[pn] = 0;
            switch (fin) {
            case 'A': emit("nUP\n"); break;
            case 'B': emit("nDOWN\n"); break;
            case 'C': emit("nRIGHT\n"); break;
            case 'D': emit("nLEFT\n"); break;
            case '~': if (!strcmp(par, "3")) emit("nDEL\n"); break;
            default: break;
            }
        } else if (c >= 32 && c < 127) {
            emit("p%c\n", c);
        } else if (c < 32 || c == 127) {
            emit("c%d\n", c);
        }
        /* bytes >= 128 (UTF-8 etc.) are dropped */
    }
    klen = kpos = 0;
}

/* ---------------- mouse ---------------- */
static uint8_t mbuf[64];
static int mlen;
static int mx, my, mcol, mrow, mbtn, mactive;
static int pending_drag;
static long last_drag;

static int accel(int d)
{
    int a = d < 0 ? -d : d;
    if (a > 12) a *= 3; else if (a > 5) a *= 2;
    return d < 0 ? -a : a;
}

static void mouse_packets(void)
{
    while (mlen >= 3) {
        int dx, dy, b, ncol, nrow, moved;
        if (!(mbuf[0] & 8)) {            /* out of sync: drop a byte */
            memmove(mbuf, mbuf + 1, --mlen);
            continue;
        }
        dx = (int8_t)mbuf[1];
        dy = (int8_t)mbuf[2];
        b = mbuf[0] & 1;
        memmove(mbuf, mbuf + 3, mlen -= 3);

        mx += accel(dx);
        my -= accel(dy);
        if (mx < 0) mx = 0;
        if (my < 0) my = 0;
        if (mx > cols * CELL_W - 1) mx = cols * CELL_W - 1;
        if (my > rows * CELL_H - 1) my = rows * CELL_H - 1;
        ncol = mx / CELL_W;
        nrow = my / CELL_H;
        moved = (ncol != mcol || nrow != mrow);
        mcol = ncol; mrow = nrow;
        mactive = 1;
        if (moved) { pointer_erase(); pointer_show(mcol, mrow); }

        if (b && !mbtn) {
            pending_drag = 0;
            emit("d%d %d 1\n", mcol + 1, mrow + 1);
        } else if (!b && mbtn) {
            if (pending_drag) { emit("g%d %d 1\n", mcol + 1, mrow + 1); pending_drag = 0; }
            emit("u%d %d 1\n", mcol + 1, mrow + 1);
        } else if (b && moved) {
            pending_drag = 1;
        }
        mbtn = b;
    }
}

static void mouse_open(void)
{
    mfd = open("/dev/input/mice", O_RDONLY | O_NONBLOCK);
    mlen = 0;
}

/* ---------------- signals ---------------- */
static void on_usr1(int s) { (void)s; want_pause = 1; }
static void on_term(int s) { (void)s; want_quit = 1; }

int main(void)
{
    struct pollfd fds[3];
    long t_tick, t_ptr, t_mopen;
    uint8_t hdr[4];
    FILE *pf;

    signal(SIGPIPE, SIG_IGN);
    signal(SIGUSR1, on_usr1);
    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);

    tty = open("/dev/tty", O_RDWR);
    if (tty < 0) tty = 0;
    vfd = open("/dev/vcsa", O_RDWR);
    if (vfd >= 0 && pread(vfd, hdr, 4, 0) == 4 && hdr[0] >= 10 && hdr[1] >= 20) {
        rows = hdr[0];
        cols = hdr[1];
    }
    mouse_open();
    mx = cols * CELL_W / 2;
    my = rows * CELL_H / 2;
    mcol = mx / CELL_W;
    mrow = my / CELL_H;

    pf = fopen("/tmp/kbm.pid", "w");
    if (pf) { fprintf(pf, "%d\n", (int)getpid()); fclose(pf); }

    tty_raw();
    t_tick = t_ptr = t_mopen = now_ms();

    while (!want_quit) {
        long now;
        int n, nf = 0, mi = -1;

        if (want_pause) {
            pointer_erase();
            tty_restore();
            want_pause = 0;
            kill(getpid(), SIGSTOP);      /* parked until SIGCONT */
            tty_raw();
            tcflush(tty, TCIFLUSH);
            mlen = 0; klen = kpos = 0;
            pdrawn = 0;
        }

        fds[nf].fd = tty; fds[nf].events = POLLIN; fds[nf].revents = 0; nf++;
        if (mfd >= 0) { mi = nf; fds[nf].fd = mfd; fds[nf].events = POLLIN; fds[nf].revents = 0; nf++; }
        fds[nf].fd = 1; fds[nf].events = 0; fds[nf].revents = 0; nf++;

        n = poll(fds, nf, 40);
        if (n < 0 && errno != EINTR) break;

        if (n > 0) {
            if (fds[0].revents & POLLIN) {
                ssize_t r = read(tty, kbuf, sizeof kbuf);
                if (r > 0) { klen = (int)r; kpos = 0; parse_keys(); }
            }
            if (mi >= 0 && (fds[mi].revents & (POLLIN | POLLERR | POLLHUP))) {
                ssize_t r = read(mfd, mbuf + mlen, sizeof mbuf - mlen);
                if (r > 0) { mlen += (int)r; mouse_packets(); }
                else if (r == 0 || (r < 0 && errno != EAGAIN && errno != EINTR)) {
                    close(mfd); mfd = -1;
                }
            }
            if (fds[nf - 1].revents & (POLLERR | POLLHUP)) break;  /* reader gone */
        }

        now = now_ms();
        if (pending_drag && now - last_drag >= 70) {
            emit("g%d %d 1\n", mcol + 1, mrow + 1);
            pending_drag = 0;
            last_drag = now;
        }
        if (now - t_tick >= 1000) { emit("t\n"); t_tick = now; }
        if (mactive && now - t_ptr >= 100) { pointer_refresh(mcol, mrow); t_ptr = now; }
        if (mfd < 0 && now - t_mopen >= 2000) { mouse_open(); t_mopen = now; }
    }

    pointer_erase();
    tty_restore();
    return 0;
}
