/*
 * species_names.h - national-dex species names (Gen I-IV, indices 0..493).
 */
#ifndef POKESTRIDE_SPECIES_NAMES_H
#define POKESTRIDE_SPECIES_NAMES_H

#include <stdint.h>

/* Returns "Pikachu" for 25, NULL for species == 0 or > 493. The returned
 * string is owned by a static const table; caller must not free it. */
const char *species_name(uint16_t species);

#endif
