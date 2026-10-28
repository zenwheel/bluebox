/*
 * DebugLib (Debugging.h): DebugAssert, the registry of debugging components
 * and their options, and TaskLevel.  _DebugLibDispatch ($AA7E) on the 68k;
 * the same routines are DebugLib's and CarbonLib's exports.
 *
 * A component registers a callback that reads and writes its options; the
 * registry is kept here and the callback asked whenever an option is read
 * or set.  DebugAssert builds the message DebugStr would be given and hands
 * it, with the raw values, to the installed output handler; with none it
 * goes where DebugStr goes on a Mac with no debugger, which is nowhere but
 * the log.
 */
#include <stdio.h>
#include <string.h>

#include "log.h"
#include "os9rest.h"
#include "os9svc.h"
#include "tb_internal.h"

enum {
    debuggingExecutionContextErr = -13880,
    debuggingDuplicateSignatureErr = -13881,
    debuggingDuplicateOptionErr = -13882,
    debuggingInvalidSignatureErr = -13883,
    debuggingInvalidOptionErr = -13884,
    debuggingInvalidNameErr = -13885,
    debuggingNoCallbackErr = -13886,
    debuggingNoMatchErr = -13887,
};
enum { kComponentDebugOption = 0, kGetDebugOption = 1, kSetDebugOption = 2 };
enum { MAX_COMPONENTS = 32, MAX_OPTIONS = 32 };
enum { kCallbackProcInfo = 0x00000FC0, kHandlerProcInfo = 0x00FFFFC0 };

typedef struct {
    int32_t selector;
    uint8_t name[256];
    bool setting; /* when the component has no callback to ask */
} dbg_option;

typedef struct {
    bool used;
    uint32_t signature;
    uint8_t name[256];
    gaddr callback;
    dbg_option options[MAX_OPTIONS];
    int noptions;
} dbg_component;

static dbg_component components[MAX_COMPONENTS];
static gaddr output_handler;

void os9rest_debug_reset(void)
{
    memset(components, 0, sizeof components);
    output_handler = 0;
}

static void pstr_get(gaddr p, uint8_t *out)
{
    uint32_t n = rd8(p);
    for (uint32_t i = 0; i <= n; i++)
        out[i] = rd8(p + i);
}

static void pstr_put(gaddr p, const uint8_t *s)
{
    for (uint32_t i = 0; i <= s[0]; i++)
        wr8(p + i, s[i]);
}

static dbg_component *component_of(uint32_t sig)
{
    for (int i = 0; i < MAX_COMPONENTS; i++)
        if (components[i].used && components[i].signature == sig)
            return &components[i];
    return NULL;
}

uint32_t tb_TaskLevel(void) { return os9svc_task_level(); }

int32_t tb_NewDebugComponent(int32_t sig, gaddr name, gaddr callback)
{
    if (os9svc_task_level())
        return debuggingExecutionContextErr;
    if (!name)
        return debuggingInvalidNameErr;
    if (component_of(sig))
        return debuggingDuplicateSignatureErr;
    for (int i = 0; i < MAX_COMPONENTS; i++)
        if (!components[i].used) {
            memset(&components[i], 0, sizeof components[i]);
            components[i].used = true;
            components[i].signature = sig;
            components[i].callback = callback;
            pstr_get(name, components[i].name);
            return noErr;
        }
    return memFullErr;
}

int32_t tb_NewDebugOption(int32_t sig, int32_t selector, gaddr name)
{
    if (os9svc_task_level())
        return debuggingExecutionContextErr;
    dbg_component *c = component_of(sig);
    if (!c)
        return debuggingInvalidSignatureErr;
    if (!name)
        return debuggingInvalidNameErr;
    if (!c->callback)
        return debuggingNoCallbackErr;
    if (selector == kComponentDebugOption)
        return debuggingDuplicateOptionErr;
    for (int i = 0; i < c->noptions; i++)
        if (c->options[i].selector == selector)
            return debuggingDuplicateOptionErr;
    if (c->noptions == MAX_OPTIONS)
        return memFullErr;
    dbg_option *o = &c->options[c->noptions++];
    memset(o, 0, sizeof *o);
    o->selector = selector;
    pstr_get(name, o->name);
    return noErr;
}

int32_t tb_DisposeDebugComponent(int32_t sig)
{
    if (os9svc_task_level())
        return debuggingExecutionContextErr;
    dbg_component *c = component_of(sig);
    if (!c)
        return debuggingInvalidSignatureErr;
    c->used = false;
    return noErr;
}

int32_t tb_GetDebugComponentInfo(uint32_t index, gaddr sig_out, gaddr name_out)
{
    uint32_t n = 0;
    for (int i = 0; i < MAX_COMPONENTS; i++)
        if (components[i].used && ++n == index) {
            if (sig_out)
                wr32(sig_out, components[i].signature);
            if (name_out)
                pstr_put(name_out, components[i].name);
            return noErr;
        }
    return debuggingNoMatchErr;
}

