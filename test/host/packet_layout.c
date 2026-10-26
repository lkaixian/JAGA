// The one place that pins the over-the-air wire format. Slave and master both
// build from common/telemetry.h, so a compile-time check here protects the
// fleet: change a field and this fails before any node can ship broken bytes.
#include <assert.h>
#include <stddef.h>
#include <stdio.h>

#include "telemetry.h"

int main(void) {
    // Packed: no padding anywhere. 1+4+4+4+4+4+4+2+1 = 28 bytes, inside the
    // nRF24's 32-byte payload ceiling with room to spare.
    assert(sizeof(telemetry_packet_t) == 28);
    assert(offsetof(telemetry_packet_t, node_id) == 0);
    assert(offsetof(telemetry_packet_t, temperature) == 1);
    assert(offsetof(telemetry_packet_t, humidity) == 5);
    assert(offsetof(telemetry_packet_t, distance_mm) == 9);
    assert(offsetof(telemetry_packet_t, distance_stddev_mm) == 13);
    assert(offsetof(telemetry_packet_t, water_level_mm) == 17);
    assert(offsetof(telemetry_packet_t, timestamp_s) == 21);
    assert(offsetof(telemetry_packet_t, batt_mv) == 25);
    assert(offsetof(telemetry_packet_t, link_retries) == 27);

    // Sentinels both sides agree on.
    assert(TELEMETRY_LEVEL_UNKNOWN == (int32_t)0x80000000);
    assert(TELEMETRY_READ_FAILED == -999.0f);
    assert(TELEMETRY_NO_LINK_HISTORY == 255u);

    printf("packet layout ok\n");
    return 0;
}
