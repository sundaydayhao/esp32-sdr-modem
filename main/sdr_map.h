#pragma once
#include "adsb.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef struct
{
    double latitude, longitude, span;
} sdr_map_view_t;
typedef struct {const char *name; double latitude, longitude; unsigned rank;} sdr_map_place_t;
const sdr_map_place_t *sdr_map_places(size_t *count);
const sdr_map_place_t *sdr_map_nearest(double latitude,double longitude,double *distance_km);
bool sdr_map_has_detail(const sdr_map_view_t *view);
double sdr_map_meters_per_pixel(const sdr_map_view_t *view,int width);
bool sdr_map_project(const sdr_map_view_t *view, int width, int height, double latitude, double longitude, int *x,
                     int *y);
void sdr_map_fit(sdr_map_view_t *view, const adsb_aircraft_t *aircraft, size_t count);
void sdr_map_zoom(sdr_map_view_t *view, double factor);
/* Caller owns the tightly packed RGB565 buffer. No allocation or I/O. */
void sdr_map_draw(uint16_t *pixels, int width, int height, const sdr_map_view_t *view);
void sdr_map_marker(uint16_t *pixels, int width, int height, int x, int y, float heading, bool selected);
