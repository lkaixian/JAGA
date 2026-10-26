#pragma once
#include <math.h>
#include <stdint.h>

#include "telemetry.h"

// Pure level maths: no ESP-IDF, no FreeRTOS - the same code the firmware runs
// and the host tests in test/host/ exercise under gcc.

// The sonar beam measures slant distance along its own axis. The sensor is
// mounted at a fixed tilt, so project that onto the vertical before
// subtracting from the mount height: water level = height - vertical.
static inline float tele_vertical_mm(uint32_t slant_mm, float mount_angle_deg) {
    return (float)slant_mm * cosf(mount_angle_deg * (float)M_PI / 180.0f);
}

static inline int32_t tele_water_level_mm(uint32_t mean_distance_mm, float mount_height_mm, float mount_angle_deg) {
    return (int32_t)(mount_height_mm - tele_vertical_mm(mean_distance_mm, mount_angle_deg));
}

// Fill the packet's three sonar fields from the valid samples (oldest first).
// n == 0 sends the sentinel: distance 0, stddev 0, water level INT32_MIN.
// Mean is integer, stddev is the population spread truncated to uint32.
// A level below the reference point comes out negative - that is a real
// reading (tank empty of water), not an error.
static inline void tele_fill_sonar(telemetry_packet_t *p, const uint32_t *samples, int n,
                                   float mount_height_mm, float mount_angle_deg) {
    if (n <= 0) {
        p->distance_mm = 0;
        p->distance_stddev_mm = 0;
        p->water_level_mm = TELEMETRY_LEVEL_UNKNOWN;
        return;
    }
    uint32_t sum = 0;
    for (int i = 0; i < n; i++) sum += samples[i];
    uint32_t mean = sum / n;

    float variance = 0;
    for (int i = 0; i < n; i++) {
        float diff = (float)samples[i] - (float)mean;
        variance += diff * diff;
    }
    variance /= n;

    p->distance_mm = mean;
    p->distance_stddev_mm = (uint32_t)sqrtf(variance);
    p->water_level_mm = tele_water_level_mm(mean, mount_height_mm, mount_angle_deg);
}
