#pragma once

#include "jsnsr40t.h"

// This node's identity in the telemetry packet. Each of the 3 physical nodes
// (2 slaves + the master, which also carries its own sensors) needs a unique
// value here.
#define NODE_ID 1

// Fixed sensor mount geometry, measured once at installation.
// TODO: replace with this node's real measured values.
#define SONAR_MOUNT_HEIGHT_MM   1000   // vertical height of sensor above the reference (e.g. tank bottom)
#define SONAR_MOUNT_ANGLE_DEG   0.0f   // tilt of sensor axis from vertical (0 = straight down)

// Sonar readings beyond this are rejected as noise. Defaults to the sensor's
// own spec ceiling; override with a smaller value if this node's tank/mount
// height is known to be shorter than that.
#define SONAR_MAX_RANGE_MM      JSNSR40T_DEFAULT_MAX_RANGE_MM

