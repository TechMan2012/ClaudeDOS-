/*
 * kbmouse -- input helper for ClaudeDOS Desktop.
 *
 * Merges the keyboard (/dev/tty, raw) and the mouse (/dev/input/mice) into
 * one line-based event stream on stdout for the sh desktop:
 *
 *   p<ch>        printable key             c<N>   control byte N (decimal)
 *   n<NAME>      UP DOWN LEFT RIGHT DEL ESC
 *   d<col> <row> 1   left button pressed   (1-based text cells)
 *   x<col> <row> 1   ...it was a double-click (sent right after its "d")
 *   g<col> <row> 1   moved with button held (rate limited)
 *   u<col> <row> 1   left button released
 *   r<col> <row> 1   right button pressed
 *   w<col> <row> <+1|-1>   wheel (+1 = scroll down, -1 = scroll up)
 *   t            one-second tick
 *
 * The pointer is a software cursor: a glyph written straight into console
 * screen memory through /dev/vcsa. Its shape follows the context:
 *   normal arrow, left-right arrows over a window's right edge/corner,
 *   up-down arrows over the bottom edge, and a spinner while the desktop
 *   is busy (/tmp/busy exists). Window rectangles come from /tmp/wins,
 *   one "x y w h" line per window, bottom-most first.
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

#ifndef DBLMS
#define DBLMS 600   /* double-click window in ms (tests build with a larger one) */
#endif
#define CELL_W 7   /* mouse counts per text column */
#define CELL_H 14  /* mouse counts per text row    */

/* VGA/CP437 glyph indices used for the pointer */
#define G_ARROW 0x10   /* ► */
#define G_LR    0x1D   /* ↔ */
#define G_UD    0x12   /* ↕ */

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

/* ---------------- window rectangles (for pointer shape) ---------------- */
#define MAXW 16
static int wx[MAXW], wy[MAXW], ww[MAXW], wh[MAXW], nwin;

static void load_wins(void)
{
    FILE *f = fopen("/tmp/wins", "r");
    int a, b, c, d;
    nwin = 0;
    if (!f) return;
    while (nwin < MAXW && fscanf(f, "%d %d %d %d", &a, &b, &c, &d) == 4) {
        wx[nwin] = a; wy[nwin] = b; ww[nwin] = c; wh[nwin] = d;
        nwin++;
    }
    fclose(f);
}

/* pick the glyph for the pointer at 1-based (col,row) */
static int spin;
static uint8_t shape_for(int col, int row)
{
    int i;
    if (access("/tmp/busy", F_OK) == 0) {
        static const char fr[4] = { '|', '/', '-', '\\' };
        return (uint8_t)fr[spin++ & 3];
    }
    load_wins();
    for (i = nwin - 1; i >= 0; i--) {
        int x = wx[i], y = wy[i], w = ww[i], h = wh[i];
        if (col < x || col >= x + w || row < y || row >= y + h) continue;
        if (col == x + w - 1 && row > y)                 return G_LR;   /* right edge + corner */
        if (row == y + h - 1 && col > x)                 return G_UD;   /* bottom edge */
        return G_ARROW;
    }
    return G_ARROW;
}

/* ---------------- software pointer (vcsa) ---------------- */
static int pdrawn, pcol, prow;
static uint8_t pch, pat, sgl, sat;

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

/* foreground colour that stays readable on the cell's background */
static uint8_t pointer_attr(uint8_t at)
{
    switch ((at >> 4) & 7) {
    case 1:  return (at & 0xF0) | 0x0E;   /* blue  -> yellow */
    case 3:
    case 7:  return (at & 0xF0) | 0x00;   /* cyan, gray -> black */
    default: return (at & 0xF0) | 0x0F;   /* black, red, ... -> white */
    }
}

static void pointer_erase(void)
{
    uint8_t ch, at;
    if (!pdrawn) return;
    /* only restore if the cell still holds our pointer */
    if (cell_get(pcol, prow, &ch, &at) == 0 && ch == sgl && at == sat)
        cell_put(pcol, prow, pch, pat);
    pdrawn = 0;
}

