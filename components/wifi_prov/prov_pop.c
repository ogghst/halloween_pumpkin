/* Proof of possession derived from the device MAC (see prov_mqtt_codec.h). */
#include <stdio.h>

#include "prov_mqtt_codec.h"

void prov_pop_from_mac(const uint8_t mac[6], char out[13])
{
    if (!mac || !out) {
        return;
    }
    /* "pump-" + 3 bytes in uppercase hex = 12 chars + NUL. */
    (void)snprintf(out, 13, "pump-%02X%02X%02X",
                   mac[3], mac[4], mac[5]);
}
