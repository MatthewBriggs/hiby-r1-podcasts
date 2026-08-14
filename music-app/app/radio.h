#ifndef RADIO_H
#define RADIO_H
#include "library.h"

#define RADIO_MAX 32
typedef struct {
    char name[LIB_NAME_LEN];
    char url[512];
} radio_station_t;

/* Loads /usr/data/radio_stations.conf, writing a starter file if absent.
 * Returns how many stations are available. */
int radio_load(radio_station_t *out, int max);
#endif
