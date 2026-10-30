/*
 * Open Transport's configurations (OTNtvUtilLib's OTCfig* calls, and
 * OTCreateConfiguration's), configurators and control streams, and the rest
 * of the native utility set: shared client memory, open handlers, control
 * masks; OTConfigLib's port calls.
 *
 * A configuration is a tree of provider names: "tcp" over "ip" over the
 * port "enet".  The program holds a node's reference; it points at the
 * node's path string, which the calls that open providers read
 * ("tilisten,tcp(Opt=1)": a module pushed above is written first), then
 * the provider's name, the options' TNetbuf and the options.  The tree
 * itself is kept here.  OTConfigureChildren fills in what is below a node
 * as Open Transport's configurators did, down to the port.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "machine.h"
#include "ot_internal.h"
#include "ppc_bridge.h"
#include "tb_internal.h"

#define R(n) (c->gpr[3 + (n)])
#define RETURN(v) do { c->gpr[3] = (uint32_t)(v); c->pc = c->lr; } while (0)

enum { MAX_NODES = 256, MAX_KIDS = 8, NAMESZ = 32, OPTSZ = 128 };
/* The node's block: the path, the name, the option netbuf, the options. */
enum { B_PATH = 0, B_NAME = 256, B_NETBUF = 288, B_OPTS = 300, B_SIZE = B_OPTS + OPTSZ };

typedef struct {
    bool used;
    gaddr addr;
    char name[NAMESZ], opts[OPTSZ];
    int parent, kid[MAX_KIDS], nkids;
    bool above;          /* a module pushed above its child: part of the path */
    uint32_t port, flags;
} cnode;

static cnode nodes[MAX_NODES];

static void cstr_at(gaddr a, char *out, size_t size)
{
    size_t i = 0;
    for (; a && a < 0xFFFFFFF0u && i < size - 1 && rd8(a + i); i++)
        out[i] = (char)rd8(a + i);
    out[i] = 0;
}

static int node_of(gaddr ref)
{
    if (!ref)
        return -1;
    for (int i = 0; i < MAX_NODES; i++)
        if (nodes[i].used && nodes[i].addr == ref)
            return i;
    return -1;
}

/* One element of a path, "name(options)", into a node's fields. */
static void set_element(cnode *n, const char *el, size_t len)
{
    size_t k = 0;
    while (k < len && el[k] != '(')
        k++;
    snprintf(n->name, sizeof n->name, "%.*s", (int)(k < NAMESZ - 1 ? k : NAMESZ - 1), el);
    n->opts[0] = 0;
    if (k < len) {
        size_t e = len;
        if (e > k && el[e - 1] == ')')
            e--;
        snprintf(n->opts, sizeof n->opts, "%.*s", (int)(e - k - 1), el + k + 1);
    }
}

static void write_path(int i, char *out, size_t size)
{
    cnode *n = &nodes[i];
    snprintf(out, size, n->opts[0] ? "%s(%s)" : "%s", n->name, n->opts);
    if (n->above && n->nkids) {
        size_t at = strlen(out);
        if (at + 1 < size) {
            out[at] = ',';
            write_path(n->kid[0], out + at + 1, size - at - 1);
        }
    }
}

/* The node's block brought up to date, and those above it whose paths
 * include it. */
static void refresh(int i)
{
    for (int depth = 0; i >= 0 && depth < MAX_NODES; depth++) {
        cnode *n = &nodes[i];
        char path[256];
        write_path(i, path, sizeof path);
        gmemset(n->addr, 0, B_OPTS);
        gput(n->addr + B_PATH, path, (uint32_t)strlen(path));
        gput(n->addr + B_NAME, n->name, (uint32_t)strlen(n->name));
        uint32_t olen = (uint32_t)strlen(n->opts);
        gput(n->addr + B_OPTS, n->opts, olen);
        wr32(n->addr + B_NETBUF, OPTSZ);
        wr32(n->addr + B_NETBUF + 4, olen);
        wr32(n->addr + B_NETBUF + 8, n->addr + B_OPTS);
        int p = n->parent;
        if (p < 0 || !nodes[p].above || !nodes[p].nkids || nodes[p].kid[0] != i)
            break;
        i = p;
    }
}

