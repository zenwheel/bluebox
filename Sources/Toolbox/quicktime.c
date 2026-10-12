/*
 * QuickTime: enough of the Movie Toolbox that applications which need it
 * start.  Movies open but play as zero length (finished at once), so an
 * application showing an intro or a clip carries on past it.  Decoding the
 * classic codecs is future work.
 */
#include "qd_internal.h"
#include "tb_internal.h"

enum { noMovieFound = -2048 };

/* A movie: its box (8 bytes) and whether it is active. */
#define MOVIE_SIZE 10

static int16_t movies_error;

int16_t tb_EnterMovies(void) { return movies_error = noErr; }
void tb_ExitMovies(void) {}

int16_t tb_OpenMovieFile(gaddr spec, gaddr refnum, uint8_t perm)
{
    int16_t rn = tb_FSpOpenDF(spec, perm, refnum);
    if (rn != noErr)
        wr16(refnum, 0);
    return movies_error = rn;
}

int16_t tb_CloseMovieFile(int16_t refnum)
{
    gaddr pb = tb_NewPtr(IOParam_SIZEOF, false, true);
    wr16(pb + IOParam_ioRefNum_off, (uint16_t)refnum);
    int16_t err = tb_PBClose(pb, false);
    tb_DisposePtr(pb);
    return err;
}

int16_t tb_NewMovieFromFile(gaddr moviep, int16_t refnum, gaddr resid, gaddr resname, int16_t flags,
                            gaddr changed)
{
    gaddr m = tb_NewHandle(MOVIE_SIZE, false, true);
    if (!m) {
        wr32(moviep, 0);
        return movies_error = memFullErr;
    }
    qrect_put(rd32(m), (qrect){ 0, 0, 240, 320 });
    wr32(moviep, m);
    if (changed)
        wr8(changed, 0);
    return movies_error = noErr;
}

void tb_DisposeMovie(gaddr movie)
{
    if (movie)
        tb_DisposeHandle(movie);
}

void tb_MoviesTask(gaddr movie, int32_t ms) {}
int16_t tb_PrerollMovie(gaddr movie, int32_t time, int32_t rate) { return noErr; }
void tb_SetMovieActive(gaddr movie, int8_t active) {}
void tb_StartMovie(gaddr movie) {}
void tb_StopMovie(gaddr movie) {}
void tb_GoToBeginningOfMovie(gaddr movie) {}
void tb_SetMovieGWorld(gaddr movie, gaddr port, gaddr gdh) {}
int16_t tb_UpdateMovie(gaddr movie) { return noErr; }
int16_t tb_GetMovieVolume(gaddr movie) { return 0x0100; }
int8_t tb_IsMovieDone(gaddr movie) { return 1; }
int32_t tb_GetMoviePreferredRate(gaddr movie) { return 0x00010000; }

void tb_GetMovieBox(gaddr movie, gaddr box)
{
    if (movie && rd32(movie))
        gmemcpy(box, rd32(movie), 8);
    else
        qrect_put(box, (qrect){ 0, 0, 0, 0 });
}

void tb_SetMovieBox(gaddr movie, gaddr box)
{
    if (movie && rd32(movie))
        gmemcpy(rd32(movie), box, 8);
}

gaddr tb_NewMovieController(gaddr movie, gaddr rect, int32_t flags) { return tb_NewHandle(4, false, true); }

void tb_DisposeMovieController(gaddr mc)
{
    if (mc)
        tb_DisposeHandle(mc);
}
