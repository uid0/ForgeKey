#ifndef FORGEKEY_COMMAND_VALIDATION_H
#define FORGEKEY_COMMAND_VALIDATION_H

#include <stdbool.h>
#include "cJSON.h"

typedef struct {
    bool ok;
    const char* error;
    const char* command_id;
    const char* nonce;
} command_validation_result_t;

void command_validation_begin(void);
command_validation_result_t command_validation_validate(cJSON* doc, const char* device_mac);
void command_validation_remember_accepted(const command_validation_result_t* result);

#endif
