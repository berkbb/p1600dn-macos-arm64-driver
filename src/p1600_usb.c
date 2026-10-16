#include "p1600_usb.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>

#define READ_CHUNK 16384U

struct P1600UsbChannel {
    IOUSBInterfaceInterface300 **interface;
    UInt8 pipe_in;
    UInt8 pipe_out;
    UInt16 packet_size;
    unsigned char *pending;      /* bytes of the last packet not yet handed out */
    size_t pending_length;
    size_t pending_offset;
};

static void fail(char *error, size_t error_size, const char *format, ...) {
    if (error && error_size > 0) {
        va_list arguments;
        va_start(arguments, format);
        vsnprintf(error, error_size, format, arguments);
        va_end(arguments);
    }
}

static bool string_property(io_registry_entry_t entry, const char *name, char *out, size_t size) {
    out[0] = '\0';
    CFStringRef key = CFStringCreateWithCString(kCFAllocatorDefault, name, kCFStringEncodingUTF8);
    if (!key) return false;
    CFTypeRef value = IORegistryEntryCreateCFProperty(entry, key, kCFAllocatorDefault, 0);
    CFRelease(key);
    bool ok = value && CFGetTypeID(value) == CFStringGetTypeID() &&
              CFStringGetCString((CFStringRef)value, out, (CFIndex)size, kCFStringEncodingUTF8);
    if (value) CFRelease(value);
    return ok;
}

static bool number_property(io_registry_entry_t entry, const char *name, long *out) {
    CFStringRef key = CFStringCreateWithCString(kCFAllocatorDefault, name, kCFStringEncodingUTF8);
    if (!key) return false;
    CFTypeRef value = IORegistryEntryCreateCFProperty(entry, key, kCFAllocatorDefault, 0);
    CFRelease(key);
    bool ok = value && CFGetTypeID(value) == CFNumberGetTypeID() &&
              CFNumberGetValue((CFNumberRef)value, kCFNumberLongType, out);
    if (value) CFRelease(value);
    return ok;
}

/* First USB device the target names; IO_OBJECT_NULL when there is none. */
static io_service_t find_device(const P1600UsbTarget *target) {
    static const char *const classes[] = {"IOUSBHostDevice", "IOUSBDevice"};
    for (size_t c = 0; c < sizeof(classes) / sizeof(classes[0]); c++) {
        io_iterator_t devices = IO_OBJECT_NULL;
        /* MACH_PORT_NULL is the default main port on every macOS release
         * (kIOMainPortDefault needs macOS 12). */
        if (IOServiceGetMatchingServices(MACH_PORT_NULL, IOServiceMatching(classes[c]), &devices) != KERN_SUCCESS) {
            continue;
        }
        io_service_t device;
        while ((device = IOIteratorNext(devices)) != IO_OBJECT_NULL) {
            char product[256], serial[256];
            long vendor = 0;
            string_property(device, "USB Product Name", product, sizeof(product));
            string_property(device, "USB Serial Number", serial, sizeof(serial));
            number_property(device, "idVendor", &vendor);
            if (p1600_usb_device_matches(target, product, serial, (uint32_t)vendor)) {
                IOObjectRelease(devices);
                return device;
            }
            IOObjectRelease(device);
        }
        IOObjectRelease(devices);
    }
    return IO_OBJECT_NULL;
}

static bool matches(long value, int wanted) {
    return wanted == P1600_USB_ANY || value == wanted;
}

/* The device's first interface with the given class triple. */
static io_service_t find_interface(io_service_t device, int interface_class, int subclass, int protocol) {
    io_iterator_t children = IO_OBJECT_NULL;
    if (IORegistryEntryCreateIterator(device, kIOServicePlane, kIORegistryIterateRecursively, &children) != KERN_SUCCESS) {
        return IO_OBJECT_NULL;
    }
    io_service_t entry;
    while ((entry = IOIteratorNext(children)) != IO_OBJECT_NULL) {
        long found_class = -1, found_subclass = -1, found_protocol = -1;
        if ((IOObjectConformsTo(entry, "IOUSBHostInterface") || IOObjectConformsTo(entry, "IOUSBInterface")) &&
            number_property(entry, "bInterfaceClass", &found_class) &&
            number_property(entry, "bInterfaceSubClass", &found_subclass) &&
            number_property(entry, "bInterfaceProtocol", &found_protocol) &&
            matches(found_class, interface_class) && matches(found_subclass, subclass) &&
            matches(found_protocol, protocol)) {
            IOObjectRelease(children);
            return entry;
        }
        IOObjectRelease(entry);
    }
    IOObjectRelease(children);
    return IO_OBJECT_NULL;
}

