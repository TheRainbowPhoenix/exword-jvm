#include <gint/display.h>
#include <gint/keyboard.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "btu.h"
#include "probes.h"
#include "loader.h"
#include "vm.h"

#define SCREEN_ROWS 18
#define SCREEN_COLUMNS 69

static btu_t hardware;
static char lines[SCREEN_ROWS][SCREEN_COLUMNS + 1];
static unsigned line_no, col_no;

static int key(void)
{
    for(;;) {
        key_event_t ev = waitevent(NULL);
        if(ev.type == KEYEV_DOWN) return ev.key;
    }
}
static void title(const char *subtitle)
{
    dclear(C_WHITE);
    dtext(12, 8, C_BLACK, "EXWord Java VM / SH7305 BTU");
    dtext(12, 28, C_BLACK, subtitle);
    dupdate();
}
static void draw_log(void)
{
    dclear(C_WHITE);
    dtext(8, 5, C_BLACK, "EXWord JVM  [real hardware]");
    for(unsigned row = 0; row < SCREEN_ROWS; ++row) {
        unsigned i = (line_no + 1 + row) % SCREEN_ROWS;
        dtext(8, 30 + 14 * (int)row, C_BLACK, lines[i]);
    }
    dupdate();
}
static void output(void *ctx, const char *bytes, size_t count)
{
    (void)ctx;
    for(size_t i = 0; i < count; ++i) {
        unsigned char c = (unsigned char)bytes[i];
        if(c == '\n' || col_no == SCREEN_COLUMNS) {
            line_no = (line_no + 1) % SCREEN_ROWS;
            lines[line_no][0] = 0;
            col_no = 0;
            draw_log();
        }
        if(c != '\n') {
            lines[line_no][col_no++] = (c >= 32 && c < 127) ? c : '?';
            lines[line_no][col_no] = 0;
        }
    }
}
static void log_text(const char *s)
{
    output(NULL, s, strlen(s));
    output(NULL, "\n", 1);
}

