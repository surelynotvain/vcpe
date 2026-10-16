/* Stable-address, demand-grown record store. Pages never move; indexes are stable until explicitly removed.
 * Header-only so existing engine clients need no new link dependency. Main thread only. */
#ifndef VCPE_STORE_H
#define VCPE_STORE_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#define VCPE_STORE_PAGES 64
typedef struct {
    uint8_t *pages[VCPE_STORE_PAGES];
    uint32_t stride, per_page, count, used, failures;
} VcpeStore;
static inline int vcpe_store_init(VcpeStore *s, uint32_t size, uint32_t per_page)
{
    if (!s || !size || size > UINT32_MAX - sizeof(void *) || !per_page || per_page > 65536) return -1;
    size = (size + sizeof(void *) - 1) & ~(uint32_t)(sizeof(void *) - 1);
    if (size > (UINT32_MAX - per_page) / per_page) return -1;
    memset(s, 0, sizeof *s); s->stride=size; s->per_page=per_page; return 0;
}
static inline void *vcpe_store_get(const VcpeStore *s, uint32_t id)
{
    if (!s->per_page || id >= s->count) return 0;
    uint32_t page=id/s->per_page, slot=id%s->per_page;
    uint8_t *p=s->pages[page];
    return p && p[s->per_page*s->stride+slot] ? p+slot*s->stride : 0;
}
static inline int vcpe_store_add(VcpeStore *s, uint32_t *id, void **record)
{
    if (!s || !s->per_page || !id || !record) return -1;
    for (uint32_t p=0; p<VCPE_STORE_PAGES; p++) {
        uint8_t *mem=s->pages[p];
        if (!mem) {
            mem=calloc(1,(size_t)s->per_page*(s->stride+1));
            if (!mem) { s->failures++; return -1; }
            s->pages[p]=mem;
        }
        uint8_t *live=mem+s->per_page*s->stride;
        for (uint32_t i=0; i<s->per_page; i++) if (!live[i]) {
            live[i]=1; *id=p*s->per_page+i; *record=mem+i*s->stride;
            memset(*record,0,s->stride); s->used++;
            if (*id>=s->count) s->count=*id+1;
            return 0;
        }
    }
    s->failures++; return -1;
}
static inline void vcpe_store_remove(VcpeStore *s, uint32_t id)
{
    if (vcpe_store_get(s,id)) { s->pages[id/s->per_page][s->per_page*s->stride+id%s->per_page]=0; s->used--; }
}
static inline uint32_t vcpe_store_trim(VcpeStore *s)
{
    uint32_t freed=0;
    if (!s->per_page) return 0;
    for (uint32_t p=0; p<VCPE_STORE_PAGES; p++) if (s->pages[p]) {
        uint8_t *live=s->pages[p]+s->per_page*s->stride; uint32_t i=0;
        while (i<s->per_page && !live[i]) i++;
        if (i==s->per_page) { free(s->pages[p]); s->pages[p]=0; freed+=s->per_page*(s->stride+1); }
    }
    return freed;
}
static inline void vcpe_store_free(VcpeStore *s)
{
    for (uint32_t p=0; p<VCPE_STORE_PAGES; p++) free(s->pages[p]);
    memset(s,0,sizeof *s);
}
#endif
