#pragma once

#include <stddef.h>

int usage_settings_get(const char *key, char *out, size_t capacity);
int usage_settings_set(const char *key, const char *value);