static void pointer_show(int c, int r)
{
    uint8_t ch, at;
    if (cell_get(c, r, &ch, &at) != 0) return;
    pch = ch;
    pat = at;
    sgl = shape_for(c + 1, r + 1);
    sat = pointer_attr(at);
    cell_put(c, r, sgl, sat);
    pcol = c;
    prow = r;
    pdrawn = 1;
}

/* keep the pointer correct: repaint if overwritten, reshape if context changed */
static void pointer_refresh(int c, int r)
{
    uint8_t ch, at, want;
    if (pdrawn && pcol == c && prow == r &&
        cell_get(c, r, &ch, &at) == 0 && ch == sgl && at == sat) {
        want = shape_for(c + 1, r + 1);
        if (want != sgl) { sgl = want; cell_put(c, r, sgl, sat); }
        return;
    }
    pdrawn = 0;
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
static int mlen, plen = 3;
static int mx, my, mcol, mrow, mbtn, mrbtn, mactive;
static int pending_drag;
static long last_drag, last_click;
static int click_col = -1, click_row = -1;

static int accel(int d)
{
    int a = d < 0 ? -d : d;
    if (a > 12) a *= 3; else if (a > 5) a *= 2;
    return d < 0 ? -a : a;
}

static void mouse_packets(void)
{
    while (mlen >= plen) {
        int dx, dy, dz = 0, b, rb, ncol, nrow, moved;
        if (!(mbuf[0] & 8)) {            /* out of sync: drop a byte */
            memmove(mbuf, mbuf + 1, --mlen);
            continue;
        }
        dx = (int8_t)mbuf[1];
        dy = (int8_t)mbuf[2];
        if (plen == 4) dz = (int8_t)mbuf[3];
        b = mbuf[0] & 1;
        rb = (mbuf[0] >> 1) & 1;
        memmove(mbuf, mbuf + plen, mlen -= plen);

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

        if (dz) emit("w%d %d %d\n", mcol + 1, mrow + 1, dz > 0 ? 1 : -1);

        if (b && !mbtn) {
            long now = now_ms();
            pending_drag = 0;
            emit("d%d %d 1\n", mcol + 1, mrow + 1);
            if (click_col == mcol && click_row == mrow && now - last_click < DBLMS) {
                emit("x%d %d 1\n", mcol + 1, mrow + 1);
                click_col = -1;                 /* no triple-click chains */
            } else {
                click_col = mcol; click_row = mrow; last_click = now;
            }
        } else if (!b && mbtn) {
            if (pending_drag) { emit("g%d %d 1\n", mcol + 1, mrow + 1); pending_drag = 0; }
            emit("u%d %d 1\n", mcol + 1, mrow + 1);
        } else if (b && moved) {
            pending_drag = 1;
        }
        if (rb && !mrbtn) emit("r%d %d 1\n", mcol + 1, mrow + 1);
        mbtn = b;
        mrbtn = rb;
    }
}

/* Ask the kernel's mouse node for IntelliMouse mode (wheel). It answers the
 * "get id" query with 3 when wheel packets (4 bytes) are enabled. */
static void mouse_open(void)
{
    static const uint8_t magic[] = { 0xf3, 200, 0xf3, 100, 0xf3, 80, 0xf2 };
    uint8_t tmp[64];
    ssize_t n;
    struct timespec ts = { 0, 80 * 1000000L };

    plen = 3;
    mlen = 0;
    mfd = open("/dev/input/mice", O_RDWR | O_NONBLOCK);
    if (mfd < 0) mfd = open("/dev/input/mice", O_RDONLY | O_NONBLOCK);
    if (mfd < 0) return;
    if (write(mfd, magic, sizeof magic) == (ssize_t)sizeof magic) {
        nanosleep(&ts, NULL);
        n = read(mfd, tmp, sizeof tmp);
        if (n > 0 && tmp[n - 1] == 0x03) plen = 4;
    }
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
