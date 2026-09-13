#include "sdr_map.h"
#include "sdr_map_data.h"
#include <math.h>
#include <stdlib.h>
#include "sdr_map_region.inc"
const sdr_map_place_t *sdr_map_places(size_t *count){if(count)*count=sizeof(regional_places)/sizeof(regional_places[0]);return regional_places;}
bool sdr_map_has_detail(const sdr_map_view_t *v){return v&&v->latitude>=22.40&&v->latitude<=22.85&&v->longitude>=113.75&&v->longitude<=114.45;}
double sdr_map_meters_per_pixel(const sdr_map_view_t *v,int w){return v&&w>0?v->span*111320*cos(v->latitude*3.141592653589793/180)/w:0;}
const sdr_map_place_t *sdr_map_nearest(double lat,double lon,double *km)
{
    if(!isfinite(lat)||!isfinite(lon))return NULL;
    const sdr_map_place_t *best=NULL;double distance=30;
    for(size_t n=0;n<sizeof(regional_places)/sizeof(regional_places[0]);++n){
        const sdr_map_place_t *p=regional_places+n;
        double d=hypot((lat-p->latitude)*111.32,(lon-p->longitude)*111.32*cos(lat*3.141592653589793/180));
        if(d<distance){best=p;distance=d;}
    }
    if(km)*km=distance;
    return best;
}
static double wrap(double n)
{
    n = fmod(n + 180, 360);
    if (n < 0)
        n += 360;
    return n - 180;
}
static double clamp(double n, double lo, double hi)
{
    return fmin(hi, fmax(lo, n));
}
static void point(const sdr_map_view_t *v, int w, int h, double lat, double lon, double *x, double *y)
{
    double scale = w / v->span;
    *x = w / 2.0 + wrap(lon - v->longitude) * scale;
    *y = h / 2.0 - (lat - v->latitude) * scale;
}
bool sdr_map_project(const sdr_map_view_t *v, int w, int h, double lat, double lon, int *x, int *y)
{
    if (!v || !x || !y || w < 1 || h < 1 || !isfinite(lat) || !isfinite(lon) || fabs(lat) > 90 || fabs(lon) > 180 ||
        !isfinite(v->span) || !isfinite(v->latitude) || !isfinite(v->longitude) || v->span <= 0)
        return false;
    double px, py;
    point(v, w, h, lat, lon, &px, &py);
    *x = (int)lround(px);
    *y = (int)lround(py);
    return *x >= 0 && *x < w && *y >= 0 && *y < h;
}
void sdr_map_zoom(sdr_map_view_t *v, double factor)
{
    if (!v || !isfinite(factor) || factor <= 0)
        return;
    v->span = clamp(v->span * factor, .01, 360);
    v->latitude = clamp(v->latitude, -85, 85);
    v->longitude = wrap(v->longitude);
}
void sdr_map_fit(sdr_map_view_t *v, const adsb_aircraft_t *a, size_t count)
{
    bool first = true;
    double ref = 0, lo = 0, hi = 0, bottom = 0, top = 0;
    for (size_t n = 0; n < count; n++)
        if (a[n].has_position && isfinite(a[n].latitude) && isfinite(a[n].longitude) && fabs(a[n].latitude) <= 90 &&
            fabs(a[n].longitude) <= 180)
        {
            if (first)
            {
                ref = a[n].longitude;
                bottom = top = a[n].latitude;
                first = false;
            }
            double d = wrap(a[n].longitude - ref);
            lo = fmin(lo, d);
            hi = fmax(hi, d);
            bottom = fmin(bottom, a[n].latitude);
            top = fmax(top, a[n].latitude);
        }
    if (first)
        return;
    v->longitude = wrap(ref + (lo + hi) / 2);
    v->latitude = (bottom + top) / 2;
    v->span = clamp(fmax((hi - lo) * 1.5, (top - bottom) * 2.5), .2, 360);
    sdr_map_zoom(v, 1);
}
static unsigned code(double x, double y, int w, int h)
{
    return (x < 0 ? 1 : x > w - 1 ? 2 : 0) | (y < 0 ? 4 : y > h - 1 ? 8 : 0);
}
static void line(uint16_t *p, int w, int h, double x0, double y0, double x1, double y1, uint16_t color)
{
    unsigned a = code(x0, y0, w, h), b = code(x1, y1, w, h);
    for (unsigned n = 0; a | b; n++)
    {
        if ((a & b) || n > 8)
            return;
        unsigned c = a ? a : b;
        double x, y;
        if (c & 8)
        {
            x = x0 + (x1 - x0) * (h - 1 - y0) / (y1 - y0);
            y = h - 1;
        }
        else if (c & 4)
        {
            x = x0 + (x1 - x0) * (-y0) / (y1 - y0);
            y = 0;
        }
        else if (c & 2)
        {
            y = y0 + (y1 - y0) * (w - 1 - x0) / (x1 - x0);
            x = w - 1;
        }
        else
        {
            y = y0 + (y1 - y0) * (-x0) / (x1 - x0);
            x = 0;
        }
        if (c == a)
        {
            x0 = x;
            y0 = y;
            a = code(x, y, w, h);
        }
        else
        {
            x1 = x;
            y1 = y;
            b = code(x, y, w, h);
        }
    }
    int x = (int)lround(x0), y = (int)lround(y0), ex = (int)lround(x1), ey = (int)lround(y1);
    int dx = abs(ex - x), sx = x < ex ? 1 : -1, dy = -abs(ey - y), sy = y < ey ? 1 : -1, err = dx + dy;
    for (;;)
    {
        if (x >= 0 && x < w && y >= 0 && y < h)
            p[y * w + x] = color;
        if (x == ex && y == ey)
            break;
        int e = 2 * err;
        if (e >= dy)
        {
            err += dy;
            x += sx;
        }
        if (e <= dx)
        {
            err += dx;
            y += sy;
        }
    }
}
void sdr_map_draw(uint16_t *p, int w, int h, const sdr_map_view_t *v)
{
    if (!p || w < 1 || h < 1 || !v || !isfinite(v->span) || !isfinite(v->latitude) || !isfinite(v->longitude) ||
        v->span <= 0)
        return;
    for (int n = 0; n < w * h; n++)
        p[n] = 0x08E3;
    const double step = v->span > 80 ? 30 : v->span > 20 ? 10 : v->span > 5 ? 2 : v->span > 1 ? 0.5 : v->span>.1?.05:.005;
    for (double lon = floor((v->longitude-v->span/2)/step)*step; lon <=v->longitude+v->span/2; lon += step)
    {
        double x, y;
        point(v, w, h, v->latitude, lon, &x, &y);
        line(p, w, h, x, 0, x, h - 1, 0x1926);
    }
    double vertical=v->span*h/w;
    for (double lat = floor((v->latitude-vertical/2)/step)*step; lat <=v->latitude+vertical/2; lat += step)
    {
        double x, y;
        point(v, w, h, lat, v->longitude, &x, &y);
        line(p, w, h, 0, y, w - 1, y, 0x1926);
    }
    bool have = false;
    double px = 0, py = 0;
    for (size_t n = 0; n < sizeof(sdr_coastline) / sizeof(sdr_coastline[0]); n++)
    {
        if (sdr_coastline[n][0] == 32767)
        {
            have = false;
            continue;
        }
        double x, y;
        point(v, w, h, sdr_coastline[n][1] / 100.0, sdr_coastline[n][0] / 100.0, &x, &y);
        if (have && fabs(x - px) < w * 180 / v->span)
            line(p, w, h, px, py, x, y, 0x4B72);
        have = true;
        px = x;
        py = y;
    }
    /* Region vertices use 1e-5 degree precision (~1 m); zoom never invents detail.
     * Cull in integer coordinates before projection. No allocation or I/O here. */
    if(v->span<4){
        int32_t west=(v->longitude-v->span/2)*100000,east=(v->longitude+v->span/2)*100000;
        int32_t south=(v->latitude-vertical/2)*100000,north=(v->latitude+vertical/2)*100000;
        const uint16_t colors[]={0x5C15,0x32AF,0xEF5D,0xB46B,0x6B91,0x3A2B};
        for(size_t n=0;n<sizeof(regional_features)/sizeof(regional_features[0]);++n){
            unsigned kind=regional_features[n].kind;
            if((v->span>.8&&kind>=4)||(v->span>.18&&kind>=5)||regional_features[n].west>east||regional_features[n].east<west||regional_features[n].south>north||regional_features[n].north<south)continue;
            double last_x=0,last_y=0;
            for(unsigned k=0;k<regional_features[n].count;++k){
                const int32_t *q=regional_points[regional_features[n].start+k];double xx,yy;
                point(v,w,h,q[1]/100000.0,q[0]/100000.0,&xx,&yy);
                if(k)line(p,w,h,last_x,last_y,xx,yy,colors[kind]);
                last_x=xx;last_y=yy;
            }
        }
    }
}

void sdr_map_marker(uint16_t *p, int w, int h, int x, int y, float heading, bool selected)
{
    if (!p || !isfinite(heading))
        return;
    const double a = heading * 3.141592653589793 / 180;
    double px[3], py[3];
    const double angles[] = {a, a + 2.5, a - 2.5};
    for (unsigned n = 0; n < 3; n++)
    {
        px[n] = x + sin(angles[n]) * 8;
        py[n] = y - cos(angles[n]) * 8;
    }
    for (unsigned n = 0; n < 3; n++)
        line(p, w, h, px[n], py[n], px[(n + 1) % 3], py[(n + 1) % 3], selected ? 0xF670 : 0x6EF8);
    if (x >= 0 && x < w && y >= 0 && y < h)
        p[y * w + x] = 0xFFFF;
}
