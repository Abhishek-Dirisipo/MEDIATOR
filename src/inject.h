#pragma once
#include <stdint.h>
#include <stdbool.h>

void inject_init(void);
void inject_start(const char *text, uint32_t len, uint32_t speed_ms, bool jitter);
void inject_task(void);
bool inject_is_active(void);
