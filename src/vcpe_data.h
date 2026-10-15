/* Read-only converted structured data. No VM, object allocation or host runtime.
 * VDAT stores typed values and offset arrays; objects have sorted UTF-8 keys.
 * Buffers remain owned by the caller. Accessors reject truncated/out-of-range data. */
#ifndef VCPE_DATA_H
#define VCPE_DATA_H
#include <stdint.h>
typedef struct { const uint8_t *base; uint32_t size, root; } VcpeData;
typedef struct { const VcpeData *doc; uint32_t off; } VcpeValue;
enum { VCPE_D_NULL, VCPE_D_INT, VCPE_D_FLOAT, VCPE_D_STRING, VCPE_D_ARRAY, VCPE_D_OBJECT, VCPE_D_BYTES };
int vcpe_data_open(VcpeData *d, const void *bytes, uint32_t size);
VcpeValue vcpe_data_root(const VcpeData *d);
VcpeValue vcpe_data_get(VcpeValue v, const char *key);
VcpeValue vcpe_data_at(VcpeValue v, int i);
VcpeValue vcpe_data_member(VcpeValue v, int i);
int vcpe_data_kind(VcpeValue v);
int vcpe_data_count(VcpeValue v);
int32_t vcpe_data_int(VcpeValue v);
double vcpe_data_number(VcpeValue v);
const char *vcpe_data_string(VcpeValue v);
const void *vcpe_data_bytes(VcpeValue v, uint32_t *size);
#endif
