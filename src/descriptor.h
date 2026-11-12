/*
 * HID report descriptor for one "Bindscript Panel": a joystick with
 * 8 axes (X, Y, Z, Rx, Ry, Rz, Slider, Dial) of 16 bits and 128 buttons.
 * Games expect this conventional DirectInput shape, so add more panels
 * for more controls and keep each panel this shape.
 *
 * Input report 1 holds the report id, 8 axes and 16 bytes of buttons:
 * 33 bytes. The axis range is 0 to 65535, with the centre at 32768.
 */
#pragma once

/* The kernel driver and the user-mode feeder share this header. Kernel
 * builds refuse stdint.h, so the kernel build defines its own types. */
#ifdef _KERNEL_MODE
typedef unsigned char bsv_u8;
typedef unsigned short bsv_u16;
#else
#include <stdint.h>
typedef uint8_t bsv_u8;
typedef uint16_t bsv_u16;
#endif

#define BSV_REPORT_ID 1
#define BSV_OUTPUT_REPORT_ID 2
#define BSV_AXES 8
#define BSV_BUTTONS 128

#pragma pack(push, 1)
typedef struct BSV_INPUT_REPORT {
    bsv_u8 report_id;               /* BSV_REPORT_ID */
    bsv_u16 axes[BSV_AXES];         /* X Y Z Rx Ry Rz Slider Dial */
    bsv_u8 buttons[BSV_BUTTONS / 8];
} BSV_INPUT_REPORT;

/* The feeder writes this output report to the HID collection with
 * WriteFile. The driver copies the payload into the next input report.
 * The payload is the same as BSV_INPUT_REPORT; only the report id differs. */
typedef struct BSV_OUTPUT_REPORT {
    bsv_u8 report_id;               /* BSV_OUTPUT_REPORT_ID */
    bsv_u16 axes[BSV_AXES];
    bsv_u8 buttons[BSV_BUTTONS / 8];
} BSV_OUTPUT_REPORT;
#pragma pack(pop)

static const bsv_u8 bsv_report_descriptor[] = {
    0x05, 0x01,        /* Usage Page (Generic Desktop) */
    0x09, 0x04,        /* Usage (Joystick) */
    0xA1, 0x01,        /* Collection (Application) */
    0x85, BSV_REPORT_ID, /*   Report ID (1) */
    /* -- 8 axes, 16-bit -- */
    0x05, 0x01,        /*   Usage Page (Generic Desktop) */
    0x09, 0x30,        /*   Usage (X) */
    0x09, 0x31,        /*   Usage (Y) */
    0x09, 0x32,        /*   Usage (Z) */
    0x09, 0x33,        /*   Usage (Rx) */
    0x09, 0x34,        /*   Usage (Ry) */
    0x09, 0x35,        /*   Usage (Rz) */
    0x09, 0x36,        /*   Usage (Slider) */
    0x09, 0x37,        /*   Usage (Dial) */
    0x15, 0x00,        /*   Logical Minimum (0) */
    0x27, 0xFF, 0xFF, 0x00, 0x00, /* Logical Maximum (65535) */
    0x75, 0x10,        /*   Report Size (16) */
    0x95, 0x08,        /*   Report Count (8) */
    0x81, 0x02,        /*   Input (Data, Var, Abs) */
    /* -- 128 buttons -- */
    0x05, 0x09,        /*   Usage Page (Button) */
    0x19, 0x01,        /*   Usage Minimum (1) */
    0x29, 0x80,        /*   Usage Maximum (128) */
    0x15, 0x00,        /*   Logical Minimum (0) */
    0x25, 0x01,        /*   Logical Maximum (1) */
    0x75, 0x01,        /*   Report Size (1) */
    0x95, 0x80,        /*   Report Count (128) */
    0x81, 0x02,        /*   Input (Data, Var, Abs) */
    /* -- feeder frame: vendor-page output report, 32 opaque bytes -- */
    0x85, BSV_OUTPUT_REPORT_ID, /* Report ID (2) */
    0x06, 0x00, 0xFF,  /*   Usage Page (Vendor Defined) */
    0x09, 0x01,        /*   Usage (Vendor 0x01) */
    0x15, 0x00,        /*   Logical Minimum (0) */
    0x26, 0xFF, 0x00,  /*   Logical Maximum (255) */
    0x75, 0x08,        /*   Report Size (8) */
    0x95, 0x20,        /*   Report Count (32) */
    0x91, 0x02,        /*   Output (Data, Var, Abs) */
    0xC0,              /* End Collection */
};
