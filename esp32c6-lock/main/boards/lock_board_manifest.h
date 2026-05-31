#ifndef FORGEKEY_LOCK_BOARD_MANIFEST_H
#define FORGEKEY_LOCK_BOARD_MANIFEST_H

#include <stdbool.h>
#include "cJSON.h"

typedef struct {
    int gpio;
    const char* owner;
    const char* signal;
    const char* pull;
    bool output;
    bool unsafe;
} lock_pin_claim_t;

typedef struct {
    const char* id;
    const char* name;
    const lock_pin_claim_t* pins;
    unsigned pin_count;
    const int* boot_strap_pins;
    unsigned boot_strap_pin_count;
    const int* adc_pins;
    unsigned adc_pin_count;
} lock_board_manifest_t;

const lock_board_manifest_t* lock_board_manifest_current(void);
bool lock_board_manifest_check(void);
void lock_board_manifest_add_health_json(cJSON* root);

#endif
