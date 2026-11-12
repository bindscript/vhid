/*
 * bindscript-vhid: a KMDF HID minidriver that follows the vhidmini2
 * sample. The driver is a lower filter under mshidkmdf.sys, which is
 * the function driver (Include=MsHidKmdf.inf). HIDCLASS sends the
 * IOCTL_HID_* requests here, and this driver answers the identity
 * queries, so the HID child device path carries our own VID and PID.
 *
 * Do not port this to the Virtual HID Framework (VHF). VHF names every
 * child HID\HID_DEVICE_SYSTEM_VHF. Some games read the VID and PID from
 * the device path, and they then see every VHF device as one device.
 *
 * The feeder opens the HID collection and writes a BSV_OUTPUT_REPORT
 * (descriptor.h). HIDCLASS delivers it as IOCTL_HID_WRITE_REPORT. The
 * driver copies the payload into the input state and completes a
 * waiting IOCTL_HID_READ_REPORT. Do not add a custom device interface:
 * an open on this devnode reaches the HIDCLASS FDO, not this driver.
 */

#include <ntddk.h>
#include <wdf.h>
#include <hidport.h>

#include "descriptor.h"

static const HID_DESCRIPTOR bsv_hid_descriptor = {
    0x09,   /* bLength */
    0x21,   /* bDescriptorType (HID) */
    0x0100, /* bcdHID 1.0 */
    0x00,   /* bCountry */
    0x01,   /* bNumDescriptors */
    {
        0x22,                          /* report descriptor type */
        sizeof(bsv_report_descriptor), /* wReportLength */
    },
};

/* 0x1209/0x0001 is the pid.codes test PID. It is not unique and must
 * not ship. Replace it with the allocated PID before any release. */
#define BSV_VID 0x1209
#define BSV_PID 0x0001
#define BSV_VERSION 0x0001

typedef struct DEVICE_CONTEXT {
    WDFQUEUE ManualQueue;      /* parked IOCTL_HID_READ_REPORT requests */
    BSV_INPUT_REPORT Current;  /* last state written by the feeder */
} DEVICE_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, GetDeviceContext);

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD EvtDeviceAdd;
EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL EvtIoInternalDeviceControl;

/* Records the status of each start step in the device's hardware key.
 * Read the values under Enum\...\Device Parameters after a failed start. */
static VOID
BsvLog(_In_ WDFDEVICE Device, _In_ PCUNICODE_STRING step, _In_ NTSTATUS status)
{
    WDFKEY key;
    if (!NT_SUCCESS(WdfDeviceOpenRegistryKey(Device, PLUGPLAY_REGKEY_DEVICE,
                                             KEY_SET_VALUE, WDF_NO_OBJECT_ATTRIBUTES,
                                             &key))) {
        return;
    }
    (void)WdfRegistryAssignULong(key, step, (ULONG)status);
    WdfRegistryClose(key);
}

NTSTATUS
DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, EvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES,
                           &config, WDF_NO_HANDLE);
}

NTSTATUS
EvtDeviceAdd(_In_ WDFDRIVER Driver, _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    UNREFERENCED_PARAMETER(Driver);

    /* Filter below mshidkmdf: it owns the FDO role and power policy. */
    WdfFdoInitSetFilter(DeviceInit);

    WDF_OBJECT_ATTRIBUTES attrs;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attrs, DEVICE_CONTEXT);

    WDFDEVICE device;
    NTSTATUS status = WdfDeviceCreate(&DeviceInit, &attrs, &device);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    DECLARE_CONST_UNICODE_STRING(kQueue, L"BsvQueue");
    DECLARE_CONST_UNICODE_STRING(kManualQueue, L"BsvManualQueue");

    DEVICE_CONTEXT* ctx = GetDeviceContext(device);
    ctx->Current.report_id = BSV_REPORT_ID;
    for (int i = 0; i < BSV_AXES; i++) {
        ctx->Current.axes[i] = 32768; /* centre */
    }

    /* Sequential dispatch serialises access to ctx->Current. READ_REPORT
     * moves to the manual queue at once, so it never blocks the queue. */
    WDF_IO_QUEUE_CONFIG qcfg;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&qcfg, WdfIoQueueDispatchSequential);
    qcfg.EvtIoInternalDeviceControl = EvtIoInternalDeviceControl;
    status = WdfIoQueueCreate(device, &qcfg, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
    BsvLog(device, &kQueue, status);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_IO_QUEUE_CONFIG mcfg;
    WDF_IO_QUEUE_CONFIG_INIT(&mcfg, WdfIoQueueDispatchManual);
    status = WdfIoQueueCreate(device, &mcfg, WDF_NO_OBJECT_ATTRIBUTES,
                              &ctx->ManualQueue);
    BsvLog(device, &kManualQueue, status);
    return status;
}

