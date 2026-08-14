#include "telink.h"

#include <string.h>

static uint8_t checksum(const uint8_t p[10])
{
    uint16_t s = 0;
    for (int i = 1; i < 10; i++) s += p[i];
    return (uint8_t)(s & 0xff);
}

uint8_t *amaran_telink_status_request(uint8_t out[10])
{
    memset(out, 0, 10);
    out[9] = 0x0e;
    out[0] = checksum(out);
    return out;
}

bool amaran_telink_decode_status(const uint8_t p[10], amaran_status_t *out)
{
    memset(out, 0, sizeof(*out));
    if (p[0] != checksum(p)) return false;

    uint8_t cmd = p[9] & 0x7f;     /* high bit is opera_type (set vs report) */
    out->command_type = cmd;

    uint64_t low64 = 0;
    for (int i = 0; i < 8; i++) low64 |= (uint64_t)p[i] << (i * 8);
    uint16_t high16 = (uint16_t)p[8] | ((uint16_t)p[9] << 8);

    out->on = (low64 >> 8) & 0x01;

    if (cmd == 0x02) {                          /* CCT mode */
        out->is_hs = false;
        int cct_raw = (int)((low64 >> 52) & 0x3ff);
        int cct_flag = (int)((low64 >> 42) & 0x01);
        int telink_cct = cct_flag ? cct_raw + 1000 : cct_raw;
        out->cct_kelvin = (uint16_t)(telink_cct * 10);
        out->intensity = (uint16_t)((((uint32_t)high16 << 2) |
                                     (uint32_t)((low64 >> 62) & 0x03)) & 0x3ff);
        int gm = (int)((low64 >> 45) & 0x7f) - 10;
        out->gm = gm < -10 ? -10 : (gm > 10 ? 10 : gm);
        out->valid = true;
        return true;
    }

    if (cmd == 0x01) {                          /* HSI mode */
        out->is_hs = true;
        uint16_t sat = (uint16_t)(((p[6] & 0x1f) << 2) | ((p[5] >> 6) & 0x03));
        uint16_t hue = (uint16_t)(((p[7] & 0x3f) << 3) | ((p[6] >> 5) & 0x07));
        uint16_t v   = (uint16_t)(((uint16_t)p[8] << 2) | ((p[7] >> 6) & 0x03));
        out->sat = (uint8_t)(sat > 100 ? 100 : sat);
        out->hue = hue > 360 ? 360 : hue;
        out->intensity = v > 1000 ? 1000 : v;
        out->valid = true;
        return true;
    }
    return false;
}

uint8_t *amaran_telink_onoff(bool on, uint8_t out[10])
{
    memset(out, 0, 10);
    out[8] = on ? 0x01 : 0x00;
    out[9] = 0x8c;
    out[0] = checksum(out);
    return out;
}

uint8_t *amaran_telink_brightness(uint16_t intensity_0_1000, uint8_t out[10])
{
    uint16_t v = intensity_0_1000 > 1000 ? 1000 : intensity_0_1000;
    memset(out, 0, 10);
    out[7] = (uint8_t)((v & 3) << 6);
    out[8] = (uint8_t)((v >> 2) & 0xff);
    out[9] = 0x8f;
    out[0] = checksum(out);
    return out;
}

/* CCT packet. Ported byte-for-byte from the validated Swift cctPacket() that
 * the amaran CLI uses on real fixtures. Two things a naive port gets wrong:
 *   1. The CCT field is kelvin/10 (an 80..2000 "telink CCT"), not raw kelvin --
 *      raw kelvin overflows the 10-bit field and produces random colors.
 *   2. The +0x18 offset and the wrap marker bit only apply ABOVE 10000K.
 *
 * gm_offset: green/magenta tint, -10..+10, 0 = neutral. Maps to the fixture's
 * 0..20 green-side scale (neutral 10): lower = greener, higher = more magenta. */
uint8_t *amaran_telink_cct(uint16_t kelvin, uint16_t intensity_0_1000,
                           int gm_offset, uint8_t out[10])
{
    int v = intensity_0_1000 > 1000 ? 1000 : intensity_0_1000;
    if (v < 0) v = 0;

    int tcct = ((int)kelvin + 5) / 10;   /* kelvin -> telink CCT, rounded */
    if (tcct < 80) tcct = 80;
    if (tcct > 2000) tcct = 2000;

    int g = gm_offset + 10;              /* -10..+10  ->  0..20 (neutral 10) */
    if (g < 0) g = 0;
    if (g > 20) g = 20;
    int gm_flag = 0;                     /* CLI always uses the green-side field */

    uint64_t low64 = ((uint64_t)(v & 0x03)) << 62;
    uint16_t high16 = (uint16_t)(0x8200 | ((v >> 2) & 0xff));
    if (tcct < 1001) {
        low64 |= (uint64_t)tcct << 52;
        high16 |= (uint16_t)((tcct >> 12) & 0xff);
    } else {
        low64 |= ((uint64_t)((tcct + 0x18) & 0x3ff)) << 52;
        low64 |= 0x0000040000000000ULL;
    }
    low64 |= (uint64_t)(gm_flag & 0x01) << 43;
    low64 |= (uint64_t)(g & 0x7f) << 45;

    for (int i = 0; i < 8; i++) out[i] = (uint8_t)((low64 >> (i * 8)) & 0xff);
    out[8] = (uint8_t)(high16 & 0xff);
    out[9] = (uint8_t)((high16 >> 8) & 0xff);
    out[0] = checksum(out);
    return out;
}

uint8_t *amaran_telink_hsi(uint16_t hue_deg, uint8_t saturation_0_100,
                           uint16_t intensity_0_1000, uint8_t out[10])
{
    uint16_t v = intensity_0_1000 > 1000 ? 1000 : intensity_0_1000;
    uint16_t h = hue_deg > 360 ? 360 : hue_deg;
    h &= 0x1ff;
    uint8_t s = saturation_0_100 > 100 ? 100 : saturation_0_100;
    s &= 0x7f;

    memset(out, 0, 10);
    out[5] = (uint8_t)((s & 3) << 6);
    out[6] = (uint8_t)(((h & 7) << 5) | ((s >> 2) & 0x1f));
    out[7] = (uint8_t)(((h >> 3) & 0x3f) | ((v & 3) << 6));
    out[8] = (uint8_t)((v >> 2) & 0xff);
    out[9] = 0x81;
    out[0] = checksum(out);
    return out;
}
