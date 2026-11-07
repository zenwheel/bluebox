/*
 * hostobjc.h: catching the Objective-C exceptions Swift cannot.
 */
#ifdef __APPLE__
#import <Foundation/Foundation.h>

#include <string.h>

#include "hostobjc.h"

bool host_objc_try(void (^__attribute__((noescape)) block)(void), char *reason, size_t size)
{
    @try {
        block();
        return true;
    } @catch (NSException *e) {
        if (reason && size) {
            const char *s = e.reason.UTF8String ?: e.name.UTF8String ?: "";
            strlcpy(reason, s, size);
        }
        return false;
    }
}
#endif
