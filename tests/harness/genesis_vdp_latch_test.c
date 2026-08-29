#include "genesis_vdp.h"

#include <stdio.h>

static int check_u16(const char *name, uint16_t got, uint16_t expected)
{
    if (got == expected)
        return 1;
    fprintf(stderr, "%s: expected $%04X, got $%04X\n", name, expected, got);
    return 0;
}

static int check_u8(const char *name, uint8_t got, uint8_t expected)
{
    if (got == expected)
        return 1;
    fprintf(stderr, "%s: expected $%02X, got $%02X\n", name, expected, got);
    return 0;
}

int main(void)
{
    GVDP v;
    gvdp_init(&v);

    /* Full-plane vertical scroll and window positions are sampled after the
     * CPU slice, then remain stable while the following line is rendered. */
    v.reg[0x01] = 0x40;
    v.reg[0x0B] = 0x00;
    v.vsram[0] = 0x0123;
    v.vsram[1] = 0x02AB;
    v.reg[0x11] = 0x91;
    v.reg[0x12] = 0x07;
    gvdp_latch_scanline_state(&v);

    if (!check_u16("plane A latch", v.vscroll_latch[0], 0x0123) ||
        !check_u16("plane B latch", v.vscroll_latch[1], 0x02AB) ||
        !check_u8("window X latch", v.window_x_latch, 0x91) ||
        !check_u8("window Y latch", v.window_y_latch, 0x07))
        return 1;

    v.vsram[0] = 0x0008;
    v.vsram[1] = 0x0010;
    v.reg[0x11] = 0x02;
    v.reg[0x12] = 0x83;
    if (!check_u16("plane A stable within line", v.vscroll_latch[0], 0x0123) ||
        !check_u16("plane B stable within line", v.vscroll_latch[1], 0x02AB) ||
        !check_u8("window X stable within line", v.window_x_latch, 0x91) ||
        !check_u8("window Y stable within line", v.window_y_latch, 0x07))
        return 1;

    gvdp_latch_scanline_state(&v);
    if (!check_u16("plane A next line", v.vscroll_latch[0], 0x0008) ||
        !check_u16("plane B next line", v.vscroll_latch[1], 0x0010) ||
        !check_u8("window X next line", v.window_x_latch, 0x02) ||
        !check_u8("window Y next line", v.window_y_latch, 0x83))
        return 1;

    /* In two-cell mode the renderer samples per-column VSRAM entries; the
     * full-plane latch must not be overwritten by that mode's line boundary. */
    v.reg[0x0B] = 0x04;
    v.vsram[0] = 0x0333;
    v.vsram[1] = 0x0222;
    gvdp_latch_scanline_state(&v);
    if (!check_u16("two-cell preserves A full latch", v.vscroll_latch[0], 0x0008) ||
        !check_u16("two-cell preserves B full latch", v.vscroll_latch[1], 0x0010))
        return 1;

    /* Writes made while display is disabled do not replace the retained
     * scroll/window state. */
    v.reg[0x01] = 0x00;
    v.reg[0x0B] = 0x00;
    v.vsram[0] = 0x0001;
    v.vsram[1] = 0x0002;
    v.reg[0x11] = 0x11;
    v.reg[0x12] = 0x22;
    gvdp_latch_scanline_state(&v);
    if (!check_u16("display-off preserves A latch", v.vscroll_latch[0], 0x0008) ||
        !check_u16("display-off preserves B latch", v.vscroll_latch[1], 0x0010) ||
        !check_u8("display-off preserves window X", v.window_x_latch, 0x02) ||
        !check_u8("display-off preserves window Y", v.window_y_latch, 0x83))
        return 1;

    puts("genesis VDP scanline latch tests passed");
    return 0;
}