static int new_node(const char *el, size_t len)
{
    int i = 0;
    while (i < MAX_NODES && nodes[i].used)
        i++;
    if (i == MAX_NODES)
        return -1;
    gaddr a = tb_NewPtr(B_SIZE, true, true);
    if (!a)
        return -1;
    nodes[i] = (cnode){ .used = true, .addr = a, .parent = -1 };
    set_element(&nodes[i], el, len);
    refresh(i);
    return i;
}

static void attach(int parent, int child, int at)
{
    cnode *p = &nodes[parent];
    if (p->nkids >= MAX_KIDS)
        return;
    if (at < 0 || at > p->nkids)
        at = p->nkids;
    memmove(&p->kid[at + 1], &p->kid[at], sizeof p->kid[0] * (size_t)(p->nkids - at));
    p->kid[at] = child;
    p->nkids++;
    nodes[child].parent = parent;
}

static int detach(int child)
{
    int p = nodes[child].parent;
    if (p < 0)
        return -1;
    cnode *n = &nodes[p];
    int at = 0;
    while (at < n->nkids && n->kid[at] != child)
        at++;
    if (at < n->nkids) {
        memmove(&n->kid[at], &n->kid[at + 1], sizeof n->kid[0] * (size_t)(n->nkids - at - 1));
        n->nkids--;
    }
    nodes[child].parent = -1;
    return at;
}

static void delete_tree(int i)
{
    if (i < 0 || !nodes[i].used)
        return;
    while (nodes[i].nkids)
        delete_tree(nodes[i].kid[--nodes[i].nkids]);
    detach(i);
    tb_DisposePtr(nodes[i].addr);
    nodes[i].used = false;
}

static int clone_tree(int i)
{
    int k = new_node("", 0);
    if (k < 0)
        return -1;
    nodes[k].above = nodes[i].above;
    nodes[k].port = nodes[i].port;
    nodes[k].flags = nodes[i].flags;
    memcpy(nodes[k].name, nodes[i].name, sizeof nodes[k].name);
    memcpy(nodes[k].opts, nodes[i].opts, sizeof nodes[k].opts);
    for (int j = 0; j < nodes[i].nkids; j++) {
        int kc = clone_tree(nodes[i].kid[j]);
        if (kc >= 0)
            attach(k, kc, -1);
    }
    refresh(k);
    return k;
}

/* A path made into nodes: "a,b(opts),c", each element a module above the
 * next.  The top one's index, or -1. */
static int parse_path(const char *path)
{
    int top = -1, prev = -1;
    const char *s = path;
    while (*s) {
        const char *e = s;
        int depth = 0;
        while (*e && (depth || *e != ',')) {
            depth += *e == '(' ? 1 : *e == ')' ? -1 : 0;
            e++;
        }
        int n = new_node(s, (size_t)(e - s));
        if (n < 0)
            break;
        if (prev >= 0) {
            nodes[prev].above = true;
            attach(prev, n, -1);
        }
        if (top < 0)
            top = n;
        prev = n;
        s = *e ? e + 1 : e;
    }
    for (int i = prev; i >= 0; i = nodes[i].parent)
        refresh(i);
    return top;
}

/* A configuration, by path: the reference to its top node. */
gaddr ot_config_new(const char *path)
{
    int i = parse_path(path);
    if (i < 0) /* an empty path is still a configuration */
        i = new_node("", 0);
    return i >= 0 ? nodes[i].addr : 0;
}

/* The node for a reference, made from its path if the program made it some
 * other way (a string of its own). */
static int node_for(gaddr ref)
{
    int i = node_of(ref);
    if (i >= 0 || !ref || ref >= 0xFFFFFFF0u)
        return i;
    char path[256];
    cstr_at(ref, path, sizeof path);
    return node_of(ot_config_new(path));
}


/* ---- OTCreateConfiguration and its kin ---- */

/* OTCreateConfiguration(path); the special paths (kDefaultInternetServicesPath,
 * -3) stay what they are, for the opens that know them. */
static void cfg_Create(ppc_cpu *c)
{
    if (R(0) >= 0xFFFFFFF0u) {
        RETURN(R(0));
        return;
    }
    char path[256];
    cstr_at(R(0), path, sizeof path);
    RETURN(ot_config_new(path));
}

