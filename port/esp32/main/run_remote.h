/*
 * Normal mode with remote reporting (CONFIG_BMS_REMOTE). See run_remote.c.
 */
#ifndef RUN_REMOTE_H
#define RUN_REMOTE_H

#define BMS_FW_VERSION "2.0.0"

/* Never returns: loops, or enters deep sleep (and the next boot comes back). */
void run_remote(void);

#endif /* RUN_REMOTE_H */
