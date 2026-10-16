/*
 * PPCHello: the first PowerPC probe.  Computes integer and floating-point
 * results (the host computes the same from this file, compiled natively),
 * writes them to "PPC Results" beside itself, opens a window, draws a
 * line of text and quits.
 */
#include <Quickdraw.h>
#include <Fonts.h>
#include <Windows.h>
#include <Files.h>
#include <Events.h>
#include <string.h>

static char out[2048];
static int len;

static void put(const char *s)
{
    while (*s && len < (int)sizeof out - 1)
        out[len++] = *s++;
}

static void puthex(unsigned long v)
{
    char b[12];
    int i;
    b[0] = ' ';
    for (i = 0; i < 8; i++)
        b[1 + i] = "0123456789ABCDEF"[(v >> (28 - 4 * i)) & 15];
    b[9] = 0;
    put(b);
}

volatile unsigned long seed = 12345;

static unsigned long next(void)
{
    seed = seed * 1103515245UL + 12345UL;
    return seed;
}

static void compute(void)
{
    int i;
    unsigned long acc = 0;
    long long big = 1;
    double d = 1.0;
    float f = 0.5f;
    put("int");
    for (i = 0; i < 64; i++) {
        unsigned long a = next(), b = next() | 1;
        acc += a / b + (a % b) * 3 + (a >> (i & 31)) + ((long)a >> (b & 31)) + (a * b);
        acc ^= (a << (i & 15)) | (b >> 3);
        big = big * 3 + (long long)(long)a;
    }
    puthex(acc);
    puthex((unsigned long)(big >> 32));
    puthex((unsigned long)big);
    put("\rfloat");
    for (i = 1; i < 40; i++) {
        d = d * 1.5 + 1.0 / i - (double)(long)(d / 7);
        f = f * 1.25f + (float)i / 3.0f;
    }
    {
        union { double d; unsigned long w[2]; } u;
        union { float f; unsigned long w; } v;
        u.d = d;
        v.f = f;
        puthex(u.w[0]);
        puthex(u.w[1]);
        puthex(v.w);
        puthex((unsigned long)(long)(d / 1e6));
    }
    put("\r");
}

int main(void)
{
    WindowPtr w;
    Rect r;
    FSSpec spec;
    short ref;
    long n;

    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitCursor();

    compute();
    if (FSMakeFSSpec(0, 0, "\pPPC Results", &spec) == fnfErr || 1) {
        FSpDelete(&spec);
        FSpCreate(&spec, 'ttxt', 'TEXT', 0);
        if (FSpOpenDF(&spec, fsRdWrPerm, &ref) == noErr) {
            n = len;
            FSWrite(ref, &n, out);
            FSClose(ref);
        }
    }

    SetRect(&r, 40, 60, 360, 160);
    w = NewWindow(NULL, &r, "\pPowerPC", true, documentProc, (WindowPtr)-1, false, 0);
    SetPort(w);
    MoveTo(20, 40);
    DrawString("\pHello from PowerPC code");
    return 0;
}