static void cfg_Clone(ppc_cpu *c)
{
    if (R(0) >= 0xFFFFFFF0u) {
        RETURN(R(0));
        return;
    }
    int i = node_for(R(0));
    int k = i >= 0 ? clone_tree(i) : -1;
    RETURN(k >= 0 ? nodes[k].addr : 0);
}

static void cfg_Delete(ppc_cpu *c)
{
    int i = node_of(R(0));
    if (i >= 0) {
        /* the whole tree it is in */
        while (nodes[i].parent >= 0)
            i = nodes[i].parent;
        delete_tree(i);
    }
    c->pc = c->lr;
}

/* ---- Building the tree ---- */

static int node_from(gaddr path, int32_t *err)
{
    char s[256];
    cstr_at(path, s, sizeof s);
    int n = new_node(s, strlen(s));
    *err = n < 0 ? kOTOutOfMemoryErr : kOTNoError;
    return n;
}

static void set_err(gaddr errp, int32_t err)
{
    if (errp)
        wr32(errp, (uint32_t)err);
}

/* OTCfigPushParent(cfig, path, &err): a module above the node. */
static void cfg_PushParent(ppc_cpu *c)
{
    int i = node_for(R(0));
    int32_t err = kOTBadConfigurationErr;
    int n = i >= 0 ? node_from(R(1), &err) : -1;
    if (n >= 0) {
        int p = nodes[i].parent, at = p >= 0 ? detach(i) : -1;
        nodes[n].above = true;
        attach(n, i, 0);
        if (p >= 0)
            attach(p, n, at);
        refresh(i);
    }
    set_err(R(2), err);
    RETURN(n >= 0 ? nodes[n].addr : 0);
}

/* OTCfigPushNewSingleChild(cfig, path, &err): the node's only child, its
 * children moved under it. */
static void cfg_PushNewSingleChild(ppc_cpu *c)
{
    int i = node_for(R(0));
    int32_t err = kOTBadConfigurationErr;
    int n = i >= 0 ? node_from(R(1), &err) : -1;
    if (n >= 0) {
        while (nodes[i].nkids) {
            int k = nodes[i].kid[0];
            detach(k);
            attach(n, k, -1);
        }
        attach(i, n, -1);
        refresh(n);
        refresh(i);
    }
    set_err(R(2), err);
    RETURN(n >= 0 ? nodes[n].addr : 0);
}

/* OTCfigPushChild(cfig, index, path, &err): a node between it and a child. */
static void cfg_PushChild(ppc_cpu *c)
{
    int i = node_for(R(0));
    int32_t err = kOTBadConfigurationErr;
    uint32_t at = R(1);
    int n = i >= 0 && at < (uint32_t)nodes[i].nkids ? node_from(R(2), &err) : -1;
    if (n >= 0) {
        int k = nodes[i].kid[at];
        detach(k);
        attach(n, k, -1);
        attach(i, n, (int)at);
        refresh(n);
        refresh(i);
    }
    set_err(R(3), err);
    RETURN(n >= 0 ? nodes[n].addr : 0);
}

/* OTCfigPopChild(cfig, index): the child taken out, its children in its place. */
static void cfg_PopChild(ppc_cpu *c)
{
    int i = node_for(R(0));
    uint32_t at = R(1);
    if (i < 0 || at >= (uint32_t)nodes[i].nkids) {
        RETURN((uint32_t)kOTNotFoundErr);
        return;
    }
    int k = nodes[i].kid[at];
    detach(k);
    int pos = (int)at;
    while (nodes[k].nkids) {
        int g = nodes[k].kid[0];
        detach(g);
        attach(i, g, pos++);
    }
    delete_tree(k);
    refresh(i);
    RETURN(kOTNoError);
}

/* OTCfigNewChild(cfig, path, &err) */
static void cfg_NewChild(ppc_cpu *c)
{
    int i = node_for(R(0));
    int32_t err = kOTBadConfigurationErr;
    int n = i >= 0 && nodes[i].nkids < MAX_KIDS ? node_from(R(1), &err) : -1;
    if (n >= 0) {
        attach(i, n, -1);
        refresh(i);
    }
    set_err(R(2), err);
    RETURN(n >= 0 ? nodes[n].addr : 0);
}

