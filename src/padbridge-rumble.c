// padbridge-rumble: pulse the 8BitDo's motors at given levels, to find the
// weakest level you can feel (padbridge.conf's rumble_floor).
//
//   padbridge-rumble [-m both|left|right] [-t ms] LEVEL...
//
// Each level (0-255) plays for -t ms (default 700), then 700 ms of silence.
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDManager.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static IOReturn motors(IOHIDDeviceRef dev, int left, int right) {
    uint8_t r[64] = {0x81, 0x11, 0x04, 0x08, 0, 0, (uint8_t)left, (uint8_t)right};
    if (left || right) r[4] = 200;
    return IOHIDDeviceSetReport(dev, kIOHIDReportTypeOutput, 0x81, r, sizeof r);
}

static void hold(IOHIDDeviceRef dev, int left, int right, int ms) {
    // The receiver stops the motors after the 200 ms in the report, so refresh.
    for (int t = 0; t < ms; t += 50) {
        motors(dev, left, right);
        usleep(50000);
    }
}

int main(int argc, char **argv) {
    const char *which = "both";
    int on_ms = 700, opt;
    while ((opt = getopt(argc, argv, "m:t:")) != -1) {
        if (opt == 'm') which = optarg;
        else if (opt == 't') on_ms = atoi(optarg);
        else return 2;
    }
    if (optind >= argc) {
        fprintf(stderr, "usage: padbridge-rumble [-m both|left|right] [-t ms] LEVEL...\n");
        return 2;
    }
    IOHIDManagerRef mgr = IOHIDManagerCreate(kCFAllocatorDefault, 0);
    // The gamepad interface: asleep, the receiver shows a vendor "IDLE" one instead.
    int vid = 0x2dc8, pid = 0x3016, page = 1, usage = 5;
    CFNumberRef v = CFNumberCreate(NULL, kCFNumberIntType, &vid), p = CFNumberCreate(NULL, kCFNumberIntType, &pid);
    CFNumberRef pg = CFNumberCreate(NULL, kCFNumberIntType, &page), u = CFNumberCreate(NULL, kCFNumberIntType, &usage);
    const void *keys[] = {CFSTR(kIOHIDVendorIDKey), CFSTR(kIOHIDProductIDKey), CFSTR(kIOHIDPrimaryUsagePageKey),
                          CFSTR(kIOHIDPrimaryUsageKey)},
               *vals[] = {v, p, pg, u};
    CFDictionaryRef match = CFDictionaryCreate(NULL, keys, vals, 4, &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    IOHIDManagerSetDeviceMatching(mgr, match);
    CFSetRef set = IOHIDManagerCopyDevices(mgr);
    if (!set || !CFSetGetCount(set)) {
        fprintf(stderr, "8BitDo Ultimate C 2.4G (2dc8:3016) not found. Is the dongle in, pad in D-input "
                        "and awake?\n");
        return 1;
    }
    IOHIDDeviceRef devs[8];
    CFSetGetValues(set, (const void **)devs);
    IOHIDDeviceRef dev = devs[0];
    IOReturn r = IOHIDDeviceOpen(dev, 0);
    if (r != kIOReturnSuccess) {
        fprintf(stderr, "could not open the pad: %08x (Input Monitoring permission?)\n", r);
        return 1;
    }
    for (int i = optind; i < argc; i++) {
        int level = atoi(argv[i]);
        int l = strcmp(which, "right") ? level : 0, rt = strcmp(which, "left") ? level : 0;
        printf("level %3d (%s)\n", level, which);
        fflush(stdout);
        hold(dev, l, rt, on_ms);
        motors(dev, 0, 0);
        usleep(700000);
    }
    motors(dev, 0, 0);
    IOHIDDeviceClose(dev, 0);
    return 0;
}
