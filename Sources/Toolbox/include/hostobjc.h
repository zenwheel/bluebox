/*
 * Objective-C exceptions the host's frameworks raise for a state they find
 * themselves in (AVAudioPlayerNode's play with its engine stopped under it
 * by a change of output device), which Swift cannot catch: the process
 * would end.  macOS only (hostobjc.m).
 */
#ifndef HOSTOBJC_H
#define HOSTOBJC_H

#ifdef __APPLE__
#include <stdbool.h>
#include <stddef.h>

/* Runs block; false if it raised an exception, with its reason in reason
 * (when given) and the exception swallowed. */
bool host_objc_try(void (^__attribute__((noescape)) block)(void), char *reason, size_t size);
#endif

#endif