/* OTCfigAddChild(cfig, child) */
static void cfg_AddChild(ppc_cpu *c)
{
    int i = node_for(R(0)), k = node_for(R(1));
    if (i < 0 || k < 0 || nodes[i].nkids >= MAX_KIDS) {
        RETURN((uint32_t)kOTBadConfigurationErr);
        return;
    }
    detach(k);
    attach(i, k, -1);
    refresh(i);
    RETURN(kOTNoError);
}

/* OTCfigRemoveChild(cfig, index): the child, on its own now. */
static void cfg_RemoveChild(ppc_cpu *c)
{
    int i = node_for(R(0));
    if (i < 0 || R(1) >= (uint32_t)nodes[i].nkids) {
        RETURN(0);
        return;
    }
    int k = nodes[i].kid[R(1)];
    detach(k);
    refresh(i);
    RETURN(nodes[k].addr);
}

/* OTCfigSetPath(cfig, path): the node's own name and options. */
static void cfg_SetPath(ppc_cpu *c)
{
    int i = node_for(R(0));
    if (i < 0) {
        RETURN((uint32_t)kOTBadConfigurationErr);
        return;
    }
    char s[256];
    cstr_at(R(1), s, sizeof s);
    set_element(&nodes[i], s, strlen(s));
    refresh(i);
    RETURN(kOTNoError);
}

static void cfg_ChangeProviderName(ppc_cpu *c)
{
    int i = node_for(R(0));
    if (i >= 0) {
        cstr_at(R(1), nodes[i].name, sizeof nodes[i].name);
        refresh(i);
    }
    c->pc = c->lr;
}

static void cfg_SetPortRef(ppc_cpu *c)
{
    int i = node_for(R(0));
    if (i >= 0)
        nodes[i].port = R(1);
    c->pc = c->lr;
}

/* ---- Looking at it ---- */

static void cfg_NumberOfChildren(ppc_cpu *c)
{
    int i = node_for(R(0));
    RETURN(i >= 0 ? nodes[i].nkids : 0);
}

static void cfg_GetParent(ppc_cpu *c)
{
    int i = node_for(R(0));
    RETURN(i >= 0 && nodes[i].parent >= 0 ? nodes[nodes[i].parent].addr : 0);
}

static void cfg_GetChild(ppc_cpu *c)
{
    int i = node_for(R(0));
    RETURN(i >= 0 && R(1) < (uint32_t)nodes[i].nkids ? nodes[nodes[i].kid[R(1)]].addr : 0);
}

static void cfg_GetOptionNetbuf(ppc_cpu *c)
{
    int i = node_for(R(0));
    RETURN(i >= 0 ? nodes[i].addr + B_NETBUF : 0);
}

static void cfg_GetPortRef(ppc_cpu *c)
{
    int i = node_for(R(0));
    RETURN(i >= 0 ? nodes[i].port : 0);
}

static void cfg_GetInstallFlags(ppc_cpu *c)
{
    int i = node_for(R(0));
    RETURN(i >= 0 ? nodes[i].flags : 0);
}

static void cfg_GetProviderName(ppc_cpu *c)
{
    int i = node_for(R(0));
    RETURN(i >= 0 ? nodes[i].addr + B_NAME : 0);
}

static void cfg_IsPort(ppc_cpu *c)
{
    int i = node_for(R(0));
    RETURN(i >= 0 && nodes[i].port != 0);
}

/* ---- OTConfigureChildren: what is under a node, as the configurators
 * built it (tcp, udp and rawip over ip; AppleTalk's protocols over ddp;
 * ip and ddp over the default port). */

static bool is_one_of(const char *s, const char *const *list)
{
    for (int i = 0; list[i]; i++)
        if (!strcmp(s, list[i]))
            return true;
    return false;
}

static int add_below(int i, const char *name)
{
    int n = new_node(name, strlen(name));
    if (n >= 0)
        attach(i, n, -1);
    return n;
}

