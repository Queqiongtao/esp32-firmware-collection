#include <string.h>

#include "sdkconfig.h"
#include "tusb.h"
#include "usb_descriptors.h"

#define STRINGIFY_INNER(value) #value
#define STRINGIFY(value) STRINGIFY_INNER(value)

#define UDISP_CONFIG_STRING \
    CONFIG_IDF_TARGET "udisp0_R368x448_Ejpg" STRINGIFY(CONFIG_USB_EXTEND_SCREEN_JPEG_QUALITY) \
    "_Fps" STRINGIFY(CONFIG_USB_EXTEND_SCREEN_MAX_FPS) "_Bl" STRINGIFY(CONFIG_USB_EXTEND_SCREEN_FRAME_LIMIT_KB)

static const tusb_desc_device_t s_device_descriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_UNSPECIFIED,
    .bDeviceSubClass = TUSB_CLASS_UNSPECIFIED,
    .bDeviceProtocol = TUSB_CLASS_UNSPECIFIED,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = CONFIG_TUSB_VID,
    .idProduct = CONFIG_TUSB_PID,
    .bcdDevice = 0x0101,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

const uint8_t s_configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, TUD_CONFIG_DESC_LEN + TUD_VENDOR_DESC_LEN, 0, 100),
    TUD_VENDOR_DESCRIPTOR(ITF_NUM_VENDOR, 0x04, EPNUM_VENDOR, 0x80 | EPNUM_VENDOR, CFG_TUD_VENDOR_EPSIZE),
};

const uint8_t *tud_descriptor_device_cb(void)
{
    return (const uint8_t *)&s_device_descriptor;
}

const uint8_t *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return s_configuration_descriptor;
}

static const char *const s_string_descriptors[] = {
    (const char[]){0x09, 0x04},
    CONFIG_TUSB_MANUFACTURER,
    UDISP_CONFIG_STRING,
    "amoled-1-8",
    UDISP_CONFIG_STRING,
};

static uint16_t s_descriptor_string[64];

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    uint8_t count = 0;
    if (index == 0) {
        memcpy(&s_descriptor_string[1], s_string_descriptors[0], 2);
        count = 1;
    } else {
        if (index >= sizeof(s_string_descriptors) / sizeof(s_string_descriptors[0])) {
            return NULL;
        }
        const char *string = s_string_descriptors[index];
        count = strlen(string);
        if (count > 63) {
            count = 63;
        }
        for (uint8_t i = 0; i < count; ++i) {
            s_descriptor_string[1 + i] = string[i];
        }
    }
    s_descriptor_string[0] = (TUSB_DESC_STRING << 8) | (2 * count + 2);
    return s_descriptor_string;
}
