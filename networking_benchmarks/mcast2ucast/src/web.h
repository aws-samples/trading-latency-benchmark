#ifndef MCAST2UCAST_WEB_H
#define MCAST2UCAST_WEB_H

#include <stdint.h>

/* Start the web dashboard on a background thread. Returns 0 on success. */
int web_start(uint16_t port);

/* Stop the web server and join the thread. */
void web_stop(void);

#endif
