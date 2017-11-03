#ifndef OPT_MALLOC
#define OPT_MALLOC

#include <stddef.h>

typedef struct hm_stats {
    long pages_mapped;
    long pages_unmapped;
    long chunks_allocated;
    long chunks_freed;
    long free_length;
} hm_stats;

hm_stats* hgetstats();
void hprintstats();


void* opt_malloc(size_t size);
void opt_free(void* item);
void* opt_realloc(void* prev, size_t size);

#endif
