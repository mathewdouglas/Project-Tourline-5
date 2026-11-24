/* Offline checks of the runtime's emulated Logitech G27 (libs/hardware/wheel_g27.c):
 * its USB descriptors, the 11-byte input report GT5's driver decodes, and the
 * force-feedback command decoder. No game, no wheel. Built by wheel_g27.py. */
#include "wheel.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int s_changed;
void wheel_host_ffb_changed(void) { s_changed++; }
int wheel_trace_on(void) { return 0; }

static int s_fail;
#define CHECK(cond, ...) do { if (!(cond)) { s_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int steer_of(const u8* r) { return (r[3] >> 2) | (r[4] << 6); }

static void send(u8 a, u8 b, u8 c, u8 d, u8 e, u8 f, u8 g)
{
    const u8 cmd[7] = { a, b, c, d, e, f, g };
    wheel_g27_output(cmd, 7);
}

int main(void)
{
    /* ---- descriptors ---- */
    u8 desc[128];
    const u32 n = wheel_g27_descriptors(desc, sizeof desc);
    CHECK(n == 18 + 41, "descriptor blob %u bytes", n);
    CHECK(desc[0] == 18 && desc[1] == 1, "device descriptor header");
    CHECK(desc[7] > 8, "bMaxPacketSize0 %u: GT5's genuine-device check wants more than 8", desc[7]);
    CHECK((desc[8] | desc[9] << 8) == 0x046D, "vendor %04X", desc[8] | desc[9] << 8);
    CHECK((desc[10] | desc[11] << 8) == 0xC29B, "product %04X (G27)", desc[10] | desc[11] << 8);
    CHECK(desc[18 + 1] == 2 && (desc[18 + 2] | desc[18 + 3] << 8) == 41, "configuration wTotalLength");
    {   /* every descriptor's length walks exactly to the end */
        u32 p = 18; int eps = 0;
        while (p < n && desc[p] >= 2) { if (desc[p + 1] == 5) eps++; p += desc[p]; }
        CHECK(p == n, "descriptor chain ends at %u of %u", p, n);
        CHECK(eps == 2, "%d endpoints (interrupt IN and OUT)", eps);
    }

    /* ---- input report ---- */
    wheel_g27_reset();
    WheelInput in; u8 r[11];
    memset(&in, 0, sizeof in); in.connected = 1; in.hat = -1;
    CHECK(wheel_g27_report(&in, r) == 11, "report length");
    CHECK((r[0] & 15) == 8, "hat centred is 8, got %d", r[0] & 15);
    CHECK(steer_of(r) == 0x2000, "rim centred 0x2000, got %04X", steer_of(r));
    CHECK(r[5] == 0xFF && r[6] == 0xFF && r[7] == 0xFF, "pedals released read FF: %02X %02X %02X", r[5], r[6], r[7]);
    CHECK(r[10] & 0x10, "report byte 10 bit 4 always set");
    in.steer = -1.f; wheel_g27_report(&in, r);
    CHECK(steer_of(r) == 0, "full left 0, got %04X", steer_of(r));
    in.steer = 1.f; wheel_g27_report(&in, r);
    CHECK(steer_of(r) == 0x3FFF, "full right 3FFF, got %04X", steer_of(r));
    in.steer = 0.f; in.throttle = 1.f; in.brake = 0.5f;
    in.buttons = (1u << WHEEL_BTN_CROSS) | (1u << WHEEL_BTN_START) | (1u << WHEEL_BTN_GEAR3);
    in.hat = 2;
    wheel_g27_report(&in, r);
    CHECK(r[5] == 0x00, "throttle floored reads 00, got %02X", r[5]);
    CHECK(r[6] >= 0x7F && r[6] <= 0x80, "brake half down reads ~80, got %02X", r[6]);
    CHECK((r[0] & 15) == 2 && (r[0] & 0x10), "hat right and Cross: %02X", r[0]);
    CHECK(r[1] & (1 << (WHEEL_BTN_START - WHEEL_BTN_R1)), "Start in byte 1: %02X", r[1]);
    CHECK(r[2] & (1 << 2), "gear 3 in byte 2: %02X", r[2]);

    /* The game's 200-degree rotation on a 900-degree PC wheel: a fifth of the PC wheel's
     * half-turn is nearly the G27's full lock, and more saturates. */
    send(0xF8, 0x81, 0xC8, 0x00, 0, 0, 0);
    WheelFfb f; wheel_g27_ffb_snapshot(&f);
    CHECK(f.range_deg == 200, "range command: %d degrees", f.range_deg);
    memset(&in, 0, sizeof in); in.connected = 1; in.hat = -1;
    in.steer = 0.2f; wheel_g27_report(&in, r);
    CHECK(fabs((steer_of(r) - 0x2000) / 8192.0 - 0.9) < 0.01, "0.2 of a 900-degree wheel at 200 degrees: %04X", steer_of(r));
    in.steer = 0.5f; wheel_g27_report(&in, r);
    CHECK(steer_of(r) == 0x3FFF, "past the 200-degree stop saturates: %04X", steer_of(r));
    CHECK(!(r[10] & 0x80), "range 200 is not above 360");

    /* ---- force feedback ---- */
    wheel_g27_reset();
    wheel_g27_ffb_snapshot(&f);
    CHECK(f.def_spring_on && f.def_spring.type == WHEEL_FX_SPRING, "power-on default spring");
    send(0x05, 0, 0, 0, 0, 0, 0);                       /* default spring off */
    wheel_g27_ffb_snapshot(&f);
    CHECK(!f.def_spring_on, "default spring off");
    s_changed = 0;
    send(0x11, 0x00, 0xC0, 0x80, 0x80, 0x80, 0x00);    /* slot 0: constant, download and play */
    wheel_g27_ffb_snapshot(&f);
    CHECK(s_changed == 1, "a decoded command tells the host side once (%d)", s_changed);
    CHECK(f.slot[0].type == WHEEL_FX_CONSTANT && f.slot[0].playing, "slot 0 constant playing");
    CHECK(fabsf(f.slot[0].level - 64.f / 127.f) < 1e-4f, "constant level %.4f", f.slot[0].level);
    send(0x11, 0x00, 0x80, 0x80, 0x80, 0x80, 0x00);
    wheel_g27_ffb_snapshot(&f);
    CHECK(f.slot[0].level == 0.f, "0x80 is no force");
    CHECK(!wheel_g27_in_menus(), "a steering force alone is not the menus");
    send(0x21, 0x0B, 0x7F, 0x7F, 0x77, 0x00, 0x80);    /* slot 1: GT5's menu centring spring */
    wheel_g27_ffb_snapshot(&f);
    CHECK(f.slot[1].type == WHEEL_FX_SPRING && f.slot[1].playing, "slot 1 spring playing");
    CHECK(f.slot[1].k_neg > 0.f && f.slot[1].k_pos > 0.f, "spring coefficients %.3f %.3f", f.slot[1].k_neg, f.slot[1].k_pos);
    CHECK(wheel_g27_in_menus(), "the menu spring means the menus");
    send(0x23, 0x0B, 0x7F, 0x7F, 0x77, 0x00, 0x80);    /* stop slot 1, as a race loads */
    CHECK(!wheel_g27_in_menus(), "spring stopped: not the menus");
    send(0x31, 0x00, 0x40, 0x20, 0x80, 0x80, 0x00);    /* slots 0 and 1 at once, own level bytes */
    wheel_g27_ffb_snapshot(&f);
    CHECK(f.slot[0].level < 0.f && f.slot[1].level < f.slot[0].level, "per-slot levels %.3f %.3f",
          f.slot[0].level, f.slot[1].level);
    send(0xF3, 0, 0, 0, 0, 0, 0);                       /* stop all four */
    wheel_g27_ffb_snapshot(&f);
    CHECK(!f.slot[0].playing && !f.slot[1].playing, "stop clears playing");
    s_changed = 0;
    send(0xF8, 0x12, 0x1F, 0, 0, 0, 0);                 /* rev lights: no effect change */
    CHECK(s_changed == 0, "rev lights change no effect");

    if (s_fail) { printf("%d check(s) failed\n", s_fail); return 1; }
    printf("wheel_g27: all checks passed\n");
    return 0;
}