/* Ask a component's callback about an option: its setting in or out. */
static bool option_call(const dbg_component *c, int32_t selector, uint32_t command, bool *setting)
{
    gaddr cell = tb_NewPtr(2, true, true);
    if (!cell)
        return *setting;
    wr8(cell, *setting);
    uint32_t args[3] = { (uint32_t)selector, command, cell };
    if (mm_is_descriptor(c->callback))
        mm_call(c->callback, kCallbackProcInfo, args, 3);
    else
        ppc_call(c->callback, args, 3);
    *setting = rd8(cell) != 0;
    tb_DisposePtr(cell);
    return *setting;
}

int32_t tb_GetDebugOptionInfo(uint32_t index, int32_t sig, gaddr selector_out, gaddr name_out, gaddr setting_out)
{
    dbg_component *c = component_of(sig);
    if (!c)
        return debuggingInvalidSignatureErr;
    if (index > (uint32_t)c->noptions)
        return debuggingNoMatchErr;
    int32_t selector = index ? c->options[index - 1].selector : kComponentDebugOption;
    bool setting = index ? c->options[index - 1].setting : false;
    if (c->callback)
        option_call(c, selector, kGetDebugOption, &setting);
    if (selector_out)
        wr32(selector_out, (uint32_t)selector);
    if (name_out) {
        static const uint8_t component_option[] = "\x1F" "Enable debugger breaks for component";
        pstr_put(name_out, index ? c->options[index - 1].name : component_option);
    }
    if (setting_out)
        wr8(setting_out, setting);
    return noErr;
}

int32_t tb_SetDebugOptionValue(int32_t sig, int32_t selector, int8_t value)
{
    dbg_component *c = component_of(sig);
    if (!c)
        return debuggingInvalidSignatureErr;
    dbg_option *o = NULL;
    for (int i = 0; i < c->noptions; i++)
        if (c->options[i].selector == selector)
            o = &c->options[i];
    if (!o && selector != kComponentDebugOption)
        return debuggingInvalidOptionErr;
    bool setting = value != 0;
    if (o)
        o->setting = setting;
    if (c->callback)
        option_call(c, selector, kSetDebugOption, &setting);
    return noErr;
}

void tb_InstallDebugAssertOutputHandler(gaddr handler) { output_handler = handler; }

/* DebugAssert(signature, options, assertion, exception, error, file, line,
 * value): the message is the assertion, and the exception, error, place and
 * value after it as far as they were given. */
void tb_DebugAssert(int32_t sig, uint32_t options, gaddr assertion, gaddr exception, gaddr error, gaddr file,
                    int32_t line, gaddr value)
{
    char text[200], msg[256];
    size_t n = 0;
    gaddr parts[3] = { assertion, exception, error };
    const char *labels[3] = { "", " (", " [" };
    const char *closers[3] = { "", ")", "]" };
    msg[0] = 0;
    for (int i = 0; i < 3; i++) {
        if (!parts[i])
            continue;
        size_t k = 0;
        while (k + 1 < sizeof text && rd8(parts[i] + k)) {
            text[k] = (char)rd8(parts[i] + k);
            k++;
        }
        text[k] = 0;
        n += (size_t)snprintf(msg + n, sizeof msg - n, "%s%s%s", labels[i], text, closers[i]);
        if (n >= sizeof msg)
            n = sizeof msg - 1;
    }
    if (file) {
        size_t k = 0;
        while (k + 1 < sizeof text && rd8(file + k)) {
            text[k] = (char)rd8(file + k);
            k++;
        }
        text[k] = 0;
        n += (size_t)snprintf(msg + n, sizeof msg - n, " in %s at line %d", text, (int)line);
        if (n >= sizeof msg)
            n = sizeof msg - 1;
    }
    if (value)
        n += (size_t)snprintf(msg + n, sizeof msg - n, " value 0x%X", (unsigned)value);
    if (n >= 255)
        n = 255;
    if (output_handler) {
        gaddr p = tb_NewPtr(256, true, true);
        if (p) {
            wr8(p, (uint8_t)n);
            for (size_t i = 0; i < n; i++)
                wr8(p + 1 + (uint32_t)i, (uint8_t)msg[i]);
            uint32_t args[9] = { sig, options, assertion, exception, error, file, (uint32_t)line, value, p };
            if (mm_is_descriptor(output_handler))
                mm_call(output_handler, kHandlerProcInfo, args, 9);
            else
                ppc_call(output_handler, args, 9);
            tb_DisposePtr(p);
        }
        return;
    }
    LOGF(LOG_TRAPS | LOG_UNIMPL, "DebugAssert: %c%c%c%c %s\n", (char)(sig >> 24), (char)(sig >> 16), (char)(sig >> 8),
         (char)sig, msg);
}
