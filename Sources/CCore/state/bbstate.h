/*
 * The state sections: included first in every C and Objective-C file of
 * the C targets (Package.swift passes it with -include), so every mutable
 * static and global they define lands in a section of its own, which a
 * snapshot saves whole (hibernate.c):
 *
 *   __DATA,__bbdata  initialised state         (saved)
 *   __DATA,__bbbss   zero-initialised state    (saved)
 *   __DATA,__bbhost  the host's own state      (not saved)
 *
 * The host's state is what a fresh process makes again for itself and a
 * snapshot cannot carry: thread objects, locks, a library's handles, file
 * descriptors, callback contexts, Metal and OpenGL objects, the JIT's
 * translations.  Such a variable is marked BB_HOST.  An Objective-C file
 * holds only the host's state (Metal, VideoToolbox), so all of its
 * variables go to the host's sections.
 *
 * Constant data is left where the compiler puts it (__TEXT, __DATA_CONST):
 * it is part of the binary.  It lives outside CCore's include directory so
 * that a module import can never stand in for it (a pragma does not cross
 * a module boundary).
 */
#ifndef CCORE_BBSTATE_H
#define CCORE_BBSTATE_H

#if defined(__APPLE__)
#if defined(__OBJC__)
#pragma clang section bss = "__DATA,__bbhostz,zerofill" data = "__DATA,__bbhost"
#else
#pragma clang section bss = "__DATA,__bbbss,zerofill" data = "__DATA,__bbdata"
#endif
#define BB_HOST __attribute__((section("__DATA,__bbhost")))
#else
/* ELF: named sections the default linker script leaves alone (not .bss.*
 * or .data.*, which it would merge into .bss and .data). */
#if defined(__OBJC__)
#pragma clang section bss = "bbhostz" data = "bbhost"
#else
#pragma clang section bss = "bbbss" data = "bbdata"
#endif
#define BB_HOST __attribute__((section("bbhost")))
#endif

#include "../include/bbmem.h"

/* A saved value that is the host's own, not the program's: a reading of
 * the host's clock, an ID made from its process.  It is kept, and goes on
 * within the process; a resume moves it on or makes it again, so two
 * processes' snapshots differ there by nature (the state map's kind 'X'
 * or 'x', which hibernate-diff leaves out with the host's pointers).  A
 * value a fresh process can simply make again belongs in BB_HOST instead. */
typedef uint64_t bb_host_value64;
typedef uint32_t bb_host_value32;

#endif