P1600UsbChannel *p1600_usb_open(const P1600UsbTarget *target, int interface_class, int subclass, int protocol,
                          char *error, size_t error_size) {
    if (!target) {
        fail(error, error_size, "No USB device");
        return NULL;
    }
    io_service_t device = find_device(target);
    if (device == IO_OBJECT_NULL) {
        fail(error, error_size, "USB printer not found");
        return NULL;
    }
    io_service_t service = find_interface(device, interface_class, subclass, protocol);
    IOObjectRelease(device);
    if (service == IO_OBJECT_NULL) {
        fail(error, error_size, "USB printer has no %02X/%02X/%02X interface", interface_class & 0xff,
             subclass & 0xff, protocol & 0xff);
        return NULL;
    }

    IOCFPlugInInterface **plugin = NULL;
    SInt32 score = 0;
    kern_return_t created = IOCreatePlugInInterfaceForService(service, kIOUSBInterfaceUserClientTypeID,
                                                              kIOCFPlugInInterfaceID, &plugin, &score);
    IOObjectRelease(service);
    if (created != KERN_SUCCESS || !plugin) {
        fail(error, error_size, "Unable to access the USB interface (0x%08x)", created);
        return NULL;
    }
    IOUSBInterfaceInterface300 **interface = NULL;
    HRESULT queried = (*plugin)->QueryInterface(plugin, CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID300),
                                                (LPVOID *)&interface);
    IODestroyPlugInInterface(plugin);
    if (queried != S_OK || !interface) {
        fail(error, error_size, "Unable to query the USB interface");
        return NULL;
    }
    IOReturn opened = (*interface)->USBInterfaceOpen(interface);
    if (opened != kIOReturnSuccess) {
        (*interface)->Release(interface);
        if (opened == kIOReturnExclusiveAccess) {
            fail(error, error_size, "The USB printer is busy (another program is using it)");
        } else {
            fail(error, error_size, "Unable to open the USB interface (0x%08x)", opened);
        }
        return NULL;
    }

    P1600UsbChannel *channel = calloc(1, sizeof(*channel));
    if (!channel) {
        (*interface)->USBInterfaceClose(interface);
        (*interface)->Release(interface);
        fail(error, error_size, "Out of memory");
        return NULL;
    }
    channel->interface = interface;
    UInt8 endpoints = 0;
    (*interface)->GetNumEndpoints(interface, &endpoints);
    for (UInt8 pipe = 1; pipe <= endpoints; pipe++) {
        UInt8 direction = 0, number = 0, transfer = 0, interval = 0;
        UInt16 size = 0;
        if ((*interface)->GetPipeProperties(interface, pipe, &direction, &number, &transfer, &size, &interval) !=
                kIOReturnSuccess ||
            transfer != kUSBBulk) continue;
        if (direction == kUSBOut && channel->pipe_out == 0) channel->pipe_out = pipe;
        if (direction == kUSBIn && channel->pipe_in == 0) {
            channel->pipe_in = pipe;
            channel->packet_size = size;
        }
    }
    if (channel->pipe_out == 0 || channel->pipe_in == 0 || channel->packet_size == 0) {
        p1600_usb_close(channel);
        fail(error, error_size, "The USB interface has no bulk pipes");
        return NULL;
    }
    return channel;
}

int p1600_usb_write(P1600UsbChannel *channel, const void *data, size_t length, unsigned timeout_ms) {
    if (!channel || (!data && length != 0)) return -1;
    const unsigned char *bytes = data;
    while (length > 0) {
        UInt32 amount = length > 65536U ? 65536U : (UInt32)length;
        if ((*channel->interface)->WritePipeTO(channel->interface, channel->pipe_out, (void *)bytes, amount,
                                                timeout_ms, timeout_ms) != kIOReturnSuccess) return -1;
        bytes += amount;
        length -= amount;
    }
    return 0;
}

ssize_t p1600_usb_read(P1600UsbChannel *channel, void *buffer, size_t length, unsigned timeout_ms) {
    if (!channel || !buffer || length == 0) return -1;
    if (channel->pending_offset < channel->pending_length) {
        size_t amount = channel->pending_length - channel->pending_offset;
        if (amount > length) amount = length;
        memcpy(buffer, channel->pending + channel->pending_offset, amount);
        channel->pending_offset += amount;
        return (ssize_t)amount;
    }
    /* Bulk reads ask for whole packets so a packet never overflows. */
    if (!channel->pending) {
        channel->pending = malloc(READ_CHUNK);
        if (!channel->pending) return -1;
    }
    UInt32 size = READ_CHUNK - READ_CHUNK % channel->packet_size;
    if ((*channel->interface)->ReadPipeTO(channel->interface, channel->pipe_in, channel->pending, &size,
                                           timeout_ms, timeout_ms) != kIOReturnSuccess) {
        (*channel->interface)->ClearPipeStallBothEnds(channel->interface, channel->pipe_in);
        return -1;
    }
    channel->pending_length = size;
    channel->pending_offset = 0;
    size_t amount = size < length ? size : length;
    memcpy(buffer, channel->pending, amount);
    channel->pending_offset = amount;
    return (ssize_t)amount;
}

void p1600_usb_close(P1600UsbChannel *channel) {
    if (!channel) return;
    (*channel->interface)->USBInterfaceClose(channel->interface);
    (*channel->interface)->Release(channel->interface);
    free(channel->pending);
    free(channel);
}
