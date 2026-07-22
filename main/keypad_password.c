#include "app_common_interfaces.h"

#define TAG "KEYPAD_PASSWORD"

#define KEYPAD_PASSWORD "P123456E"

static size_t password_index;
static bool password_unlocked;

static void keypad_password_reset(void)
{
    password_index = 0;
}

void keypad_password_handle_key_event(char key, bool pressed)
{
    const char *password = KEYPAD_PASSWORD;

    if (!pressed || password_unlocked) {
        return;
    }

    if (key == password[password_index]) {
        password_index++;
        ESP_LOGI(TAG, "Password progress %u/%u",
                 (unsigned int)password_index,
                 (unsigned int)strlen(password));

        if (password[password_index] == '\0') {
            password_unlocked = true;
            keypad_password_reset();
            ESP_LOGW(TAG, "Password accepted: switch to external physical keypad");
            virtual_keypad_set_external_physical(true);
            keypad_master_scan_disable_for_external_physical_keypad();
        }
        return;
    }

    if (key == password[0]) {
        password_index = 1;
        ESP_LOGI(TAG, "Password progress 1/%u",
                 (unsigned int)strlen(password));
    } else if (password_index != 0) {
        ESP_LOGW(TAG, "Password mismatch at key=%c, reset sequence", key);
        keypad_password_reset();
    }
}

bool keypad_password_is_unlocked(void)
{
    return password_unlocked;
}
