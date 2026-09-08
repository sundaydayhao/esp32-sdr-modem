# RTL-SDR driver attribution

`main/rtlsdr.c` incorporates register programming derived from librtlsdr and
the RTL-SDR Blog driver. V4/direct-sampling sequences were checked against
RTL-SDR Blog commit `aed0ea19f3a273370a13c9009b96313c75d54c7b`:

- https://github.com/rtlsdrblog/rtl-sdr-blog/blob/aed0ea19f3a273370a13c9009b96313c75d54c7b/src/tuner_r82xx.c
- https://github.com/rtlsdrblog/rtl-sdr-blog/blob/aed0ea19f3a273370a13c9009b96313c75d54c7b/src/librtlsdr.c

The Rafael tuner driver credits Mauro Carvalho Chehab and Steve Markgraf;
librtlsdr credits Steve Markgraf, Dimitri Stolnikov and contributors.
These sources are GPL-2.0-or-later. The upstream license is in `COPYING`.
Preserve notices and corresponding-source obligations when redistributing
this port or firmware containing it.

Codec2 retains its LGPL-2.1 license in `components/codec2/vendor/COPYING`.
ESP-IDF/BSP, LVGL and FT8 components retain their respective licenses.
