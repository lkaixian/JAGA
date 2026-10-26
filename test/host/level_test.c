// common/level.h against hand-checked numbers: the sentinel path, mean/stddev,
// tilt projection, truncation and out-of-range (negative) levels.
#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "level.h"

static telemetry_packet_t read_sonar(const uint32_t *samples, int n, float height_mm, float angle_deg) {
    telemetry_packet_t p = {0};
    tele_fill_sonar(&p, samples, n, height_mm, angle_deg);
    return p;
}

int main(void) {
    uint32_t one[] = {100};
    uint32_t pair[] = {100, 200};
    uint32_t close[] = {100, 101};
    uint32_t slant[] = {1000};
    uint32_t dry_well[] = {1500};

    // No valid samples at all -> the sentinel every parser on the bus expects.
    telemetry_packet_t s = read_sonar(one, 0, 1000, 0);
    assert(s.distance_mm == 0 && s.distance_stddev_mm == 0);
    assert(s.water_level_mm == TELEMETRY_LEVEL_UNKNOWN);

    // Straight down: level = mount height - distance.
    telemetry_packet_t a = read_sonar(one, 1, 1000, 0);
    assert(a.distance_mm == 100 && a.distance_stddev_mm == 0 && a.water_level_mm == 900);

    // Spread of 100/200: mean 150, population stddev exactly 50.
    telemetry_packet_t b = read_sonar(pair, 2, 1000, 0);
    assert(b.distance_mm == 150 && b.distance_stddev_mm == 50 && b.water_level_mm == 850);

    // Mean truncates toward zero (201/2 = 100), and the sqrt of a 0.5 variance
    // truncates to 0. Pinned here so a rewrite of the maths notices.
    telemetry_packet_t c = read_sonar(close, 2, 1000, 0);
    assert(c.distance_mm == 100 && c.distance_stddev_mm == 0 && c.water_level_mm == 900);

    // 60 deg tilt: vertical = 1000 * cos(60) = 500 +- float noise; level
    // lands within 1 mm of 500 (the int cast can eat 0.999...).
    telemetry_packet_t t = read_sonar(slant, 1, 1000, 60.0f);
    assert(t.distance_mm == 1000);
    assert(fabsf((float)t.water_level_mm - 500.0f) <= 1.0f);
    assert(fabsf(tele_vertical_mm(1000, 60.0f) - 500.0f) < 0.01f);

    // Out of range: beam reading beyond the mount height means the sensor
    // sees below the reference point -> negative level, not an error.
    telemetry_packet_t d = read_sonar(dry_well, 1, 400, 0); // master placeholder height
    assert(d.distance_mm == 1500 && d.water_level_mm == 400 - 1500);

    // Fractional mount heights truncate like the firmware always has.
    assert(tele_water_level_mm(100, 100.5f, 0) == 0);

    printf("level maths ok\n");
    return 0;
}
