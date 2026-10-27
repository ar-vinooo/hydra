/* Independent BC6H/BC7 block decoder used as the oracle of
 * tests/texture_format_reference.py. It wraps bcdec.h (iOrange/bcdec, MIT or
 * Unlicense), which tests/texture_format_reference.py downloads at run time;
 * it is not vendored.
 *
 * stdin lines:  "<bc7|bc6u|bc6s> <32 hex digits>"
 * stdout lines: 64 hex 32-bit words, 16 texels of (r, g, b, a) as float bits
 */
#define BCDEC_IMPLEMENTATION
#include "bcdec.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static uint32_t float_bits(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

int main(void) {
    char kind[16], hex[64];
    while (scanf("%15s %63s", kind, hex) == 2) {
        unsigned char block[16];
        for (int i = 0; i < 16; ++i) {
            unsigned value;
            sscanf(hex + 2 * i, "%2x", &value);
            block[i] = (unsigned char)value;
        }
        float out[16][4];
        if (strcmp(kind, "bc7") == 0) {
            unsigned char pixels[16][4];
            bcdec_bc7(block, pixels, 16);
            for (int i = 0; i < 16; ++i)
                for (int c = 0; c < 4; ++c)
                    out[i][c] = pixels[i][c] / 255.0f;
        } else {
            float rgb[16][3];
            bcdec_bc6h_float(block, rgb, 12, strcmp(kind, "bc6s") == 0);
            for (int i = 0; i < 16; ++i) {
                out[i][0] = rgb[i][0];
                out[i][1] = rgb[i][1];
                out[i][2] = rgb[i][2];
                out[i][3] = 1.0f;
            }
        }
        for (int i = 0; i < 16; ++i)
            for (int c = 0; c < 4; ++c)
                printf("%x ", float_bits(out[i][c]));
        printf("\n");
    }
    return 0;
}
