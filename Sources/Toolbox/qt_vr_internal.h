/* QuickTime VR's state, shared by its engine (qt_vr.c) and its calls (qt_vr_lib.c). */
#ifndef QT_VR_INTERNAL_H
#define QT_VR_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#include "qt_vr.h"

enum {
    notAQTVRMovieErr = -30540, constraintReachedErr = -30541, callNotSupportedByNodeErr = -30542,
    selectorNotSupportedByNodeErr = -30543, invalidNodeIDErr = -30544, invalidViewStateErr = -30545,
    timeNotInViewErr = -30546, propertyNotSupportedByNodeErr = -30547, settingNotSupportedByNodeErr = -30548,
    limitReachedErr = -30549, invalidNodeFormatErr = -30550, invalidHotSpotIDErr = -30551,
};

enum { VR_PAN = 0, VR_TILT = 1, VR_FOV = 2, VR_VCH = 4, VR_VCV = 5 };
enum { kQTVRCurrentNode = 0, kQTVRPreviousNode = (int32_t)0x80000000, kQTVRDefaultNode = (int32_t)0x80000001 };
enum { kQTVRStatic = 1, kQTVRMotion = 2, kQTVRCurrentMode = 0, kQTVRAllModes = 100 };
/* The intercepted calls' selectors, from kQTVRSetPanAngleSelector. */
enum {
    SEL_PAN = 0x2000, SEL_TILT, SEL_FOV, SEL_CENTER, SEL_ENTER, SEL_WITHIN, SEL_LEAVE, SEL_DOWN, SEL_STILLDOWN,
    SEL_UP, SEL_TRIGGER, SEL_HOTTYPE, SEL_SETPARAM, SEL_GETPARAM, SEL_LAST = SEL_GETPARAM,
};

#define VR_MAX_NODES 512
#define VR_MAX_DISABLED 64

typedef struct {
    uint32_t id, type;      /* 'pano' or 'obje' */
    int64_t time, duration; /* movie time */
    uint8_t *info;          /* its node information atom container (12-byte header, then 'sean') */
    uint32_t info_len;
} vr_node;

typedef struct {
    gaddr proc;
    int32_t refcon;
    uint32_t flags;
} vr_proc;

typedef struct {
    float pan, tilt, width, height;
    uint32_t flags;
} vr_area;

typedef struct vr {
    bool used;
    gaddr token;            /* the QTVRInstance programs hold */
    gaddr movie, track, mc;
    bool old;               /* a QTVR 1.0 panorama ('STpn') or object ('stna') */
    bool old_object;
    uint8_t *world;         /* the VR world atom container */
    uint32_t world_len;
    vr_node *nodes;
    int nnodes, cur, prev;
    uint32_t default_node;

    /* the node shown */
    bool object, loaded;
    float lim_min[3], lim_max[3];   /* the file's pan, tilt, field of view (degrees) */
    float con_min[3], con_max[3];   /* the constraints (QTVRSetConstraints) */
    float pan, tilt, fov, vch, vcv;
    float def_pan, def_tilt, def_fov, def_vch, def_vcv;
    /* a panorama: its picture unrotated (iw across the pan range), and its
     * hot spots' IDs at hw by hh */
    uint32_t *image;
    int iw, ih;
    uint8_t *hot;
    int hw, hh;
    bool wraps, horizontal;
    uint32_t image_size[2], hot_size[2];
    uint16_t frames[2], hot_frames[2];
    gaddr image_track, hot_track;
    /* an object */
    uint32_t columns, rows, view_duration, ui, view_states, def_state, down_state, cur_state;
    float motion_scale, view_rate, frame_rate;
    uint32_t anim, control;
    int64_t view_time;      /* within the view's duration */
    bool frame_anim, view_anim;
    int64_t frame_dur;      /* an animation frame's duration (movie time) */
    uint32_t *obj_frame;    /* the frame decoded last, and its sample */
    int ow, oh;
    int64_t obj_frame_time;
    int64_t anim_last_ms;

    int units;              /* kQTVRDegrees (0) or kQTVRRadians (1) */
    bool visible, dirty, hots_visible, streaming;
    bool over_tracking, down_tracking, tracking_down;
    uint32_t over_id, down_id;
    uint32_t down_pt, last_pt, last_ticks; /* a click being tracked */
    bool moved;
    float rate[3];          /* QTVRSetViewParameter's rates of change (degrees a second) */
    int64_t rate_last_ms;
    bool all_hots_off;
    uint32_t off_ids[VR_MAX_DISABLED], off_types[VR_MAX_DISABLED];
    int noff_ids, noff_types;
    int32_t imaging[3][8];  /* per mode (static, motion): correction, quality, direct draw... */
    int mode;               /* kQTVRStatic or kQTVRMotion */
    uint32_t interaction[8];/* hysteresis, timeout, pan-tilt speed, zoom speed */
    bool translate_on_down;
    float mouse_motion_scale;
    uint32_t nudge_mode;
    uint32_t transition_type, transition_speed, transition_dir;
    bool transition_on;
    uint32_t bb_geometry, bb_format;
    uint16_t bb_resolution;
    int16_t bb_cache;
    uint16_t cursor_type;
    int16_t cursor_id;
    gaddr cursor_handle;
    vr_proc intercepts[SEL_LAST - SEL_PAN + 1];
    bool in_intercept[SEL_LAST - SEL_PAN + 1];
    vr_proc entering, leaving, mouse_over, prescreen, backbuf;
    vr_area *areas;
    int nareas;
    gaddr bb_world;         /* the back buffer's GWorld while a back buffer routine draws */
    /* where the view was drawn last */
    gaddr port;
    qrect box;
    uint32_t *view;
    int vw, vh;
} vr;

