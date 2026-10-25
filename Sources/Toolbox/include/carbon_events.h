/*
 * CarbonLib's Carbon Event Manager (carbon_events.c) and Multiprocessing
 * Services (multiprocessing.c), and what the rest of the Toolbox calls in
 * them: sending a Carbon event from a control, a window or a menu, and the
 * event loop's turn for timers, posted events and tasks.
 */
#ifndef CARBON_EVENTS_H
#define CARBON_EVENTS_H

#include <stdbool.h>
#include <stdint.h>

#include "mem.h"

/* Event classes, and the result a handler gives an event it leaves. */
enum {
    kEventClassMouse = 0x6D6F7573,       /* 'mous' */
    kEventClassKeyboard = 0x6B657962,    /* 'keyb' */
    kEventClassTextInput = 0x74657874,   /* 'text' */
    kEventClassApplication = 0x6170706C, /* 'appl' */
    kEventClassAppleEvent = 0x65707063,  /* 'eppc' */
    kEventClassMenu = 0x6D656E75,        /* 'menu' */
    kEventClassWindow = 0x77696E64,      /* 'wind' */
    kEventClassControl = 0x636E746C,     /* 'cntl' */
    kEventClassCommand = 0x636D6473,     /* 'cmds' */
    eventNotHandledErr = -9874,
};

/* Opaque references a program holds (EventRef, EventTargetRef, MPQueueID
 * and the rest): a distinct word of guest memory each, mapped to the
 * object here.  ref_get gives the object when the reference is of `kind`. */
gaddr ref_new(int kind, void *obj);
void *ref_get(gaddr ref, int kind);
int ref_kind(gaddr ref);
void ref_free(gaddr ref);

/* ---- Carbon events, for the other managers ---- */

/* A new event (retain count 1), its parameters, and sending it: the
 * target's handlers in turn, then up to its parent target (a control's
 * window, a window's application).  The result is the handler's, or
 * eventNotHandledErr when none took it. */
gaddr ce_event_new(uint32_t cls, uint32_t kind);
void ce_event_set(gaddr event, uint32_t name, uint32_t type, uint32_t size, const void *data);
void ce_event_set32(gaddr event, uint32_t name, uint32_t type, uint32_t value);
/* A parameter's bytes (up to size), or false when the event lacks it. */
bool ce_event_get(gaddr event, uint32_t name, void *out, uint32_t size);
void ce_event_release(gaddr event);
int32_t ce_send(gaddr event, gaddr target);

/* The targets: the application's, a window's, a control's and a menu's
 * (made when first asked for), and the user focus (the focus window's, or
 * the application's). */
gaddr ce_application_target(void);
gaddr ce_window_target(gaddr window);
gaddr ce_control_target(gaddr control);
gaddr ce_menu_target(gaddr menu);
gaddr ce_focus_target(void);
/* Whether any handler is installed at all (sending can be skipped). */
bool ce_active(void);

/* Ready-made sends: an event of class cls with the object as its direct
 * object (kEventParamDirectObject, of typeWindowRef, typeControlRef or
 * typeMenuRef), to the object's own target. */
int32_t carbon_send_window_event(gaddr window, uint32_t kind);
int32_t carbon_send_control_event(gaddr control, uint32_t kind);
int32_t carbon_send_menu_event(gaddr menu, uint32_t kind);
/* kEventCommandProcess with an HICommand: from a menu item (menu, item),
 * or from a control (menu 0); to the user focus, which passes it on to
 * the application. */
int32_t carbon_send_command(uint32_t command_id, gaddr menu, uint16_t item, uint32_t modifiers);
/* A window made with kWindowStandardHandlerAttribute: the standard
 * handler on its target (InstallStandardEventHandler). */
void carbon_install_standard_handler(gaddr window);
/* An object going away: its target and handlers go with it. */
void carbon_window_disposed(gaddr window);
void carbon_control_disposed(gaddr control);
void carbon_menu_disposed(gaddr menu);

/* WaitNextEvent's turn (events.c): timers due fire, posted Carbon events
 * are dispatched, and Multiprocessing tasks run.  Returns the sleep
 * (ticks) cut short to the next timer. */
int32_t carbon_event_loop_turn(int32_t sleep);
void carbon_events_reset(void);

/* ---- Multiprocessing Services ---- */

/* Each task ready to run gets a turn until it yields or waits (from a
 * thread that isn't a task); due MP timers fire.  Returns how many ran. */
int mp_run_tasks(void);
bool mp_have_tasks(void);
void mp_reset(void);

/* The Thread Manager's side (threadmgr.c): told when thread id ends, and
 * the thread to hand back to then; the running thread's ID. */
void thread_set_exit(uint32_t id, void (*hook)(uint32_t id, uint32_t result), uint32_t exit_to);
uint32_t thread_current_id(void);

#endif
