/*
 * The Communications Toolbox, shared by its managers: the routine table
 * (ctb.c dispatches it for 68k code and exports it to PowerPC code), the
 * tools' configurations and the calls every manager has.
 *
 * Each routine has a selector (high byte the manager: 1 the Connection
 * Manager, 2 the File Transfer Manager, 3 the Terminal Manager, 4 the
 * utilities, 5 the Communications Resource Manager), its arguments in
 * declaration order ('w' a short, 'l' a long or pointer, 'b' a Boolean)
 * and its result ('v' none, 'w', 'l', 'b', or 'p' a Point).
 */
#ifndef CTB_H
#define CTB_H

#include <stdbool.h>
#include <stdint.h>

#include "tb_internal.h"

/* X(name, selector, arguments, result, function, exported to PowerPC) */
#define CTB_ROUTINES(X) \
    X(InitCM, 0x0101, "", 'w', cm_init, 1) \
    X(CMSetRefCon, 0x0102, "ll", 'v', ctb_set_refcon, 1) \
    X(CMGetRefCon, 0x0103, "l", 'l', ctb_get_refcon, 1) \
    X(CMSetUserData, 0x0104, "ll", 'v', ctb_set_userdata, 1) \
    X(CMGetUserData, 0x0105, "l", 'l', ctb_get_userdata, 1) \
    X(CMGetToolName, 0x0106, "wl", 'v', ctb_get_tool_name, 1) \
    X(CMGetProcID, 0x0107, "l", 'w', ctb_get_proc_id, 1) \
    X(CMNew, 0x0108, "wllll", 'l', cm_new, 1) \
    X(CMDispose, 0x0109, "l", 'v', cm_dispose, 1) \
    X(CMIdle, 0x010A, "l", 'v', cm_idle, 1) \
    X(CMOpen, 0x010B, "lbll", 'w', cm_open, 1) \
    X(CMListen, 0x010C, "lbll", 'w', cm_listen, 1) \
    X(CMAccept, 0x010D, "lb", 'w', cm_accept, 1) \
    X(CMClose, 0x010E, "lbllb", 'w', cm_close, 1) \
    X(CMAbort, 0x010F, "l", 'w', cm_abort, 1) \
    X(CMStatus, 0x0110, "lll", 'w', cm_status, 1) \
    X(CMRead, 0x0111, "lllwblll", 'w', cm_read, 1) \
    X(CMWrite, 0x0112, "lllwbllw", 'w', cm_write, 1) \
    X(CMActivate, 0x0113, "lb", 'v', ctb_nothing, 1) \
    X(CMResume, 0x0114, "lb", 'v', ctb_nothing, 1) \
    X(CMMenu, 0x0115, "lww", 'b', ctb_nothing, 1) \
    X(CMReset, 0x0116, "l", 'v', cm_reset, 1) \
    X(CMValidate, 0x0117, "l", 'b', ctb_validate, 1) \
    X(CMDefault, 0x0118, "lwb", 'v', ctb_default, 1) \
    X(CMSetupSetup, 0x0119, "wlwll", 'v', ctb_nothing, 1) \
    X(CMSetupItem, 0x011A, "wlwlll", 'v', ctb_nothing, 1) \
    X(CMSetupCleanup, 0x011B, "wlwll", 'v', ctb_nothing, 1) \
    X(CMGetConfig, 0x011C, "l", 'l', ctb_get_config, 1) \
    X(CMSetConfig, 0x011D, "ll", 'w', ctb_set_config, 1) \
    X(CMIntlToEnglish, 0x011E, "lllw", 'w', ctb_intl, 1) \
    X(CMEnglishToIntl, 0x011F, "lllw", 'w', ctb_intl, 1) \
    X(CMGetVersion, 0x0120, "l", 'l', ctb_get_version, 1) \
    X(CMGetCMVersion, 0x0121, "", 'w', ctb_mgr_version, 1) \
    X(CMSetupFilter, 0x0122, "wlwllll", 'b', ctb_nothing, 1) \
    X(CMSetupPreflight, 0x0123, "wl", 'l', ctb_setup_preflight, 1) \
    X(CMChoose, 0x0124, "lll", 'w', ctb_choose, 1) \
    X(CMBreak, 0x0125, "llbl", 'v', cm_break, 1) \
    X(CMAddSearch, 0x0126, "llwl", 'l', cm_add_search, 1) \
    X(CMRemoveSearch, 0x0127, "ll", 'v', cm_remove_search, 1) \
    X(CMClearSearch, 0x0128, "l", 'v', cm_clear_search, 1) \
    X(CMIOKill, 0x0129, "lw", 'w', cm_iokill, 1) \
    X(CMEvent, 0x012A, "ll", 'v', ctb_nothing, 1) \
    X(CMSetupPostflight, 0x012B, "w", 'v', ctb_nothing, 1) \
    X(CMGetConnEnvirons, 0x012C, "ll", 'w', cm_get_environs, 1) \
    X(CMPChoose, 0x012D, "lll", 'w', ctb_choose, 1) \
    X(CMSetupXCleanup, 0x012E, "wlwlbl", 'v', ctb_nothing, 1) \
    X(CMGetErrorString, 0x012F, "lwl", 'v', ctb_error_string, 1) \
    X(CMNewIOPB, 0x0130, "ll", 'w', cm_new_iopb, 1) \
    X(CMDisposeIOPB, 0x0131, "ll", 'w', cm_dispose_iopb, 1) \
    X(CMPBRead, 0x0132, "llb", 'w', cm_pb_read, 1) \
    X(CMPBWrite, 0x0133, "llb", 'w', cm_pb_write, 1) \
    X(CMPBIOKill, 0x0134, "ll", 'w', cm_pb_iokill, 1) \
    X(InitFT, 0x0201, "", 'w', ft_init, 1) \
    X(FTSetRefCon, 0x0202, "ll", 'v', ctb_set_refcon, 1) \
    X(FTGetRefCon, 0x0203, "l", 'l', ctb_get_refcon, 1) \
    X(FTSetUserData, 0x0204, "ll", 'v', ctb_set_userdata, 1) \
    X(FTGetUserData, 0x0205, "l", 'l', ctb_get_userdata, 1) \
    X(FTGetToolName, 0x0206, "wl", 'v', ctb_get_tool_name, 1) \
    X(FTGetProcID, 0x0207, "l", 'w', ctb_get_proc_id, 1) \
    X(FTNew, 0x0208, "wlllllllll", 'l', ft_new, 1) \
    X(FTDispose, 0x0209, "l", 'v', ft_dispose, 1) \
    X(FTExec, 0x020A, "l", 'v', ft_exec, 1) \
    X(FTStart, 0x020B, "lwl", 'w', ft_start, 1) \
    X(FTCleanup, 0x020C, "lb", 'w', ft_abort, 0) \
    X(FTAbort, 0x020D, "l", 'w', ft_abort, 1) \
    X(FTResume, 0x020E, "lb", 'v', ctb_nothing, 1) \
    X(FTValidate, 0x020F, "l", 'b', ctb_validate, 1) \
    X(FTDefault, 0x0210, "lwb", 'v', ctb_default, 1) \
    X(FTSetupPreflight, 0x0211, "wl", 'l', ctb_setup_preflight, 1) \
    X(FTSetupSetup, 0x0212, "wlwll", 'v', ctb_nothing, 1) \
    X(FTSetupFilter, 0x0213, "wlwllll", 'b', ctb_nothing, 1) \
    X(FTSetupItem, 0x0214, "wlwlll", 'v', ctb_nothing, 1) \
    X(FTSetupCleanup, 0x0215, "wlwll", 'v', ctb_nothing, 1) \
    X(FTGetConfig, 0x0216, "l", 'l', ctb_get_config, 1) \
    X(FTSetConfig, 0x0217, "ll", 'w', ctb_set_config, 1) \
    X(FTIntlToEnglish, 0x0218, "lllw", 'w', ctb_intl, 1) \
    X(FTEnglishToIntl, 0x0219, "lllw", 'w', ctb_intl, 1) \
    X(FTGetVersion, 0x021A, "l", 'l', ctb_get_version, 1) \
    X(FTGetFTVersion, 0x021B, "", 'w', ctb_mgr_version, 1) \
    X(FTChoose, 0x021C, "lll", 'w', ctb_choose, 1) \
    X(FTEvent, 0x021D, "ll", 'v', ctb_nothing, 1) \
    X(FTSetupPostflight, 0x021E, "w", 'v', ctb_nothing, 1) \
    X(FTMenu, 0x021F, "lww", 'b', ctb_nothing, 1) \
    X(FTActivate, 0x0220, "lb", 'v', ctb_nothing, 1) \
    X(FTPChoose, 0x0221, "lll", 'w', ctb_choose, 1) \
    X(FTSetupXCleanup, 0x0222, "wlwlbl", 'v', ctb_nothing, 1) \
    X(FTGetErrorString, 0x0223, "lwl", 'v', ctb_error_string, 1) \
    X(FTSend, 0x0224, "lwll", 'w', ft_send, 1) \
    X(FTReceive, 0x0225, "lll", 'w', ft_receive, 1) \
    X(InitTM, 0x0301, "", 'w', tm_init, 1) \
    X(TMNew, 0x0302, "lllwllllllll", 'l', tm_new, 1) \
    X(TMDispose, 0x0303, "l", 'v', tm_dispose, 1) \
    X(TMKey, 0x0304, "ll", 'v', tm_key, 1) \
    X(TMUpdate, 0x0305, "ll", 'v', tm_update, 1) \
    X(TMPaint, 0x0306, "lll", 'v', tm_paint, 1) \
    X(TMActivate, 0x0307, "lb", 'v', tm_activate, 1) \
    X(TMResume, 0x0308, "lb", 'v', tm_activate, 1) \
    X(TMClick, 0x0309, "ll", 'v', tm_click, 1) \
    X(TMStream, 0x030A, "lllw", 'l', tm_stream, 1) \
    X(TMMenu, 0x030B, "lww", 'b', ctb_nothing, 1) \
    X(TMReset, 0x030C, "l", 'v', tm_reset, 1) \
    X(TMClear, 0x030D, "l", 'v', tm_clear, 1) \
    X(TMResize, 0x030E, "ll", 'v', tm_resize, 1) \
    X(TMGetSelect, 0x030F, "lll", 'l', tm_get_select, 1) \
    X(TMGetLine, 0x0310, "lwl", 'v', tm_get_line, 1) \
    X(TMSetSelection, 0x0311, "llw", 'v', tm_set_selection, 1) \
    X(TMScroll, 0x0312, "lww", 'v', tm_scroll, 1) \
    X(TMIdle, 0x0313, "l", 'v', tm_idle, 1) \
    X(TMValidate, 0x0314, "l", 'b', ctb_validate, 1) \
    X(TMDefault, 0x0315, "lwb", 'v', ctb_default, 1) \
    X(TMSetupPreflight, 0x0316, "wl", 'l', ctb_setup_preflight, 1) \
    X(TMSetupSetup, 0x0317, "wlwll", 'v', ctb_nothing, 1) \
    X(TMSetupFilter, 0x0318, "wlwllll", 'b', ctb_nothing, 1) \
    X(TMSetupItem, 0x0319, "wlwlll", 'v', ctb_nothing, 1) \
    X(TMSetupCleanup, 0x031A, "wlwll", 'v', ctb_nothing, 1) \
    X(TMGetConfig, 0x031B, "l", 'l', ctb_get_config, 1) \
    X(TMSetConfig, 0x031C, "ll", 'w', ctb_set_config, 1) \
    X(TMIntlToEnglish, 0x031D, "lllw", 'w', ctb_intl, 1) \
    X(TMEnglishToIntl, 0x031E, "lllw", 'w', ctb_intl, 1) \
    X(TMGetProcID, 0x031F, "l", 'w', ctb_get_proc_id, 1) \
    X(TMGetToolName, 0x0320, "wl", 'v', ctb_get_tool_name, 1) \
    X(TMSetRefCon, 0x0321, "ll", 'v', ctb_set_refcon, 1) \
    X(TMGetRefCon, 0x0322, "l", 'l', ctb_get_refcon, 1) \
    X(TMSetUserData, 0x0323, "ll", 'v', ctb_set_userdata, 1) \
    X(TMGetUserData, 0x0324, "l", 'l', ctb_get_userdata, 1) \
    X(TMGetVersion, 0x0325, "l", 'l', ctb_get_version, 1) \
    X(TMGetTMVersion, 0x0326, "", 'w', ctb_mgr_version, 1) \
    X(TMAddSearch, 0x0327, "lllwl", 'w', tm_add_search, 1) \
    X(TMRemoveSearch, 0x0328, "lw", 'v', tm_remove_search, 1) \
    X(TMClearSearch, 0x0329, "l", 'v', tm_clear_search, 1) \
    X(TMGetCursor, 0x032A, "lw", 'p', tm_get_cursor, 1) \
    X(TMGetTermEnvirons, 0x032B, "ll", 'w', tm_get_environs, 1) \
    X(TMChoose, 0x032C, "lll", 'w', ctb_choose, 1) \
    X(TMEvent, 0x032D, "ll", 'v', ctb_nothing, 1) \
    X(TMDoTermKey, 0x032E, "ll", 'b', tm_do_term_key, 1) \
    X(TMCountTermKeys, 0x032F, "l", 'w', tm_count_term_keys, 1) \
    X(TMGetIndTermKey, 0x0330, "lwl", 'v', tm_get_ind_term_key, 1) \
    X(TMSetupPostflight, 0x0331, "w", 'v', ctb_nothing, 1) \
    X(TMPeekLine, 0x0332, "lwl", 'v', tm_peek_line, 1) \
    X(TMPChoose, 0x0333, "lll", 'w', ctb_choose, 1) \
    X(TMSetupXCleanup, 0x0334, "wlwlbl", 'v', ctb_nothing, 1) \
    X(TMGetErrorString, 0x0335, "lwl", 'v', ctb_error_string, 1) \
    X(InitCTBUtilities, 0x0401, "", 'w', ctb_init, 1) \
    X(AppendDITL, 0x0402, "llw", 'v', ctb_append_ditl, 0) \
    X(CountDITL, 0x0403, "l", 'w', ctb_count_ditl, 0) \
    X(ShortenDITL, 0x0404, "lw", 'v', ctb_shorten_ditl, 0) \
    X(CTBGetCTBVersion, 0x0405, "", 'w', ctb_mgr_version, 1) \
    X(StandardNBP, 0x0406, "llwlllll", 'w', ctb_nbp, 1) \
    X(NuLookup, 0x0406, "llwlllll", 'w', ctb_nbp, 1) \
    X(CustomNBP, 0x0407, "llwlllllwll", 'w', ctb_nbp, 1) \
    X(NuPLookup, 0x0407, "llwlllllwll", 'w', ctb_nbp, 1) \
    X(InitCRM, 0x0501, "", 'w', ctb_init, 1) \
    X(CRMGetHeader, 0x0502, "", 'l', crm_get_header, 1) \
    X(CRMInstall, 0x0503, "l", 'v', crm_install_r, 1) \
    X(CRMRemove, 0x0504, "l", 'w', crm_remove, 1) \
    X(CRMSearch, 0x0505, "l", 'l', crm_search_r, 1) \
    X(CRMGetCRMVersion, 0x0506, "", 'w', ctb_mgr_version, 1) \
    X(CRMGetResource, 0x0507, "lw", 'l', crm_get_resource, 1) \
    X(CRMGet1Resource, 0x0508, "lw", 'l', crm_get_resource, 1) \
    X(CRMGetIndResource, 0x0509, "lw", 'l', crm_get_resource, 1) \
    X(CRMGet1IndResource, 0x050A, "lw", 'l', crm_get_resource, 1) \
    X(CRMGetNamedResource, 0x050B, "ll", 'l', crm_get_resource, 1) \
    X(CRMGet1NamedResource, 0x050C, "ll", 'l', crm_get_resource, 1) \
    X(CRMReleaseResource, 0x050D, "l", 'v', crm_release_resource, 1) \
    X(CRMGetIndex, 0x050E, "l", 'l', ctb_zero, 1) \
    X(CRMLocalToRealID, 0x050F, "lwlw", 'w', crm_local_id, 1) \
    X(CRMRealToLocalID, 0x0510, "lwlw", 'w', crm_local_id, 1) \
    X(CRMGetIndToolName, 0x0511, "lwl", 'w', crm_get_ind_tool_name, 1) \
    X(CRMReserveRF, 0x0513, "w", 'w', ctb_zero, 1) \
    X(CRMReleaseRF, 0x0514, "w", 'w', ctb_zero, 1) \
    X(CRMGetRFCount, 0x0515, "w", 'w', ctb_zero, 0) \
    X(CRMFindCommunications, 0x051A, "ll", 'w', crm_find_communications, 1) \
    X(CRMToolPreflight, 0x051F, "wl", 'v', ctb_nothing, 0) \
    X(CRMToolPostflight, 0x0520, "l", 'v', ctb_nothing, 0) \
    X(CRMIsDriverOpen, 0x0521, "l", 'b', crm_is_driver_open, 1) \
    X(CRMGetToolResource, 0x0523, "wlw", 'l', ctb_zero, 1) \
    X(CRMReleaseToolResource, 0x0524, "wl", 'v', ctb_nothing, 1) \
    X(CRMParseCAPSResource, 0x0525, "lll", 'w', crm_parse_caps, 1) \
    X(CRMGetToolNamedResource, 0x0526, "wll", 'l', ctb_zero, 1)

