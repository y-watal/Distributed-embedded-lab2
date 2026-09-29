#ifndef BLINKERS_H
#define BLINKERS_H

enum indicator_mode {
    INDICATOR_OFF,
    INDICATOR_LEFT,
    INDICATOR_RIGHT
};

void blinkers_start(void);
enum indicator_mode blinkers_get_mode(void);

#endif