/* Retain the user's 8/8 hardware diagnostic independently from the JVM. */
static void hardware_probes(void)
{
    title("HARDWARE: verifying J_JP @R1");
    if(!btu_start(&hardware)) {
        dtext(16, 75, C_RED, "BTU unavailable / register mismatch");
        dtext(16, 290, C_BLACK, "EXIT to return");
        dupdate();
        while(key() != KEY_EXIT) {}
        return;
    }
    unsigned passed = 0;
    dclear(C_WHITE);
    dtext(12, 7, C_BLACK, "SH7305 BTU / executable bytecodes");
    for(unsigned i = 0; i < probe_case_count; ++i) {
        probe_result_t result = probe_run(&hardware, &probe_cases[i]);
        passed += result.ok != 0;
        dprint(12, 36 + 26 * (int)i,
               result.ok ? C_BLACK : C_RED,
               "%s: %s", probe_cases[i].name,
               result.ok ? "PASS" : "FAIL");
        dupdate();
    }
    btu_stop(&hardware);
    dprint(12, 258, passed == probe_case_count ? C_BLACK : C_RED,
           "%u / %u hardware tests passed", passed, probe_case_count);
    dtext(12, 290, C_BLACK, "EXIT to return");
    dupdate();
    while(key() != KEY_EXIT) {}
}
static int select_main(jvm_loader_t *loader)
{
    int runnable[JVM_FILES_MAX];
    int count = 0, index = 0;
    for(unsigned i = 0; i < loader->count; ++i)
        if(loader->entries[i].has_main)
            runnable[count++] = i;
    if(!count) {
        title("No main(String[]) classes");
        dtext(12, 83, C_RED, "Class files found, but none has main().");
        dtext(12, 293, C_BLACK, "EXIT to return");
        dupdate();
        while(key() != KEY_EXIT) {}
        return -1;
    }
    for(int i = 0; i < count; ++i)
        if(!strcmp(loader->entries[runnable[i]].internal_name, "Main"))
            index = i; /* favor upstream cpjvm-rs suite */
    for(;;) {
        dclear(C_WHITE);
        dtext(12, 8, C_BLACK, "Choose Java main class");
        dprint(12, 28, C_BLACK, "%d runnable / %u class files",
               count, loader->count);
        int first = index > 14 ? index - 14 : 0;
        for(int n = first; n < count && n < first + 15; ++n) {
            const jvm_file_t *f = &loader->entries[runnable[n]];
            dprint(12, 50 + 15 * (n - first),
                   n == index ? C_BLUE : C_BLACK,
                   "%c %.52s", n == index ? '>' : ' ', f->internal_name);
        }
        dtext(12, 286, C_BLACK, "UP/DOWN select  EXE run  EXIT back");
        dupdate();
        int k = key();
        if(k == KEY_EXIT) return -1;
        if(k == KEY_DOWN && index + 1 < count) ++index;
        if(k == KEY_UP && index) --index;
        if(k == KEY_EXE) return runnable[index];
    }
}
static void run_class(jvm_loader_t *loader, const char *class_name)
{
    memset(lines, 0, sizeof lines);
    line_no = col_no = 0;
    draw_log();
    if(!btu_start(&hardware)) {
        log_text("[BTU unavailable: software interpreter]");
    } else {
        log_text("[BTU enabled: real J_JP bytecode execution]");
    }
    jvm_vm_t *vm = calloc(1, sizeof *vm);
    if(!vm) {
        if(hardware.active) btu_stop(&hardware);
        log_text("Cannot allocate Java VM");
        while(key() != KEY_EXIT) {}
        return;
    }
    jvm_vm_init(vm, loader, hardware.active ? &hardware : NULL, output, NULL);
    int rc = jvm_vm_run(vm, class_name);
    if(hardware.active) btu_stop(&hardware);
    char msg[210];
    if(rc) {
        snprintf(msg, sizeof msg, "VM ERROR: %s", vm->error);
        log_text(msg);
    } else log_text("[Java main returned successfully]");
    snprintf(msg, sizeof msg, "BTU %u, C steps %u, classes %u, objects %u",
             (unsigned)vm->stats.btu_entries,
             (unsigned)vm->stats.sw_steps,
             (unsigned)vm->stats.classes_loaded,
             (unsigned)vm->stats.allocations);
    log_text(msg);
    jvm_vm_destroy(vm);
    free(vm);
    log_text("[press EXIT to return]");
    draw_log();
    while(key() != KEY_EXIT) {}
}
int main(void)
{
    for(;;) {
        title("EXE: scan and run Java class files");
        dtext(12, 76, C_BLACK, "Look for .cls / .cla / .class beside D01");
        dtext(12, 103, C_BLACK, "Class identity comes from JVM this_class");
        dtext(12, 143, C_BLACK, "EXE: Java launcher");
        dtext(12, 167, C_BLACK, "H or F1: repeat 8 hardware probes");
        dtext(12, 289, C_BLACK, "EXIT: return to EXWord");
        dupdate();
        int k = key();
        if(k == KEY_EXIT) return -2;
        if(k == KEY_F1 || k == KEY_EX_H) { hardware_probes(); continue; }
        if(k != KEY_EXE) continue;

        title("Searching EXWord filesystem...");
        jvm_loader_t *loader = calloc(1, sizeof *loader);
        if(!loader) {
            title("Not enough memory for class index");
            while(key() != KEY_EXIT) {}
            continue;
        }
        int count = jvm_loader_autodetect(loader);
        if(count <= 0) {
            title("No Java class files found");
            dtext(12, 76, C_RED, loader->status);
            dtext(12, 103, C_BLACK, "Copy .cls files into BTPRF directory");
            dtext(12, 291, C_BLACK, "EXIT to return");
            dupdate();
            while(key() != KEY_EXIT) {}
        } else {
            int selected = select_main(loader);
            if(selected >= 0) {
                char name[JVM_CLASSNAME_MAX];
                strcpy(name, loader->entries[selected].internal_name);
                run_class(loader, name);
            }
        }
        free(loader);
    }
}