static NTSTATUS
RequestCopyFromBuffer(_In_ WDFREQUEST Request, _In_ const VOID* Source,
                      _In_ size_t Length)
{
    WDFMEMORY memory;
    NTSTATUS status = WdfRequestRetrieveOutputMemory(Request, &memory);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    size_t outLength;
    WdfMemoryGetBuffer(memory, &outLength);
    if (outLength < Length) {
        return STATUS_INVALID_BUFFER_SIZE;
    }
    status = WdfMemoryCopyFromBuffer(memory, 0, (PVOID)Source, Length);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    WdfRequestSetInformation(Request, Length);
    return status;
}

/* HID minidriver IOCTLs carry a HID_XFER_PACKET at Irp->UserBuffer,
 * whatever buffering method the IOCTL declares. The vhidmini2 sample
 * (kmdf/util.c) reads it the same way. */
static NTSTATUS
GetXferPacket(_In_ WDFREQUEST Request, _In_ BOOLEAN Write,
              _Out_ HID_XFER_PACKET* Packet)
{
    WDF_REQUEST_PARAMETERS params;
    WDF_REQUEST_PARAMETERS_INIT(&params);
    WdfRequestGetParameters(Request, &params);

    size_t len = Write ? params.Parameters.DeviceIoControl.InputBufferLength
                       : params.Parameters.DeviceIoControl.OutputBufferLength;
    if (len < sizeof(HID_XFER_PACKET)) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    RtlCopyMemory(Packet, WdfRequestWdmGetIrp(Request)->UserBuffer,
                  sizeof(HID_XFER_PACKET));
    return STATUS_SUCCESS;
}

/* Copies one feeder frame (WRITE_REPORT or SET_OUTPUT_REPORT) into the
 * input state and completes one waiting read. */
static NTSTATUS
BsvAcceptFeederFrame(_In_ DEVICE_CONTEXT* Ctx, _In_ WDFREQUEST Request)
{
    HID_XFER_PACKET packet;
    NTSTATUS status = GetXferPacket(Request, TRUE, &packet);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (packet.reportId != BSV_OUTPUT_REPORT_ID ||
        packet.reportBufferLen < sizeof(BSV_OUTPUT_REPORT)) {
        return STATUS_INVALID_PARAMETER;
    }
    const BSV_OUTPUT_REPORT* frame = (const BSV_OUTPUT_REPORT*)packet.reportBuffer;
    RtlCopyMemory(Ctx->Current.axes, frame->axes, sizeof(Ctx->Current.axes));
    RtlCopyMemory(Ctx->Current.buttons, frame->buttons, sizeof(Ctx->Current.buttons));

    WDFREQUEST reader;
    if (NT_SUCCESS(WdfIoQueueRetrieveNextRequest(Ctx->ManualQueue, &reader))) {
        NTSTATUS rs = RequestCopyFromBuffer(reader, &Ctx->Current,
                                            sizeof(Ctx->Current));
        WdfRequestComplete(reader, rs);
    }
    WdfRequestSetInformation(Request, sizeof(BSV_OUTPUT_REPORT));
    return STATUS_SUCCESS;
}

