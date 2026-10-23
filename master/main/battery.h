#pragma once

#include "driver/adc.h"

// Battery voltage on an ADC1 pin through a divider. Defaults: GPIO35, two equal resistors.
// Tune BATT_CAL against a multimeter reading (real volts / reported volts).
#ifndef BATT_ADC_CHANNEL
#define BATT_ADC_CHANNEL ADC1_CHANNEL_7  // GPIO35
#endif
#ifndef BATT_DIVIDER
#define BATT_DIVIDER 2.0f
#endif
#ifndef BATT_CAL
#define BATT_CAL 1.0f
#endif

/** @brief Battery voltage in mV. */
uint16_t battery_mv(void);
