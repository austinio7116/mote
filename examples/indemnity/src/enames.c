/*
 * ThumbyElite — procedural name generation.
 *
 * Classic Elite digraph chaining: names are built from a table of
 * letter-pairs chosen by successive bit-fields of the seed, which gives
 * that distinctive "Lave / Diso / Riedquat" cadence. Tables in flash.
 */
#include "enames.h"

/* 32 digraphs — vowel-heavy, pronounceable chains. */
static const char digraphs[32][3] = {
    "LE", "XE", "GE", "ZA", "CE", "BI", "SO", "US",
    "ES", "AR", "MA", "IN", "DI", "RE", "A",  "ER",
    "AT", "EN", "BE", "RA", "LA", "VE", "TI", "ED",
    "OR", "QU", "AN", "TE", "IS", "RI", "ON", "U",
};

static uint32_t mix(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

char *ename_system(uint32_t seed, char *out) {
    uint32_t h = mix(seed);
    int syll = 2 + (int)(h & 3);          /* 2-5 digraphs */
    if (syll > 4) syll = 4;
    h >>= 2;
    char *p = out;
    for (int i = 0; i < syll; i++) {
        const char *d = digraphs[h & 31];
        h >>= 5;
        *p++ = d[0];
        if (d[1]) *p++ = d[1];
        if (h == 0) h = mix(seed ^ (uint32_t)(i + 1));
    }
    *p = 0;
    /* Title-case: first letter stays, rest lower? 3x5 font is upper-only —
     * keep all caps. Trim absurdly short results by appending a digraph. */
    if (p - out < 3) {
        const char *d = digraphs[mix(seed ^ 99u) & 31];
        *p++ = d[0];
        if (d[1]) *p++ = d[1];
        *p = 0;
    }
    return out;
}

static const char *station_suffix[8] = {
    "PORT", "DOCK", "HUB", "STATION", "ORBITAL", "TERMINAL", "GATE", "POINT",
};

char *ename_station(uint32_t seed, char *out) {
    char *p = out;
    uint32_t h = mix(seed ^ 0xBEEF1234u);
    int syll = 2 + (int)(h & 1);
    h >>= 1;
    for (int i = 0; i < syll; i++) {
        const char *d = digraphs[h & 31];
        h >>= 5;
        *p++ = d[0];
        if (d[1]) *p++ = d[1];
    }
    *p++ = ' ';
    const char *suf = station_suffix[mix(seed ^ 0x51A7104u) & 7];
    while (*suf) *p++ = *suf++;
    *p = 0;
    return out;
}
