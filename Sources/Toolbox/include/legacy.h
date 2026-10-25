/*
 * The old hardware and networking managers: SCSI, Slot, Power, Start,
 * Data Access, Ethernet, the PPC Toolbox, file mapping and the Device
 * Manager's installation calls (not a public API).
 */
#ifndef LEGACY_H
#define LEGACY_H

#include <stdbool.h>
#include <stdint.h>

#include "mem.h"

/* A new machine: every manager here back to its start. */
void legacy_reset(void);

/* power.c: the Start Manager's parameter RAM */
void startmgr_reset(void);
void startmgr_get_video(gaddr pb);

/* scsi.c */
void scsi_reset(void);

/* dbaccess.c */
void dbaccess_reset(void);

#endif
