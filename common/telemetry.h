#pragma once
#include <stdint.h>

// The over-the-air binary packet, in ONE place. The slave project (main/) and
// the master project (master/main/) both include this header, so the two sides
// cannot drift apart - CI's test/host/packet_layout.c asserts the exact byte
// offsets and any change that moves a field fails the firmware-host-tests job.
// This is a real wire format: an old node talking to a new master decodes as
// garbage, so bump nothing here without planning the node fleet's rollout.
typedef struct __attribute__((packed)) {
    uint8_t node_id;             // which physical node this reading came from
    float temperature;           // C; TELEMETRY_READ_FAILED when the DHT22 did not answer
    float humidity;              // %; TELEMETRY_READ_FAILED likewise
    uint32_t distance_mm;        // mean of up to SONAR_SAMPLE_COUNT samples (0 when all failed)
    uint32_t distance_stddev_mm; // spread across those samples (0 if <2 valid)
    int32_t water_level_mm;      // TELEMETRY_LEVEL_UNKNOWN when every sonar sample failed
    uint32_t timestamp_s;        // seconds since this node's last power-on (the master stamps wall-clock time on receipt)
    uint16_t batt_mv;            // battery voltage
    uint8_t link_retries;        // nRF24 retries the previous acknowledged send needed (0-15), TELEMETRY_NO_LINK_HISTORY = none yet
} telemetry_packet_t;

// Sentinel values shared by both sides (see test/host for the exact numbers).
#define TELEMETRY_READ_FAILED (-999.0f)          // DHT22: no temperature/humidity
#define TELEMETRY_LEVEL_UNKNOWN ((int32_t)0x80000000) // INT32_MIN: sonar gave nothing
#define TELEMETRY_NO_LINK_HISTORY 255u           // no acknowledged send since power-on
