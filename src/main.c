#include <gint/display.h>
#include <gint/keyboard.h>
#include <stdint.h>
#include "btu.h"
#include "probes.h"

/* One device probe at a time. Never run MMIO before the user presses EXE. */
static btu_t hardware;

static void heading(const char *subtitle)
{
    dclear(C_WHITE);
    dtext(16, 10, C_BLACK, "EXWord SH7305 BTU proof");
    dtext(16, 33, C_BLACK, subtitle);
    dupdate();
}

static int key(void)
{
    for(;;) {
        key_event_t ev = waitevent(NULL);
        if(ev.type == KEYEV_DOWN)
            return ev.key;
    }
}

int main(void)
{
    heading("Real J_JP bytecode probes (not a JVM)");
    dtext(16, 72, C_BLACK, "EXE: access BTU MMIO and start tests");
    dtext(16, 97, C_RED, "Only on an SH7305-compatible EXWord!");
    dtext(16, 122, C_BLACK, "An incompatible CPU may fault or freeze.");
    dtext(16, 155, C_BLACK, "EXIT: leave without touching hardware");
    dupdate();

    for(;;) {
        int k = key();
        if(k == KEY_EXIT)
            return -2;
        if(k == KEY_EXE)
            break;
    }

    heading("Checking real MMIO + J_JP...");
    if(!btu_start(&hardware)) {
        dtext(16, 88, C_RED, "BTU register verification FAILED");
        dtext(16, 110, C_BLACK, "Hardware not proven. Press EXIT.");
        dupdate();
        while(key() != KEY_EXIT) {}
        return -2;
    }

    dclear(C_WHITE);
    dtext(16, 8, C_BLACK, "BTU hardware bytecode checks");
    dprint(16, 28, C_BLACK, "Control = %08x", (unsigned)btu_control());
    dupdate();

    unsigned passed = 0;
    for(unsigned i = 0; i < probe_case_count; ++i) {
        probe_result_t r = probe_run(&hardware, &probe_cases[i]);
        int y = 54 + 26 * (int)i;
        dprint(16, y, r.ok ? C_BLACK : C_RED, "%s: %s",
               probe_cases[i].name, r.ok ? "PASS" : "FAIL");
        if(!r.ok)
            dprint(240, y, C_RED, "k%u PC+%d SP=%08x",
                   (unsigned)r.ctx.kind,
                   (int)(r.ctx.out_r11 -
                         (uint32_t)(uintptr_t)probe_cases[i].bytes),
                   (unsigned)r.ctx.out_r13);
        passed += (unsigned)r.ok;
        dupdate();
    }
    btu_stop(&hardware); /* return clock, registers, SR (in trampoline) */

    dprint(16, 267, passed == probe_case_count ? C_BLACK : C_RED,
           "%u/%u hardware probes passed", passed, probe_case_count);
    dtext(16, 292, C_BLACK, "Press EXIT to return to EXWord");
    dupdate();
    while(key() != KEY_EXIT) {}
    return -2;
}
