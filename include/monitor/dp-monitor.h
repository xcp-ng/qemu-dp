#ifndef DP_MONITOR_H
#define DP_MONITOR_H

#include "chardev/char.h"

void dp_monitor_init(Chardev *chr);
void dp_monitor_destroy(void);

#endif
