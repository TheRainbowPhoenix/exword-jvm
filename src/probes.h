#ifndef EXWORD_PROBES_H
#define EXWORD_PROBES_H
#include "btu.h"

typedef struct {
    const char *name;
    const uint8_t *bytes;
    unsigned length;
    unsigned stop_pc;
    unsigned stack_words;
    uint32_t top;
    uint32_t next_top; /* used for long results, otherwise zero */
    uint32_t local0;
    uint32_t event_code;
    unsigned kind;
    int check_local0;
    int check_next_top;
} probe_case_t;

extern const probe_case_t probe_cases[];
extern const unsigned probe_case_count;

typedef struct {
    int ok;
    btu_context_t ctx;
    uint32_t top;
    uint32_t next_top;
    uint32_t local0;
} probe_result_t;

probe_result_t probe_run(btu_t *b, const probe_case_t *tc);

#endif
