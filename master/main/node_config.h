#pragma once

#include "jsnsr40t.h"

// This node's identity in the telemetry packet. Each of the 3 physical nodes
// (2 slaves + this master, which also carries its own sensors) needs a
// unique value here. Slaves currently use 1 and 2.
#define NODE_ID 3

// Fixed sensor mount geometry, measured once at installation.
// TODO: replace with this node's real measured values.
#define SONAR_MOUNT_HEIGHT_MM   1000   // vertical height of sensor above the reference (e.g. tank bottom)
#define SONAR_MOUNT_ANGLE_DEG   0.0f   // tilt of sensor axis from vertical (0 = straight down)

// Sonar readings beyond this are rejected as noise. Defaults to the sensor's
// own spec ceiling; override with a smaller value if this node's tank/mount
// height is known to be shorter than that.
#define SONAR_MAX_RANGE_MM      JSNSR40T_DEFAULT_MAX_RANGE_MM

// The OLED is a demo/debug aid. Set to 0 for real deployment - skips the
// I2C probe and all display writes, saving its draw on battery/solar power.
#define OLED_ENABLED 1
