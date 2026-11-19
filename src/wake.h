/*
 * wake.h - interruptible sleep.
 *
 * The main loop spends almost all of its life here.  The control thread (or a
 * signal handler) can cut the sleep short, so a "stop"/"mode" command takes
 * effect immediately instead of after the next poll interval.
 */
#ifndef GTAVI_WAKE_H
#define GTAVI_WAKE_H

#include <stdbool.h>

bool wake_init(void);
void wake_signal(void);

/* Returns 1 when signalled, 0 on timeout, -1 on error. */
int wake_wait(int timeout_ms);

void wake_close(void);

#endif /* GTAVI_WAKE_H */
