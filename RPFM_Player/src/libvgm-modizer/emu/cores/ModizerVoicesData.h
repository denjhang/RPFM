// Stub: ModizerVoicesData.h — scope voice channel mapping
#pragma once
#include <stdint.h>

#define SCOPE_MAX_GROUPS 8

typedef struct {
    int type;
    int count;
    int start;
    const char *label;
} ScopeChGroup;

typedef struct {
    const char *chip_name;
    int chip_inst;
    int num_channels;
    int slot_base;
    int active;
    int num_groups;
    ScopeChGroup groups[SCOPE_MAX_GROUPS];
} ScopeChipSlot;

#ifdef __cplusplus
extern "C" {
#endif

static ScopeChipSlot s_scopeSlots[16] = {};
static int s_scopeChipCount = 0;

static inline ScopeChipSlot* scope_find_slot(const char* name, int inst) {
    for (int i = 0; i < s_scopeChipCount; i++) {
        if (s_scopeSlots[i].active && s_scopeSlots[i].chip_inst == inst)
            return &s_scopeSlots[i];
    }
    // Auto-register
    if (s_scopeChipCount < 16) {
        ScopeChipSlot* s = &s_scopeSlots[s_scopeChipCount++];
        s->chip_name = name;
        s->chip_inst = inst;
        s->num_channels = 5;
        s->slot_base = s_scopeChipCount * 5;
        s->active = 1;
        s->num_groups = 0;
        return s;
    }
    return nullptr;
}

#ifdef __cplusplus
}
#endif
