// common/bcd.h: what the DS1307 driver encodes must decode back unchanged.
#include <assert.h>
#include <stdio.h>

#include "bcd.h"

int main(void) {
    // Every value the clock fields can hold round-trips.
    for (uint8_t dec = 0; dec <= 99; dec++) {
        assert(bcd_to_dec(dec_to_bcd(dec)) == dec);
    }
    // The register encoding the chip actually stores (BCD 59 = 0x59).
    assert(dec_to_bcd(59) == 0x59);
    assert(dec_to_bcd(0) == 0x00);
    assert(dec_to_bcd(9) == 0x09);
    assert(bcd_to_dec(0x07) == 7);
    assert(bcd_to_dec(0x99) == 99);

    printf("bcd round trip ok\n");
    return 0;
}