static void configure(int i, int depth)
{
    static const char *const over_ip[] = { "tcp", "udp", "rawip", "dnr", NULL };
    static const char *const over_ddp[] = { "atp", "adsp", "asp", "pap", "nbp", "zip", "atlk", NULL };
    static const char *const ports[] = { "enet", "ltlk", "tokn", "fddi", "ppp", "slip", NULL };
    if (i < 0 || depth > 8)
        return;
    cnode *n = &nodes[i];
    if (!n->nkids) {
        if (is_one_of(n->name, over_ip))
            add_below(i, "ip");
        else if (is_one_of(n->name, over_ddp))
            add_below(i, "ddp");
        else if (!strcmp(n->name, "ip") || !strcmp(n->name, "ddp") || !strcmp(n->name, "arp")) {
            int p = add_below(i, "enet");
            if (p >= 0)
                nodes[p].port = ot_default_port_ref();
        }
    }
    bool port = false;
    for (int k = 0; ports[k]; k++)
        if (!strncmp(n->name, ports[k], strlen(ports[k])))
            port = true;
    if (port && !n->port)
        n->port = ot_default_port_ref();
    for (int k = 0; k < nodes[i].nkids; k++)
        configure(nodes[i].kid[k], depth + 1);
    refresh(i);
}

static void cfg_ConfigureChildren(ppc_cpu *c)
{
    int i = node_for(R(0));
    if (i < 0) {
        RETURN((uint32_t)kOTBadConfigurationErr);
        return;
    }
    configure(i, 0);
    RETURN(kOTNoError);
}

/* ---- Configurators ---- */

enum { MAX_CFIGORS = 16 };
static struct {
    gaddr ref, user, configure, create, event;
    bool m68k;
} cfigors[MAX_CFIGORS];

static int cfigor_of(gaddr ref)
{
    for (int i = 0; i < MAX_CFIGORS; i++)
        if (ref && cfigors[i].ref == ref)
            return i;
    return -1;
}

/* OTNewConfigurator(userData, configure, createStream, handleEvent): the
 * configurator's object (its first long is Open Transport's). */
static void cfg_NewConfigurator(ppc_cpu *c)
{
    int i;
    for (i = 0; i < MAX_CFIGORS && cfigors[i].ref; i++)
        ;
    gaddr ref = i < MAX_CFIGORS ? tb_NewPtr(8, true, true) : 0;
    if (ref) {
        cfigors[i].ref = ref;
        cfigors[i].user = R(0);
        cfigors[i].configure = R(1);
        cfigors[i].create = R(2);
        cfigors[i].event = R(3);
        cfigors[i].m68k = ot_from_68k;
    }
    RETURN(ref);
}

static void cfg_DeleteConfigurator(ppc_cpu *c)
{
    int i = cfigor_of(R(0));
    if (i >= 0) {
        tb_DisposePtr(cfigors[i].ref);
        cfigors[i].ref = 0;
    }
    c->pc = c->lr;
}

static void cfg_GetConfiguratorUserData(ppc_cpu *c)
{
    int i = cfigor_of(R(0));
    RETURN(i >= 0 ? cfigors[i].user : 0);
}

/* OTIsMasterConfigurator(cfigor): the program's never are. */
static void cfg_IsMasterConfigurator(ppc_cpu *c) { RETURN(false); }

/* OTCreateConfigurator / OTFindConfigurator / OTGetIndexedConfigurator:
 * Open Transport's own configurators aren't objects a program can have. */
static void cfg_none(ppc_cpu *c) { RETURN(0); }

/* OTCreateControlStream(cfig, cfigor, proc, context): an asynchronous
 * stream to the configuration's driver; kStreamOpenEvent brings it. */
static void cfg_CreateControlStream(ppc_cpu *c)
{
    void (*fn)(ppc_cpu *) = cfm_export_fn("OTAsyncCreateStream");
    uint32_t cfg = R(0), proc = R(2), ctx = R(3);
    R(0) = cfg;
    R(1) = 0;
    R(2) = proc;
    R(3) = ctx;
    if (fn)
        fn(c);
    else
        RETURN((uint32_t)kOTNotSupportedErr);
}

