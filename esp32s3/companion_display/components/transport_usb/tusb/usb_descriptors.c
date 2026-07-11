#include <string.h>
#include "sdkconfig.h"
#include "tusb.h"
#include "usb_descriptors.h"

static const tusb_desc_device_t s_device_descriptor = {
    .bLength = sizeof(tusb_desc_device_t), .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0201, .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON, .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE, .idVendor = CONFIG_COMPANION_USB_VID,
    .idProduct = CONFIG_COMPANION_USB_PID, .bcdDevice = 0x0100,
    .iManufacturer = 1, .iProduct = 2, .iSerialNumber = 3, .bNumConfigurations = 1,
};

static const uint8_t s_config_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, TUD_CONFIG_DESC_LEN + TUD_VENDOR_DESC_LEN, 0, 100),
    TUD_VENDOR_DESCRIPTOR(ITF_NUM_VENDOR, 4, EPNUM_VENDOR_OUT, 0x80 | EPNUM_VENDOR_IN, CFG_TUD_VENDOR_EPSIZE),
};

/* Microsoft OS 2.0 compatible-ID descriptor: Windows 8.1+ binds its built-in WinUSB driver. */
static const uint8_t s_ms_os_20_descriptor[] = {
    0x0a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x06, 0xb2, 0x00,
    0x08, 0x00, 0x01, 0x00, 0xa8, 0x00,
    0x08, 0x00, 0x02, 0x00, 0xa0, 0x00,
    0x14, 0x00, 0x03, 0x00, ITF_NUM_VENDOR, 0x00, 0x57, 0x49, 0x4e, 0x55, 0x53, 0x42, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x84, 0x00, 0x04, 0x00, 0x07, 0x00, 0x2a, 0x00,
    'D',0,'e',0,'v',0,'i',0,'c',0,'e',0,'I',0,'n',0,'t',0,'e',0,'r',0,'f',0,'a',0,'c',0,'e',0,'G',0,'U',0,'I',0,'D',0,0,0,
    '{',0,'5',0,'A',0,'1',0,'7',0,'2',0,'9',0,'1',0,'9',0,'-',0,'3',0,'B',0,'D',0,'9',0,'-',0,'4',0,'A',0,'0',0,'D',0,'-',0,'A',0,'7',0,'D',0,'A',0,'-',0,'D',0,'D',0,'3',0,'A',0,'4',0,'1',0,'9',0,'C',0,'7',0,'6',0,'4',0,'B',0,'}',0,0,0,
};

static const uint8_t s_bos_descriptor[] = {
    0x05, TUSB_DESC_BOS, 0x21, 0x00, 0x01,
    0x1c, 0x10, 0x05, 0x00, 0xd8, 0xdd, 0x60, 0xdf, 0x45, 0x89, 0x4c, 0xc7,
    0x9c, 0xd2, 0x65, 0x9d, 0x9e, 0x64, 0x8a, 0x9f, 0x00, 0x00, 0x03, 0x06,
    0xb2, 0x00, 0x00, 0x00,
};

const uint8_t *tud_descriptor_device_cb(void) { return (const uint8_t *)&s_device_descriptor; }
const uint8_t *tud_descriptor_configuration_cb(uint8_t index) { (void)index; return s_config_descriptor; }
const uint8_t *tud_descriptor_bos_cb(void) { return s_bos_descriptor; }

static const char *const s_strings[] = { (const char[]){0x09, 0x04}, "Companion Display", "Companion Display USB", "companion-display" };
static uint16_t s_string[64];
const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    uint8_t count = 0;
    if (index == 0) { memcpy(&s_string[1], s_strings[0], 2); count = 1; }
    else if (index < TU_ARRAY_SIZE(s_strings)) { const char *str = s_strings[index]; count = strlen(str); for (uint8_t i = 0; i < count; ++i) s_string[i + 1] = str[i]; }
    else return NULL;
    s_string[0] = (TUSB_DESC_STRING << 8) | (2 * count + 2);
    return s_string;
}

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request)
{
    if (stage == CONTROL_STAGE_SETUP && request->bmRequestType_bit.type == TUSB_REQ_TYPE_VENDOR &&
        request->bRequest == 0x20 && request->wIndex == 0x07) {
        return tud_control_xfer(rhport, request, (void *)s_ms_os_20_descriptor, sizeof(s_ms_os_20_descriptor));
    }
    return false;
}
