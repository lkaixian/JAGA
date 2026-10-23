#include <stdbool.h>
#include "esp_adc_cal.h"
#include "battery.h"

uint16_t battery_mv(void) {
    if (!BATT_FITTED) return 0;
    static esp_adc_cal_characteristics_t cal;
    static bool ready;
    if (!ready) {
        adc1_config_width(ADC_WIDTH_BIT_12);
        adc1_config_channel_atten(BATT_ADC_CHANNEL, ADC_ATTEN_DB_11);
        esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12, 1100, &cal);
        ready = true;
    }
    uint32_t raw = 0;
    for (int i = 0; i < 16; i++) raw += adc1_get_raw(BATT_ADC_CHANNEL);
    return (uint16_t)(esp_adc_cal_raw_to_voltage(raw / 16, &cal) * BATT_DIVIDER * BATT_CAL);
}
