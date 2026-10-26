#pragma once
#include <stdint.h>

// DS1307 registers store BCD. Both projects' ds1307.c include this instead of
// keeping their own copies; test/host/bcd_test.c round-trips it.

static inline uint8_t bcd_to_dec(uint8_t bcd) { return ((bcd >> 4) * 10) + (bcd & 0x0F); }
static inline uint8_t dec_to_bcd(uint8_t dec) { return (uint8_t)(((dec / 10) << 4) | (dec % 10)); }