/* Every routine: its selector and arguments (shorts sign-extended,
 * Booleans 0 or 1); the result as a long. */
typedef uint32_t (*ctb_fn)(uint16_t sel, const uint32_t *a);
#define CTB_DECLARE(n, s, g, r, fn, p) uint32_t fn(uint16_t sel, const uint32_t *a);
CTB_ROUTINES(CTB_DECLARE)
#undef CTB_DECLARE

enum { MGR_CM = 1, MGR_FT = 2, MGR_TM = 3 };
static inline int ctb_mgr(uint16_t sel) { return sel >> 8; }

/* The fields the three managers' records share. */
enum { REC_procID = 0, REC_flags = 2, REC_errCode = 6, REC_refCon = 8, REC_userData = 12, REC_defProc = 16,
       REC_config = 20, REC_oldConfig = 24, REC_private = 40 };

/* ---- Tools and their configurations ---- */

/* A configuration is a block of the tool's (the record's config): a tag
 * naming the tool, then its fields, each with a name in config strings
 * ("Baud 9600 Parity None Port \"Modem Port\""). */
enum { CF_INT, CF_BOOL, CF_ENUM, CF_STR };
typedef struct {
    const char *key;
    uint8_t type;
    uint16_t off;              /* in the block: a long, a byte, a short, a Pascal string */
    int32_t lo, hi;            /* CF_INT: the range; CF_STR: hi the longest */
    const char *const *names;  /* CF_ENUM: the values' names, NULL-terminated */
    const char *def;           /* the default, as a config string has it */
} ctb_field;

