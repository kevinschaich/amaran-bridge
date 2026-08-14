/*
 * Telink proprietary 0x26 vendor payload builders.
 *
 * Reverse-engineered from the amaran Desktop app's PyMeshSDK by intercepting
 * writeValue: calls on the live mesh proxy. Every payload is exactly 10 bytes;
 * out[0] is a checksum over out[1..9] and out[9] is the command type.
 *
 * This file is the crown jewels. It is copied verbatim from the original
 * firmware and must not be "cleaned up".
 */

#ifndef AMARAN_TELINK_H
#define AMARAN_TELINK_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Telink company id. The lights expose a vendor model under this cid, which is
 * what the AppKey binds to -- but the opcode below is sent as a *single byte*,
 * not as a 3-byte vendor opcode. Anything < 0x80 is a 1-byte opcode per the
 * BLE Mesh spec, and the fixtures' custom Telink stack registers 0x26 directly.
 * Sending it in the standard 3-byte vendor form does nothing at all. */
#define AMARAN_VENDOR_COMPANY_ID 0x0211
#define AMARAN_VENDOR_OPCODE     0x26

/* All builders write 10 bytes to out[0..9] and return out. */
uint8_t *amaran_telink_onoff(bool on, uint8_t out[10]);
uint8_t *amaran_telink_brightness(uint16_t intensity_0_1000, uint8_t out[10]);

/* gm_offset: green/magenta tint, -10..+10, 0 = neutral (negative greener,
 * positive more magenta). */
uint8_t *amaran_telink_cct(uint16_t kelvin, uint16_t intensity_0_1000,
                           int gm_offset, uint8_t out[10]);

uint8_t *amaran_telink_hsi(uint16_t hue_deg, uint8_t saturation_0_100,
                           uint16_t intensity_0_1000, uint8_t out[10]);

/* Status request (cmd_type 0x0e). The fixture answers with its current state.
 * The reply is addressed to the provisioner (0x0001), not to whoever asked --
 * which is why this node lives at 0x0001. */
uint8_t *amaran_telink_status_request(uint8_t out[10]);

typedef struct {
    bool valid;
    bool on;
    bool is_hs;
    uint16_t intensity;   /* 0..1000 */
    uint16_t cct_kelvin;
    int gm;
    uint16_t hue;
    uint8_t sat;
    uint8_t command_type;
} amaran_status_t;

/* Decode a 10-byte reply. True only for decodable state replies (0x01 HSI /
 * 0x02 CCT) with a good checksum; 0x0a is a diagnostic page we ignore. */
bool amaran_telink_decode_status(const uint8_t p[10], amaran_status_t *out);

#ifdef __cplusplus
}
#endif

#endif /* AMARAN_TELINK_H */
