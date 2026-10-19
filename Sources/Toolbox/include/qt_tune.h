/* QuickTime Music Architecture's Tune Player and Note Allocator (qt_tune.c). */
#ifndef QT_TUNE_H
#define QT_TUNE_H

/* Register the 'tune' and 'nota' components: a new run (components_reset). */
void qt_tune_register(void);

/* Keep queued tunes playing and deliver their callbacks: a safe point. */
void qt_tune_pump(void);

#endif