typedef struct {
    const char *name;
    uint32_t tag;
    uint16_t size;             /* of the configuration block */
    const ctb_field *fields;
    int nfields;
} ctb_tool;

/* A manager's tools (procID n is tools[n - 1]) and what it does when a
 * record's configuration changes. */
typedef struct {
    const ctb_tool *const *tools;
    int ntools;
    void (*config_changed)(gaddr h);
    gaddr (*renew)(gaddr h, int16_t proc_id); /* the record again with another tool (Choose) */
    void (*dispose)(gaddr h);
} ctb_manager;
extern const ctb_manager cm_manager, tm_manager, ft_manager;

const ctb_tool *ctb_tool_of(int mgr, int16_t proc_id);
gaddr ctb_config_new(const ctb_tool *t);
void ctb_config_defaults(const ctb_tool *t, gaddr block);
/* A config string applied: 0, or where the parse stopped (1-based), or -1. */
int16_t ctb_config_parse(const ctb_tool *t, gaddr block, const char *s);
void ctb_config_string(const ctb_tool *t, gaddr block, char *out, size_t cap);
/* A string field as a C string. */
void ctb_config_str(gaddr block, uint16_t off, char *out, size_t cap);
/* The record's new configuration and the old one, as CMNew and its
 * kind set them up; and the record's own when it is disposed of. */
void ctb_record_init(gaddr h, int mgr, int16_t proc_id, uint32_t flags, int32_t refcon, int32_t user_data);
void ctb_record_free(gaddr h);

/* An application's routine (a ProcPtr or a UPP), Pascal conventions as
 * procInfo says; nothing for NULL. */
uint32_t ctb_callback(gaddr proc, uint32_t proc_info, const uint32_t *args, int nargs);
uint64_t ctb_now_ms(void);
/* Ticks (-1 for ever) as an end time in ms (UINT64_MAX for ever). */
uint64_t ctb_deadline(int32_t ticks);

/* The managers' own pollers and resets (ctb_cm.c, ctb_ft.c, ctb_tm.c). */
void cm_poll(void);
void cm_reset_all(void);
void tm_reset_all(void);
void ft_reset_all(void);

#endif
