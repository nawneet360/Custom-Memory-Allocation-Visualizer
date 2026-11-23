#include "custom_allocator.h"
#include <stdio.h>
#include <string.h>

#define ALLOC_MAGIC  0xCAFEBABEu   
#define FREE_MAGIC   0xDEADBEEFu  
#define ALIGN_UP(n)  (((n) + CA_ALIGNMENT - 1) & ~(size_t)(CA_ALIGNMENT - 1))
#define HEADER_SIZE  ALIGN_UP(sizeof(BlockHeader))

static unsigned char heap[CA_HEAP_SIZE] __attribute__((aligned(16)));
static BlockHeader  *first_block = NULL;

static AllocLogEntry history[CA_MAX_LOG];
static int history_count = 0;
static int total_allocs  = 0;
static int total_frees   = 0;


static void *payload_of(BlockHeader *b) { return (unsigned char *)b + HEADER_SIZE; }

static size_t offset_of(BlockHeader *b) { return (size_t)((unsigned char *)b - heap); }

static void log_event(AllocLogKind kind, void *ptr, size_t req, size_t act) {
    if (history_count >= CA_MAX_LOG) return;          /* log full: just stop recording */
    AllocLogEntry *e = &history[history_count];
    e->kind = kind;  e->ptr = ptr;  e->req_size = req;  e->act_size = act;
    e->seq = history_count++;
}

static BlockHeader *find_block(void *ptr) {
    for (BlockHeader *b = first_block; b; b = b->next)
        if (payload_of(b) == ptr) return b;
    return NULL;
}
static bool split_block(BlockHeader *b, size_t need) {
    if (b->size < need + HEADER_SIZE + CA_MIN_SPLIT_SIZE) return false;

    BlockHeader *rest = (BlockHeader *)((unsigned char *)b + HEADER_SIZE + need);
    rest->size    = b->size -need-HEADER_SIZE;
    rest->is_free = true;
    rest->magic   = FREE_MAGIC;
    rest->prev    = b;
    rest->next    = b->next;
    if (b->next) b->next->prev = rest;

    b->next = rest;
    b->size = need;
    return true;
}
static void merge_with_next(BlockHeader *b) {
    BlockHeader *n = b->next;
    if (!n || !n->is_free) return;

    b->size += HEADER_SIZE + n->size;
    b->next  = n->next;
    if (n->next) n->next->prev = b;
    n->magic = 0;                             
}

void ca_init(void) {
    memset(heap, 0, sizeof(heap));

    first_block          = (BlockHeader *)heap;      
    first_block->size    = CA_HEAP_SIZE - HEADER_SIZE;
    first_block->is_free = true;
    first_block->magic   = FREE_MAGIC;
    first_block->next    = NULL;
    first_block->prev    = NULL;

    history_count = total_allocs = total_frees = 0;
}


void *ca_malloc(size_t size) {
    if (size == 0 || size > CA_HEAP_SIZE) return NULL;
    if (!first_block) ca_init();

    size_t need = ALIGN_UP(size);

    for (BlockHeader *b = first_block; b; b = b->next) {     
        if (!b->is_free || b->size < need) continue;

        split_block(b, need);
        b->is_free = false;
        b->magic   = ALLOC_MAGIC;

        total_allocs++;
        log_event(CA_LOG_ALLOC, payload_of(b), size, b->size);
        return payload_of(b);
    }

    fprintf(stderr, "[ca_malloc] out of memory: could not find %zu bytes\n", size);
    return NULL;
}


bool ca_free(void *ptr) {
    if (!ptr) return true;                         

    BlockHeader *b = find_block(ptr);
    if (!b) {
        fprintf(stderr, "[ca_free] %p was never given out by this allocator\n", ptr);
        return false;
    }
    if (b->is_free) {
        fprintf(stderr, "[ca_free] double free at %p\n", ptr);
        return false;
    }
    if (b->magic != ALLOC_MAGIC) {
        fprintf(stderr, "[ca_free] header at %p is corrupted (magic 0x%X)\n", ptr, b->magic);
        return false;
    }

    b->is_free = true;
    b->magic   = FREE_MAGIC;
    total_frees++;
    log_event(CA_LOG_FREE, ptr, 0, b->size);

    
    merge_with_next(b);
    if (b->prev && b->prev->is_free) merge_with_next(b->prev);

    return true;
}



void *ca_realloc(void *ptr, size_t new_size) {
    if (!ptr)          return ca_malloc(new_size);
    if (new_size == 0) { ca_free(ptr); return NULL; }

    BlockHeader *b = find_block(ptr);
    if (!b || b->is_free || b->magic != ALLOC_MAGIC) {
        fprintf(stderr, "[ca_realloc] %p is not a live block\n", ptr);
        return NULL;
    }

    size_t need = ALIGN_UP(new_size);

    if (need <= b->size) {
        if (split_block(b, need)) merge_with_next(b->next);
        log_event(CA_LOG_REALLOC, ptr, new_size, b->size);
        return ptr;
    }

    
    BlockHeader *n = b->next;
    if (n && n->is_free && b->size + HEADER_SIZE + n->size >= need) {
        merge_with_next(b);
        split_block(b, need);
        log_event(CA_LOG_REALLOC, ptr, new_size, b->size);
        return ptr;
    }


    size_t old_size = b->size;
    void *fresh = ca_malloc(new_size);
    if (!fresh) return NULL;                       
    memcpy(fresh, ptr, old_size);
    ca_free(ptr);
    log_event(CA_LOG_REALLOC, fresh, new_size, ca_get_block_size(fresh));
    return fresh;
}

