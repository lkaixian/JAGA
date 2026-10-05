#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// One cached reading, field-for-field the same as a row of telem.csv (see
// TELEMETRY_CSV_HEADER in main.c).
typedef struct {
    uint32_t log_timestamp_s;
    uint8_t  node_id;
    uint32_t sender_timestamp_s;
    float    temperature_c;
    float    humidity_pct;
    uint32_t distance_mm;
    uint32_t distance_stddev_mm;
    int32_t  water_level_mm;
} api_reading_t;

// Timestamp used for log_timestamp_s (RTC wall time, or seconds since boot).
// Defined in main.c.
uint32_t current_timestamp_s(void);

/**
 * @brief Start Wi-Fi (WIFI_SSID/WIFI_PASS in node_config.h), mDNS as
 *        "jaga-master.local", and the HTTP JSON API on port 80:
 *          GET /api/status                    - now_s, uptime_s, rtc_ok, sd_ok
 *          GET /api/nodes                     - static per-node id/name/lat/lng
 *          GET /api/latest                    - each node's most recent reading
 *          GET /api/history?node=1-3&limit=N  - that node's recent readings, newest first
 *        Does not wait for Wi-Fi: the retry handler keeps reconnecting and
 *        httpd serves as soon as an IP arrives (logged as "Got IP"). Call once
 *        from app_main after hardware init.
 */
esp_err_t api_start(void);

/** @brief Report hardware state for /api/status. Call before api_start(). */
void api_set_health(bool rtc_ok, bool sd_ok);

/**
 * @brief Record one reading (this node's own, or one drained from a slave)
 *        into the API's in-RAM ring buffer, so /api/latest and /api/history
 *        serve it without re-reading the SD card. Safe to call even if
 *        api_start() never ran or failed - it's just a no-op then.
 */
void api_record_reading(const api_reading_t *r);

#ifdef __cplusplus
}
#endif
