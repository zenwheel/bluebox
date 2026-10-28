/*
 * QuickDraw 3D's storage and files (QD3DStorage.h, QD3DIO.h): to come.
 */
#include <stdlib.h>

#include "q3_view.h"

uint8_t *q3_storage_bytes(gaddr storage, uint32_t *size)
{
    *size = 0;
    return NULL;
}
void q3_file_write_object(gaddr file, gaddr object, q3view *v) {}
bool q3_file_start_writing(gaddr file) { return false; }
bool q3_file_end_writing(gaddr file) { return false; }
void q3_file_classes(void) {}
gaddr q3_read_3dmf(const uint8_t *bytes, uint32_t size) { return 0; }
uint8_t *q3_write_3dmf(gaddr object, uint32_t *size)
{
    *size = 0;
    return NULL;
}

const ppc_export q3_file_exports[] = { { "", NULL } };
const unsigned q3_file_export_count = 0;
