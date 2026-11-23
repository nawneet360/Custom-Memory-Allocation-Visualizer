

#ifndef CUSTOM_ALLOCATOR_H
#define CUSTOM_ALLOCATOR_H

#include <stddef.h>
#include <stdbool.h>

#define CA_HEAP_SIZE       (64 * 1024)  
#define CA_ALIGNMENT       8           
#define CA_MIN_SPLIT_SIZE  32        
#define CA_MAX_LOG         256         

typedef struct BlockHeader {
    size_t              size;    
    bool                is_free; 
    struct BlockHeader *next;      
    struct BlockHeader *prev;    
} BlockHeader;


typedef enum { CA_LOG_ALLOC, CA_LOG_FREE, CA_LOG_REALLOC } AllocLogKind;

typedef struct {
    AllocLogKind kind;
    void        *ptr;
    size_t       req_size;         
    size_t       act_size;     
    int          seq;               
} AllocLogEntry;


typedef struct {
    size_t total_heap;
    size_t total_allocated;     /* payload bytes in use            */
    size_t total_free;          /* payload bytes that are free     */
    size_t metadata_overhead;   /* bytes spent on headers          */
    int    free_block_count;
    size_t largest_free_block;
    double fragmentation_pct;   
    int    alloc_count;
    int    free_count;
    int    current_live;
} AllocStats;

/* ---- the API ---- */
void   ca_init(void);                             
void  *ca_malloc(size_t size);                    
bool   ca_free(void *ptr);                         
void  *ca_realloc(void *ptr, size_t new_size);    
void  *ca_calloc(size_t count, size_t elem_size);
size_t ca_get_block_size(void *ptr);             
void   ca_get_stats(AllocStats *out);
void   ca_dump_heap(void);       
void   ca_dump_stats(void);      
int    ca_check_leaks(void);     
void   ca_dump_alloc_log(void);   
bool   ca_validate_heap(void);            

#endif 

    unsigned int        magic;    