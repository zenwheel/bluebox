/*
 * CarbonLib's names for the user and the machine (CSCopyUserName and
 * CSCopyMachineName, CarbonLib 1.3): Sharing Setup's owner and computer
 * name, taken from the host account and the host's name as
 * GetDefaultUser takes the owner.
 */
#include <pwd.h>
#include <string.h>
#include <unistd.h>

#include "carbon_cf.h"
#include "ppc_bridge.h"
#include "tb_internal.h"

#define R(n) (c->gpr[3 + (n)])
#define RETURN(v) do { c->gpr[3] = (uint32_t)(v); c->pc = c->lr; } while (0)

/* CSCopyUserName(useShortName): the account's short name, or its full one. */
static void CSCopyUserName(ppc_cpu *c)
{
    const struct passwd *pw = getpwuid(getuid());
    const char *name = "User";
    if (pw && (uint8_t)R(0) && pw->pw_name && *pw->pw_name)
        name = pw->pw_name;
    else if (pw && pw->pw_gecos && *pw->pw_gecos)
        name = pw->pw_gecos;
    else if (pw && pw->pw_name && *pw->pw_name)
        name = pw->pw_name;
    RETURN(cfstr_from_utf8(name));
}

/* CSCopyMachineName(): the host's name, without its domain. */
static void CSCopyMachineName(ppc_cpu *c)
{
    char host[256] = "Macintosh";
    if (gethostname(host, sizeof host - 1) != 0 || !host[0])
        strcpy(host, "Macintosh");
    host[sizeof host - 1] = 0;
    char *dot = strchr(host, '.');
    if (dot && dot != host)
        *dot = 0;
    RETURN(cfstr_from_utf8(host));
}

const ppc_export carbonuser_exports[] = {
    { "CSCopyMachineName", CSCopyMachineName },
    { "CSCopyUserName", CSCopyUserName },
};
const unsigned carbonuser_export_count = sizeof carbonuser_exports / sizeof carbonuser_exports[0];
