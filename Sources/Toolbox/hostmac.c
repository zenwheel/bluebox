/*
 * A physical interface's Ethernet address from IOKit (its controller's
 * IOMACAddress), for EtherTalk: getifaddrs gives programs without an
 * entitlement a stand-in.  On its own because CoreFoundation's headers and
 * the Toolbox's both define noErr.
 */
#include <stdbool.h>
#include <stdint.h>
#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#endif

bool host_interface_mac(const char *ifname, uint8_t out[6]);

bool host_interface_mac(const char *ifname, uint8_t out[6])
{
#ifdef __APPLE__
    CFMutableDictionaryRef match = IOBSDNameMatching(kIOMainPortDefault, 0, ifname);
    io_service_t intf = match ? IOServiceGetMatchingService(kIOMainPortDefault, match) : 0;
    if (!intf)
        return false;
    io_registry_entry_t controller = 0;
    bool found = false;
    if (IORegistryEntryGetParentEntry(intf, kIOServicePlane, &controller) == KERN_SUCCESS) {
        CFTypeRef d = IORegistryEntryCreateCFProperty(controller, CFSTR("IOMACAddress"), kCFAllocatorDefault, 0);
        if (d && CFGetTypeID(d) == CFDataGetTypeID() && CFDataGetLength(d) == 6) {
            CFDataGetBytes(d, CFRangeMake(0, 6), out);
            found = true;
        }
        if (d)
            CFRelease(d);
        IOObjectRelease(controller);
    }
    IOObjectRelease(intf);
    return found;
#else
    (void)ifname;
    (void)out;
    return false;
#endif
}
