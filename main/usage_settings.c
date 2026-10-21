#include "usage_settings.h"

#include <string.h>
#include "nvs.h"
#include "nvs_flash.h"

#define SETTINGS_NAMESPACE "usage_dash"

int usage_settings_get(const char *key, char *out, size_t capacity) {
    if (!key || !out || capacity == 0) return -1;
    nvs_handle_t handle;
    if (nvs_open(SETTINGS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return -1;
    size_t length = capacity;
    esp_err_t error = nvs_get_str(handle, key, out, &length);
    nvs_close(handle);
    if (error != ESP_OK) { out[0] = '\0'; return -1; }
    return 0;
}

int usage_settings_set(const char *key, const char *value) {
    if (!key || !value || strlen(key) > 15 || strlen(value) > 255) return -1;
    nvs_handle_t handle;
    if (nvs_open(SETTINGS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return -1;
    esp_err_t error = nvs_set_str(handle, key, value);
    if (error == ESP_OK) error = nvs_commit(handle);
    nvs_close(handle);
    return error == ESP_OK ? 0 : -1;
}