static NTSTATUS
BsvGetString(_In_ WDFREQUEST Request)
{
    /* The string id arrives as the pointer value of Type3InputBuffer, not
     * as data it points to: id in the low word, language in the high word. */
    WDF_REQUEST_PARAMETERS params;
    WDF_REQUEST_PARAMETERS_INIT(&params);
    WdfRequestGetParameters(Request, &params);
    ULONG stringId =
        PtrToUlong(params.Parameters.DeviceIoControl.Type3InputBuffer) & 0xffff;

    static const WCHAR manufacturer[] = L"Bindscript";
    static const WCHAR product[] = L"Bindscript Panel";
    static const WCHAR serial[] = L"BSP1";

    switch (stringId) {
    case HID_STRING_ID_IMANUFACTURER:
        return RequestCopyFromBuffer(Request, manufacturer, sizeof(manufacturer));
    case HID_STRING_ID_IPRODUCT:
        return RequestCopyFromBuffer(Request, product, sizeof(product));
    case HID_STRING_ID_ISERIALNUMBER:
        return RequestCopyFromBuffer(Request, serial, sizeof(serial));
    default:
        return STATUS_INVALID_PARAMETER;
    }
}

VOID
EvtIoInternalDeviceControl(_In_ WDFQUEUE Queue, _In_ WDFREQUEST Request,
                           _In_ size_t OutputBufferLength,
                           _In_ size_t InputBufferLength,
                           _In_ ULONG IoControlCode)
{
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    WDFDEVICE device = WdfIoQueueGetDevice(Queue);
    DEVICE_CONTEXT* ctx = GetDeviceContext(device);
    NTSTATUS status;

    switch (IoControlCode) {
    case IOCTL_HID_GET_DEVICE_DESCRIPTOR:
        status = RequestCopyFromBuffer(Request, &bsv_hid_descriptor,
                                       bsv_hid_descriptor.bLength);
        break;

    case IOCTL_HID_GET_REPORT_DESCRIPTOR:
        status = RequestCopyFromBuffer(Request, bsv_report_descriptor,
                                       sizeof(bsv_report_descriptor));
        break;

    case IOCTL_HID_GET_DEVICE_ATTRIBUTES: {
        HID_DEVICE_ATTRIBUTES attributes;
        RtlZeroMemory(&attributes, sizeof(attributes));
        attributes.Size = sizeof(attributes);
        attributes.VendorID = BSV_VID;
        attributes.ProductID = BSV_PID;
        attributes.VersionNumber = BSV_VERSION;
        status = RequestCopyFromBuffer(Request, &attributes, sizeof(attributes));
        break;
    }

    case IOCTL_HID_READ_REPORT:
        status = WdfRequestForwardToIoQueue(Request, ctx->ManualQueue);
        if (NT_SUCCESS(status)) {
            return; /* completed by the next feeder frame */
        }
        break;

    case IOCTL_HID_WRITE_REPORT:
    case IOCTL_HID_SET_OUTPUT_REPORT:
        status = BsvAcceptFeederFrame(ctx, Request);
        break;

    case IOCTL_HID_GET_INPUT_REPORT: {
        HID_XFER_PACKET packet;
        status = GetXferPacket(Request, FALSE, &packet);
        if (NT_SUCCESS(status)) {
            if (packet.reportBufferLen < sizeof(BSV_INPUT_REPORT)) {
                status = STATUS_INVALID_BUFFER_SIZE;
            } else {
                RtlCopyMemory(packet.reportBuffer, &ctx->Current,
                              sizeof(BSV_INPUT_REPORT));
                WdfRequestSetInformation(Request, sizeof(BSV_INPUT_REPORT));
            }
        }
        break;
    }

    case IOCTL_HID_GET_STRING:
        status = BsvGetString(Request);
        break;

    default:
        status = STATUS_NOT_IMPLEMENTED;
        break;
    }

    WdfRequestComplete(Request, status);
}