void *ca_calloc(size_t count, size_t elem_size) {
    if (elem_size != 0 && count > CA_HEAP_SIZE / elem_size) return NULL;  /* overflow guard */
    size_t total = count * elem_size;
    void *p = ca_malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

size_t ca_get_block_size(void *ptr) {
    BlockHeader *b = find_block(ptr);
    return (b && !b->is_free) ? b->size : 0;
}


void ca_get_stats(AllocStats *out) {
    memset(out, 0, sizeof(*out));
    out->total_heap = CA_HEAP_SIZE;

    for (BlockHeader *b = first_block; b; b = b->next) {
        out->metadata_overhead += HEADER_SIZE;
        if (b->is_free) {
            out->total_free += b->size;
            out->free_block_count++;
            if (b->size > out->largest_free_block) out->largest_free_block = b->size;
        } else {
            out->total_allocated += b->size;
            out->current_live++;
        }
    }
    out->alloc_count = total_allocs;
    out->free_count  = total_frees;


    if (out->total_free > 0)
        out->fragmentation_pct = 100.0 * (1.0 - (double)out->largest_free_block / out->total_free);
}



void ca_dump_heap(void) {
    printf("\n== Heap map ==\n");
    printf("  %-3s  %-8s  %-8s  %s\n", "#", "Offset", "Size", "Status");
    printf("  ---  --------  --------  ------\n");

    int i = 0;
    for (BlockHeader *b = first_block; b; b = b->next, i++)
        printf("  %-3d  0x%06zX  %-8zu  %s\n", i, offset_of(b), b->size,
               b->is_free ? "free" : "IN USE");

    printf("  (%d block(s), each with a %zu-byte header)\n\n", i, (size_t)HEADER_SIZE);
}

void ca_dump_stats(void) {
    AllocStats s;
    ca_get_stats(&s);

    printf("\n== Allocator stats ==\n");
    printf("  Heap size         : %zu bytes\n", s.total_heap);
    printf("  In use (payload)  : %zu bytes in %d block(s)\n", s.total_allocated, s.current_live);
    printf("  Free              : %zu bytes in %d piece(s)\n", s.total_free, s.free_block_count);
    printf("  Largest free piece: %zu bytes\n", s.largest_free_block);
    printf("  Header overhead   : %zu bytes\n", s.metadata_overhead);
    printf("  Fragmentation     : %.1f%%", s.fragmentation_pct);
    if (s.fragmentation_pct == 0.0)       printf("  (free space is all in one piece)\n");
    else if (s.fragmentation_pct < 30.0)  printf("  (a little scattered)\n");
    else                                  printf("  (badly scattered: big requests may fail)\n");
    printf("  Total mallocs/frees: %d / %d\n\n", s.alloc_count, s.free_count);
}

int ca_check_leaks(void) {
    int leaks = 0;
    printf("\n== Leak check ==\n");
    for (BlockHeader *b = first_block; b; b = b->next) {
        if (b->is_free) continue;
        printf("  leak: %zu bytes at offset 0x%zX were never freed\n", b->size, offset_of(b));
        leaks++;
    }
    if (leaks == 0) printf("  No leaks. Every malloc has a matching free.\n\n");
    else            printf("  %d leaked block(s). Every malloc needs a matching free!\n\n", leaks);
    return leaks;
}

void ca_dump_alloc_log(void) {
    printf("\n== History (%d operation(s)) ==\n", history_count);
    if (history_count == 0) { printf("  Nothing yet. Try: int* p = malloc(16);\n\n"); return; }

    static const char *names[] = { "malloc", "free", "realloc" };
    printf("  %-4s  %-8s  %-14s  %-9s  %s\n", "#", "Action", "Pointer", "Asked", "Block size");
    for (int i = 0; i < history_count; i++) {
        AllocLogEntry *e = &history[i];
        printf("  %-4d  %-8s  %-14p  %-9zu  %zu\n",
               e->seq + 1, names[e->kind], e->ptr, e->req_size, e->act_size);
    }
    if (history_count == CA_MAX_LOG) printf("  (log is full; later operations are not recorded)\n");
    printf("\n");
}

bool ca_validate_heap(void) {
    bool ok = true;
    size_t total = 0;
    int i = 0;
    BlockHeader *prev = NULL;

    printf("\n== Heap check ==\n");
    for (BlockHeader *b = first_block; b; prev = b, b = b->next, i++) {
        if (b->prev != prev) {
            printf("  block %d: broken prev link\n", i);               ok = false;
        }
        if (b->magic != (b->is_free ? FREE_MAGIC : ALLOC_MAGIC)) {
            printf("  block %d: bad magic 0x%X (header overwritten?)\n", i, b->magic); ok = false;
        }
        if (b->size % CA_ALIGNMENT != 0) {
            printf("  block %d: size %zu is not aligned\n", i, b->size); ok = false;
        }
        if (b->is_free && b->next && b->next->is_free) {
            printf("  block %d: two free blocks side by side (missed merge)\n", i); ok = false;
        }
        total += HEADER_SIZE + b->size;
        if (total > CA_HEAP_SIZE) {
            printf("  block %d: runs past the end of the heap\n", i);  ok = false; break;
        }
    }
    if (ok && total != CA_HEAP_SIZE) {
        printf("  blocks cover %zu bytes but heap is %d\n", total, CA_HEAP_SIZE); ok = false;
    }

    if (ok) printf("  Healthy: %d block(s), sizes add up, no corruption.\n\n", i);
    else    printf("\n");
    return ok;
}
