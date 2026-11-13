#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define POOL_SIZE   20000
#define HEADER_SIZE 32
#define ALIGN       32
#define NUM_LEVELS  11

typedef struct chunk {
    size_t        size;       /* data size, excluding header            */
    struct chunk *prev;       /* previous chunk in memory (chunk list)  */
    struct chunk *next_free;  /* next chunk in this level's free list   */
    uint32_t      is_free;
    uint32_t      _pad;       /* pad to 32 bytes                         */
} chunk_t;

_Static_assert(sizeof(chunk_t) == HEADER_SIZE, "header must be exactly 32 bytes");

static void    *pool = NULL;                  
static chunk_t *free_lists[NUM_LEVELS];       

static inline size_t round_up(size_t n) {
    return (n + ALIGN - 1) & ~(size_t)(ALIGN - 1);
}

static inline void *chunk_to_ptr(chunk_t *c) {
    return (char *)c + HEADER_SIZE;
}

static inline chunk_t *ptr_to_chunk(void *p) {
    return (chunk_t *)((char *)p - HEADER_SIZE);
}

/* Next chunk in memory, or NULL if c is the last one in the pool. */
static inline chunk_t *next_chunk(chunk_t *c) {
    char *n = (char *)c + HEADER_SIZE + c->size;
    return (n < (char *)pool + POOL_SIZE) ? (chunk_t *)n : NULL;
}

static void print_str(const char *s) {
    write(STDOUT_FILENO, s, strlen(s));
}

/* Map a chunk size to its level: [0,32)->0, [32,64)->1, ..., clamp to 10. */
static int level_of(size_t size) {
    
    int level = 0;
    size_t chunk_size = 32;

    for(; level < NUM_LEVELS; level++, chunk_size *= 2)
        if(size < chunk_size) return level;
    return NUM_LEVELS - 1;
}

/* Append c to the END of its level's free list. */
static void freelist_append(chunk_t *c) {
    
    int level = level_of(c->size);
    c->next_free = NULL;
    if(!free_lists[level]) free_lists[level] = c;
    else {
        chunk_t *cur = free_lists[level];
        while(cur->next_free) cur = cur->next_free;
        cur->next_free = c;
    }
}

/* Remove c from its level's free list. */
static void freelist_remove(chunk_t *c) {
    
    int level = level_of(c->size);
    if(free_lists[level] == c) free_lists[level] = c->next_free;
    else {
        chunk_t *cur = free_lists[level];
        while(cur && cur->next_free != c) cur = cur->next_free;
        if(cur) cur->next_free = c->next_free;
    }
}

/*
 * Search levels starting at level_of(size). Within a level, pick the FIRST
 * chunk with the smallest c->size >= size. Return NULL if nothing fits.
 */
static chunk_t *find_best_fit(size_t size) {
    
    int level = level_of(size);
    chunk_t *cur, *best = NULL;
    while(level < NUM_LEVELS){
        for(cur = free_lists[level]; cur; cur = cur->next_free){
            if(cur->size >= size && (!best || cur->size < best->size)) best = cur;
        }
        if(best) return best;
        level++;
    }
    return best;
}

/*
 * c is a free chunk already removed from the free lists, c->size >= size.
 * If the leftover is big enough to hold a header + data, carve a new free
 * chunk after it, fix up prev links, and put the leftover in the free lists.
 */
static void split_chunk(chunk_t *c, size_t size) {
    
    size_t leftover = c->size - size;
    if(leftover > HEADER_SIZE) {
        chunk_t *after = next_chunk(c);   /* chunk after c, before resizing */
        chunk_t *newc = (chunk_t *)((char *)c + size + HEADER_SIZE);
        newc->size = leftover - HEADER_SIZE;
        newc->prev = c;
        newc->is_free = 1;
        if(after) after->prev = newc;
        c->size = size;
        freelist_append(newc);
    }
}

/*
 * c was just freed. Merge with the next and/or previous chunk in memory if
 * they are free (remove them from the free lists first). Return the
 * resulting chunk (may be c->prev if merged backwards).
 */
static chunk_t *coalesce(chunk_t *c) {
    
    if(c->prev && c->prev->is_free) {
        freelist_remove(c->prev);
        c->prev->size += c->size + HEADER_SIZE;
        c = c->prev;
    }
    chunk_t *nxt = next_chunk(c);         /* NULL at end of pool */
    if(nxt && nxt->is_free) {
        freelist_remove(nxt);
        c->size += nxt->size + HEADER_SIZE;
    }
    nxt = next_chunk(c);
    if(nxt) nxt->prev = c;
    return c;
}

/* Largest free data size (excluding header) across the whole pool. */
static size_t max_free_chunk(void) {
    
    size_t largest = 0;
    for(int i = 0; i < NUM_LEVELS; i++){
        for(chunk_t *cur = free_lists[i]; cur; cur = cur->next_free)
            if(cur->size > largest) largest = cur->size;
    }
    return largest;
}

static int is_allocated_chunk(chunk_t *c) {
    char *p = (char *)c;
    if (p < (char *)pool || p >= (char *)pool + POOL_SIZE)
        return 0;
    if ((uintptr_t)(p - (char *)pool) % ALIGN != 0)
        return 0;
    for (chunk_t *cur = (chunk_t *)pool; cur; cur = next_chunk(cur))
        if (cur == c)
            return !cur->is_free;
    return 0;
}

static int pool_init(void) {
    pool = mmap(NULL, POOL_SIZE, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (pool == MAP_FAILED) {
        pool = NULL;
        return -1;
    }
    memset(free_lists, 0, sizeof(free_lists));

    chunk_t *first   = (chunk_t *)pool;
    first->size      = POOL_SIZE - HEADER_SIZE;
    first->prev      = NULL;
    first->next_free = NULL;
    first->is_free   = 1;
    freelist_append(first);
    return 0;
}

static void pool_destroy(void) {
    char buf[64];
    snprintf(buf, sizeof(buf), "Max Free Chunk Size = %zu\n", max_free_chunk());
    print_str(buf);
    munmap(pool, POOL_SIZE);
    pool = NULL;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */
void *malloc(size_t size) {
    if (!pool && pool_init() != 0)
        return NULL;

    if (size == 0) {            /* end-of-test signal */
        pool_destroy();
        return NULL;
    }

    if (size > POOL_SIZE - HEADER_SIZE)
        return NULL;

    size = round_up(size);

    chunk_t *c = find_best_fit(size);
    if (!c)
        return NULL;

    freelist_remove(c);
    split_chunk(c, size);
    c->is_free = 0;
    return chunk_to_ptr(c);
}

void free(void *ptr) {
    if (!ptr || !pool)
        return;

    chunk_t *c = ptr_to_chunk(ptr);
    if (!is_allocated_chunk(c))
        return;
    c->is_free = 1;
    c = coalesce(c);
    freelist_append(c);
}