/* qt_vr.c */
vr *vr_for_token(gaddr token);
vr *vr_for_movie(gaddr movie, bool make);
gaddr vr_find_track(gaddr movie, int32_t index);
vr *vr_instance(gaddr track, gaddr mc);
int vr_node_index(const vr *v, uint32_t id);
int16_t vr_go_to_node(vr *v, int32_t id);
bool vr_set_angle(vr *v, int kind, float deg);    /* constrained; false if at a limit */
void vr_set_center(vr *v, float h, float v_);
void vr_effective_limits(const vr *v, int kind, float *lo, float *hi);
float vr_wrap_constrain(const vr *v, int kind, float value);
uint32_t vr_constraint_status(const vr *v);
void vr_show_default(vr *v);
void vr_update(vr *v, bool now);
bool vr_pt_to_angles(const vr *v, int h, int vv, float *pan, float *tilt);
uint32_t vr_hot_at(vr *v, int h, int vv);          /* local point: its hot spot (0 none) */
uint32_t vr_hot_type(const vr *v, uint32_t id);
bool vr_hot_known(const vr *v, uint32_t id);
bool vr_hot_enabled(const vr *v, uint32_t id);
int vr_visible_hots(vr *v, uint32_t *ids, int max);
gaddr vr_hot_region(vr *v, uint32_t id, gaddr rgn);
int16_t vr_trigger(vr *v, uint32_t id, const uint8_t *info, uint32_t info_len, uint32_t atom);
void vr_coord_from_angles(const vr *v, float pan, float tilt, float *x, float *y);
void vr_angles_from_coord(const vr *v, float x, float y, float *pan, float *tilt);
void vr_object_position(const vr *v, int *col, int *row);
float vr_column_pan(const vr *v, int col);
float vr_row_tilt(const vr *v, int row);
int vr_pan_column(const vr *v, float pan);
int vr_tilt_row(const vr *v, float tilt);
void vr_nudge(vr *v, int direction);
void vr_animate(vr *v);
void vr_refresh_back_buffer(vr *v);

/* qt_vr_lib.c: an intercepted call through the program's intercept
 * routine, if any (p: the call's parameters as the intercept record holds
 * them), then its own behaviour unless cancelled. */
int16_t vr_call(vr *v, uint32_t sel, uint32_t *p, int n);
int16_t vr_original(vr *v, uint32_t sel, const uint32_t *p);
/* The program's routines. */
int16_t vr_enter_proc(vr *v, uint32_t id);
bool vr_leave_proc(vr *v, uint32_t from, uint32_t to); /* true: cancelled */
void vr_mouse_over(vr *v, uint32_t id, uint32_t flags);
void vr_prescreen(vr *v);
void vr_idle_mouse(vr *v, uint32_t pt); /* a local point */

#endif