/* OTNewControlMask(): a bit of its own each time. */
static uint32_t masks_given;
static void cfg_NewControlMask(ppc_cpu *c)
{
    uint32_t bit = masks_given < 32 ? 1u << masks_given++ : 0;
    RETURN(bit);
}

/* ---- Memory, handlers ---- */

/* OTAllocSharedClientMem(size) / OTFreeSharedClientMem(mem): memory that
 * outlives the program's heap, in the system heap. */
static void cfg_AllocSharedClientMem(ppc_cpu *c) { RETURN(tb_NewPtr((int32_t)R(0), true, false)); }

static void cfg_FreeSharedClientMem(ppc_cpu *c)
{
    if (R(0))
        tb_DisposePtr(R(0));
    c->pc = c->lr;
}

/* OTRegisterOpenHandler / OTUnregisterOpenHandler: kernel-level hooks on
 * provider opens, which nothing here would call. */
static void cfg_noErr(ppc_cpu *c) { RETURN(kOTNoError); }
static void cfg_void(ppc_cpu *c) { c->pc = c->lr; }

/* ---- OTConfigLib ---- */

/* OTGetDefaultPort(): the port TCP/IP and AppleTalk use. */
static void cfg_GetDefaultPort(ppc_cpu *c) { RETURN(ot_default_port_ref()); }

void ot_config_reset(void)
{
    memset(nodes, 0, sizeof nodes);
    memset(cfigors, 0, sizeof cfigors);
    masks_given = 0;
}

const ppc_export otf_exports[] = {
    { "OTCreateConfiguration", cfg_Create }, { "OTCloneConfiguration", cfg_Clone },
    { "OTDestroyConfiguration", cfg_Delete },
    { "OTCfigNewConfiguration", cfg_Create }, { "OTCfigDeleteConfiguration", cfg_Delete },
    { "OTCfigCloneConfiguration", cfg_Clone }, { "OTCfigPushNewSingleChild", cfg_PushNewSingleChild },
    { "OTCfigPopChild", cfg_PopChild }, { "OTCfigPushParent", cfg_PushParent }, { "OTCfigPushChild", cfg_PushChild },
    { "OTCfigAddChild", cfg_AddChild }, { "OTCfigNewChild", cfg_NewChild }, { "OTCfigSetPath", cfg_SetPath },
    { "OTCfigRemoveChild", cfg_RemoveChild }, { "OTCfigSetPortRef", cfg_SetPortRef },
    { "OTCfigChangeProviderName", cfg_ChangeProviderName }, { "OTCfigNumberOfChildren", cfg_NumberOfChildren },
    { "OTCfigGetParent", cfg_GetParent }, { "OTCfigGetOptionNetbuf", cfg_GetOptionNetbuf },
    { "OTCfigGetPortRef", cfg_GetPortRef }, { "OTCfigGetInstallFlags", cfg_GetInstallFlags },
    { "OTCfigGetProviderName", cfg_GetProviderName }, { "OTCfigIsPort", cfg_IsPort },
    { "OTCfigGetChild", cfg_GetChild }, { "OTConfigureChildren", cfg_ConfigureChildren },
    { "OTNewConfigurator", cfg_NewConfigurator }, { "OTDeleteConfigurator", cfg_DeleteConfigurator },
    { "OTGetConfiguratorUserData", cfg_GetConfiguratorUserData },
    { "OTIsMasterConfigurator", cfg_IsMasterConfigurator }, { "OTConfiguratorUnloaded", cfg_void },
    { "OTCreateConfigurator", cfg_none }, { "OTFindConfigurator", cfg_none }, { "OTGetIndexedConfigurator", cfg_none },
    { "OTCreateControlStream", cfg_CreateControlStream }, { "OTNewControlMask", cfg_NewControlMask },
    { "OTAllocSharedClientMem", cfg_AllocSharedClientMem }, { "OTFreeSharedClientMem", cfg_FreeSharedClientMem },
    { "OTRegisterOpenHandler", cfg_noErr }, { "OTUnregisterOpenHandler", cfg_noErr },
    { "OTFindClientRecord", cfg_none }, { "OTGetDefaultPort", cfg_GetDefaultPort },
};
const unsigned otf_export_count = sizeof otf_exports / sizeof otf_exports[0];
