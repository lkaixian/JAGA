#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_NODES 8 // node ids 1..MAX_NODES

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
 * @brief Start the push task: every few minutes it powers the SIM900A, dials PPP
 *        (MODEM_APN in node_config.h), POSTs the queued readings to INGEST_URL and
 *        powers the modem off again. Does nothing if INGEST_URL is empty. Call once
 *        from app_main after hardware init.
 */
esp_err_t api_start(void);

/** @brief Report hardware state for /api/status. Call once before api_start() and again every cycle. */
void api_set_health(bool rtc_ok, bool sd_ok, bool radio_ok);

/**
 * @brief Queue one reading (this node's own, or one drained from a slave) for
 *        the next push. Safe to call even if api_start() never ran or failed -
 *        it's just a no-op then.
 */
void api_record_reading(const api_reading_t *r);

#ifdef __cplusplus
}
#endif
