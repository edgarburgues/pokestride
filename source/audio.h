#pragma once

#include <stdint.h>

/* Initialize NDSP audio for beeper emulation. */
void audioInit(void);

/* Shut down audio. */
void audioExit(void);

/* Update beeper state from Timer W registers.
 * Call once per frame from the main loop.
 * Reads GRA to determine frequency, volume setting from RAM. */
void audioUpdate(uint16_t graValue, uint8_t volume, bool timerActive);

/* Returns true if NDSP initialized successfully. */
bool audioIsReady(void);
