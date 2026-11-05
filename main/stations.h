#ifndef STATIONS_H
#define STATIONS_H

#include <stddef.h>

typedef struct {
    const char *name;
    const char *url;
} radio_station_t;

extern const radio_station_t g_radio_stations[];
extern const size_t g_radio_station_count;

#endif /* STATIONS_H */
