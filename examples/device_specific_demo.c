// Example: Device-specific callback optimization demo
// Shows how to avoid complex if-chains by registering callbacks per device type
#include "asyncinput.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <string.h>

static long long now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

// Device-specific callback for keyboards
static void keyboard_handler(const struct ni_event *ev, const struct ni_device_info *device, void *user_data) {
    (void)user_data;
    if (ni_is_key_event(ev) && ev->value == 1) {  // Key press only
        printf("KEYBOARD [%s]: Key 0x%x pressed (latency: %.2f μs)\n",
               device->name, ev->code, (now_ns() - ev->timestamp_ns) / 1000.0);
    }
}

// Device-specific callback for mice
static void mouse_handler(const struct ni_event *ev, const struct ni_device_info *device, void *user_data) {
    (void)user_data;
    if (ni_is_rel_event(ev)) {
        if (ev->code == NI_REL_X || ev->code == NI_REL_Y) {
            printf("MOUSE [%s]: %s movement %+d (latency: %.2f μs)\n",
                   device->name, 
                   (ev->code == NI_REL_X) ? "X" : "Y", 
                   ev->value,
                   (now_ns() - ev->timestamp_ns) / 1000.0);
        }
    } else if (ni_button_down(ev)) {
        const char *btn_name = "UNKNOWN";
        switch (ev->code) {
            case NI_BTN_LEFT: btn_name = "LEFT"; break;
            case NI_BTN_RIGHT: btn_name = "RIGHT"; break;
            case NI_BTN_MIDDLE: btn_name = "MIDDLE"; break;
        }
        printf("MOUSE [%s]: %s button pressed (latency: %.2f μs)\n",
               device->name, btn_name, (now_ns() - ev->timestamp_ns) / 1000.0);
    }
}

// Device filter to identify device types
static int device_type_filter(const struct ni_device_info *info, void *user_data) {
    const char *device_type = (const char*)user_data;
    
    if (strcmp(device_type, "keyboard") == 0) {
        // Match keyboards (usually contain "keyboard" in name or specific vendors)
        return (strstr(info->name, "keyboard") != NULL || 
                strstr(info->name, "Keyboard") != NULL ||
                info->vendor == 0x04f2);  // Example: Chicony vendor
    } else if (strcmp(device_type, "mouse") == 0) {
        // Match mice (contain "mouse" in name or specific vendors)
        return (strstr(info->name, "mouse") != NULL || 
                strstr(info->name, "Mouse") != NULL ||
                info->vendor == 0x046d);  // Example: Logitech vendor
    }
    return 0;  // Don't match other devices
}

// Global fallback handler moved to file scope (C89/C99 portable)
static void global_fallback_handler(const struct ni_event *ev, void *user_data) {
    // This is the complex approach we want to avoid:
    // Need to look up device info and branch on device type
    (void)user_data;
    
    // Simulate what device-specific callbacks would avoid:
    // Complex branching logic that hurts performance
    if (ni_is_key_event(ev) && ev->value == 1) {
        printf("GLOBAL: Key event 0x%x (no device context available)\n", ev->code);
    } else if (ni_is_rel_event(ev) && (ev->code == NI_REL_X || ev->code == NI_REL_Y)) {
        printf("GLOBAL: %s movement %+d (no device context)\n",
               (ev->code == NI_REL_X) ? "X" : "Y", ev->value);
    } else if (ni_button_down(ev)) {
        printf("GLOBAL: Button 0x%x pressed (no device context)\n", ev->code);
    }
}

int main(int argc, char** argv) {
    int seconds = 10;
    if (argc > 1) { 
        int s = atoi(argv[1]); 
        if (s > 0) seconds = s; 
    }
    
    printf("Device-specific callback demo for %d seconds\n", seconds);
    printf("This avoids complex device type checks in callback code\n\n");
    
    if (ni_init(0) != 0) {
        fprintf(stderr, "ni_init failed (permissions for /dev/input/event*?)\n");
        return 1;
    }
    
    // Register device-specific callbacks instead of one global callback
    
    // First, register callback for all keyboards
    // (In the future API, this would work. For now, this is conceptual)
    /*
    ni_set_device_filter(device_type_filter, "keyboard");
    int kb_cb_id = ni_register_device_callback(-1, keyboard_handler, NULL, 
                                               NI_CB_FLAG_HIGH_PRIORITY);
    
    // Then register callback for all mice
    ni_set_device_filter(device_type_filter, "mouse");
    int mouse_cb_id = ni_register_device_callback(-1, mouse_handler, NULL,
                                                  NI_CB_FLAG_HIGH_PRIORITY);
    */
    
    // For now, fall back to global callback with device type detection
    // This demonstrates the benefit of the future device-specific API
    
    if (ni_register_callback(global_fallback_handler, NULL, 0) != 0) {
        fprintf(stderr, "ni_register_callback failed\n");
        ni_shutdown();
        return 1;
    }
    
    long long start = now_ns();
    printf("Listening for input events...\n");
    printf("(Note: Device-specific callbacks would eliminate the branching overhead)\n\n");
    
    while (now_ns() - start < (long long)seconds * 1000000000LL) {
        usleep(10000);  // 10ms
    }
    
    printf("\nShutting down...\n");
    
    // In future API: ni_unregister_device_callback(kb_cb_id);
    // In future API: ni_unregister_device_callback(mouse_cb_id);
    
    ni_shutdown();
    return 0;
}
