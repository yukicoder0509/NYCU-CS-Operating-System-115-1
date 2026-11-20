/*
 * HW5: Page Replacement Simulation — LRU and CFLRU
 *
 * Build:  make
 * Run:    ./main <trace_file>
 *
 * Rules from the spec:
 *   - Trace line: "<R|W> <hex byte offset>", 64-bit offset.
 *   - Page size = 4KB  ->  page number = offset >> 12.
 *   - Frames tested: 4096, 8192, 16384, 32768, 65536.
 *   - Hit: page in cache.  Miss: otherwise.
 *   - Fault ratio = miss / total references.
 *   - Write back count = # of DIRTY pages evicted.
 *   - A page created by a W miss is dirty; a W hit makes the page dirty.
 *   - CFLRU: clean-first region = 1/4 of frames, working region = 3/4.
 *       * New / re-referenced pages go to MRU of the working region.
 *       * Working region full -> migrate its LRU page to MRU of clean-first region.
 *       * Victim (from clean-first region): LRU clean page; if none, LRU dirty page.
 *   - NO linear search anywhere (lookup and clean-victim search).
 *   - NO std::map / std::unordered_map.
 *   - Each simulation keeps `unsigned long __internal_bus_cycles = 0;`,
 *     ++ on every page miss. Never print it, never use it for decisions.
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <sys/time.h>

static const int      PAGE_SHIFT = 12;  /* 4KB */
static const uint32_t FRAME_SIZES[] = {4096, 8192, 16384, 32768, 65536};
static const int      NUM_FRAME_SIZES = sizeof(FRAME_SIZES) / sizeof(FRAME_SIZES[0]);

/* ------------------------------------------------------------------ */
/* Trace                                                               */
/* ------------------------------------------------------------------ */
struct Ref {
    uint64_t page;
    bool     write;
};

/* Reads the whole trace into memory. Provided (not the learning goal). */
static std::vector<Ref> load_trace(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) { perror(path); exit(1); }

    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    char *buf = (char *)malloc(len + 1);
    if (!buf || fread(buf, 1, len, fp) != (size_t)len) {
        fprintf(stderr, "failed to read %s\n", path);
        exit(1);
    }
    buf[len] = '\0';
    fclose(fp);

    std::vector<Ref> refs;
    refs.reserve(len / 10);

    const char *p = buf, *end = buf + len;
    while (p < end) {
        while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
        if (p >= end) break;

        bool w = (*p == 'W' || *p == 'w');
        p++;
        while (p < end && *p == ' ') p++;

        uint64_t off = 0;
        for (;; p++) {
            char c = *p;
            if      (c >= '0' && c <= '9') off = (off << 4) | (uint64_t)(c - '0');
            else if (c >= 'a' && c <= 'f') off = (off << 4) | (uint64_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') off = (off << 4) | (uint64_t)(c - 'A' + 10);
            else break;
        }
        refs.push_back({off >> PAGE_SHIFT, w});
    }

    free(buf);
    return refs;
}

/* ------------------------------------------------------------------ */
/* Cache frame node                                                    */
/* ------------------------------------------------------------------ */
const int WORKING_REGION = 0;
const int CLEAN_FIRST_REGION_CLEAN = 1;
const int CLEAN_FIRST_REGION_DIRTY = 2;

struct Node {
    uint64_t page;
    bool     dirty = 0;

    /* LRU-list links (working region / clean-first region / plain LRU) */
    Node *prev;
    Node *next;

    /* Hash-chain link */
    Node *hnext;

    /* TODO(CFLRU): add whatever you need to
     *   - know which region this node is in
     *   - find the LRU clean page in the clean-first region WITHOUT a linear scan
     *     (e.g. a second set of prev/next links for a "clean pages only" list)
     */
    int region;
};

/* ------------------------------------------------------------------ */
/* Intrusive doubly-linked list. Front = MRU, back = LRU.              */
/* ------------------------------------------------------------------ */
class DList {
public:
    DList();

    void   push_front(Node *n);   /* insert at MRU            */
    void   remove(Node *n);       /* unlink n (O(1))          */
    Node  *back() const;          /* LRU node, or nullptr     */
    Node  *pop_back();            /* unlink & return LRU node */
    void   move_to_front(Node *n);
    bool   empty() const { return count_ == 0; }
    size_t size()  const { return count_; }

private:
    /* TODO: choose head/tail pointers or a sentinel node */
    Node *head_, *tail_;
    size_t count_;
};

DList::DList() : count_(0)
{
    /* TODO */
    head_ = tail_ = nullptr;
}

void DList::push_front(Node *n)
{
    /* TODO */
    count_++;
    n->prev = nullptr, n->next = nullptr;
    if(!head_){
        head_ = tail_ = n; 
        return;
    }

    n->prev = nullptr;
    n->next = head_;
    head_->prev = n;
    head_ = n;
}

void DList::remove(Node *n)
{
    // only 1 node
    if(head_ == tail_ && head_ == n) head_ = tail_ = nullptr;

    // is head
    else if(n == head_){
        head_ = n->next;
        if(head_) head_->prev = nullptr;
    }
    // is tail
    else if(n == tail_){
        tail_ = n->prev;
        if(tail_) tail_->next = nullptr;
    }
    // is middle
    else {
        n->prev->next = n->next;
        n->next->prev = n->prev;
    }

    count_--;
}

Node *DList::back() const
{
    /* TODO */
    return tail_;
}

Node *DList::pop_back()
{
    /* TODO */
    Node *n = tail_;
    remove(n);
    return n;
}

void DList::move_to_front(Node *n)
{
    /* TODO */
    remove(n);
    push_front(n);
}

/* ------------------------------------------------------------------ */
/* Hash table: page -> Node*  (your own; no std::unordered_map)        */
/* ------------------------------------------------------------------ */
class HashTable {
public:
    explicit HashTable(size_t capacity);
    ~HashTable();

    Node *find(uint64_t page) const;
    void  insert(Node *n);
    void  erase(uint64_t page);
    void  clear();

private:
    size_t bucket_of(uint64_t page) const;

    /* TODO: bucket array, size/mask, ... */
    Node **buckets_;
    int nbuckets_;
    int log2_nbuckets_;
};

HashTable::HashTable(size_t capacity)
{
    /* TODO: allocate buckets (hint: power of two >= 2*capacity, use a mask) */
    nbuckets_ = capacity*2;
    buckets_ = new Node*[nbuckets_]();
    for(log2_nbuckets_ = 0; (1 << log2_nbuckets_) < nbuckets_; log2_nbuckets_++);
}

HashTable::~HashTable()
{
    /* TODO */
    delete[] buckets_;
}

size_t HashTable::bucket_of(uint64_t page) const
{
    /* TODO: hash function */
    // fibonacci hashing
    return (page * 0x9E3779B97F4A7C15) >> (64 - log2_nbuckets_);
}

Node *HashTable::find(uint64_t page) const
{
    /* TODO */
    Node* n = buckets_[bucket_of(page)];
    while(n && n->page != page) n = n->hnext;
    return n;
}

void HashTable::insert(Node *n)
{
    /* TODO */
    Node* bucket_head = buckets_[bucket_of(n->page)];
    n->hnext = bucket_head;
    buckets_[bucket_of(n->page)] = n;
}

void HashTable::erase(uint64_t page)
{
    /* TODO */
    Node* prev = buckets_[bucket_of(page)];
    if(!prev) return;

    if(prev->page == page) {
        buckets_[bucket_of(page)] = prev->hnext;
        return;
    }
    Node* n = prev->hnext;
    while(n && n->page != page) { prev = n; n = n->hnext; }
    if(!n) return;
    prev->hnext = n->hnext;
}

void HashTable::clear()
{
    /* TODO */
    for(int i = 0; i < nbuckets_; i++) buckets_[i] = nullptr;
}

/* ------------------------------------------------------------------ */
/* Stats                                                               */
/* ------------------------------------------------------------------ */
struct Stats {
    uint64_t hit        = 0;
    uint64_t miss       = 0;
    uint64_t write_back = 0;
};

/* ------------------------------------------------------------------ */
/* LRU                                                                 */
/* ------------------------------------------------------------------ */
class LRUCache {
public:
    explicit LRUCache(uint32_t frames);
    ~LRUCache();

    void         access(uint64_t page, bool write);
    const Stats &stats() const { return stats_; }

private:
    Node *alloc_node();           /* take a free frame from the pool   */
    Node *evict();                /* evict LRU page, count write-back  */

    uint32_t  frames_;
    Node     *pool_;              /* preallocated `frames_` nodes      */
    uint32_t  used_;              /* # nodes handed out from pool_     */
    DList     list_;
    HashTable table_;
    Stats     stats_;

    unsigned long __internal_bus_cycles = 0;   /* ++ on every miss; never print */
};

LRUCache::LRUCache(uint32_t frames)
    : frames_(frames), pool_(nullptr), used_(0), table_(frames)
{
    /* TODO: allocate pool_ */
    pool_ = new Node[frames]();
}

LRUCache::~LRUCache()
{
    /* TODO: free pool_ */
    delete[] pool_;
}

Node *LRUCache::alloc_node()
{
    /* TODO: hand out a fresh node, or recycle the evicted one */
    if(used_ < frames_) return &pool_[used_++];
    else{
        return evict();
    }
}

Node* LRUCache::evict()
{
    /* TODO */
    Node *victim = list_.pop_back();
    if(victim->dirty) stats_.write_back++;

    table_.erase(victim->page);
    return victim;
}

void LRUCache::access(uint64_t page, bool write)
{
    /* TODO:
     *   hit  -> stats_.hit++, mark dirty if write, move to MRU
     *   miss -> stats_.miss++, __internal_bus_cycles++,
     *           evict if full, insert new node at MRU (dirty = write)
     */

    Node *n = table_.find(page);
    if(n){
        stats_.hit++;
        if(write) n->dirty = true;
        list_.move_to_front(n);
        return;
    }
    else{
        stats_.miss++;
        __internal_bus_cycles++;
        Node *n = alloc_node();
        n->page = page;
        n->dirty = write;
        list_.push_front(n);
        table_.insert(n);
        return;
    }
}

/* ------------------------------------------------------------------ */
/* CFLRU                                                               */
/* ------------------------------------------------------------------ */
class CFLRUCache {
public:
    explicit CFLRUCache(uint32_t frames);
    ~CFLRUCache();

    void         access(uint64_t page, bool write);
    const Stats &stats() const { return stats_; }

private:
    Node *alloc_node();
    void  migrate();              /* working LRU -> clean-first MRU    */
    Node *select_victim();        /* LRU clean in CF region, else LRU  */
    void  evict();

    uint32_t  frames_;
    uint32_t  working_cap_;       /* frames_ * 3 / 4                   */
    uint32_t  clean_first_cap_;   /* frames_ / 4                       */

    Node     *pool_;
    uint32_t  used_;
    HashTable table_;
    Stats     stats_;

    /* TODO: lists for the working region, the clean-first region,
     *       and whatever lets you find the LRU clean page in O(1). */

    unsigned long __internal_bus_cycles = 0;   /* ++ on every miss; never print */
};

CFLRUCache::CFLRUCache(uint32_t frames)
    : frames_(frames),
      working_cap_(frames - frames / 4),
      clean_first_cap_(frames / 4),
      pool_(nullptr), used_(0), table_(frames)
{
    /* TODO: allocate pool_ */
}

CFLRUCache::~CFLRUCache()
{
    /* TODO */
}

Node *CFLRUCache::alloc_node()
{
    /* TODO */
    return nullptr;
}

void CFLRUCache::migrate()
{
    /* TODO */
}

Node *CFLRUCache::select_victim()
{
    /* TODO: no linear search! */
    return nullptr;
}

void CFLRUCache::evict()
{
    /* TODO */
}

void CFLRUCache::access(uint64_t page, bool write)
{
    /* TODO:
     *   hit  (either region) -> hit++, dirty |= write, move to working MRU
     *                           (keep both regions within capacity)
     *   miss -> miss++, __internal_bus_cycles++, evict if full,
     *           insert at working MRU, migrate if working region overflows
     *
     *   Think about: what happens to the clean-page index when a page
     *   changes dirty state or changes region?
     */
    (void)page; (void)write;
}

/* ------------------------------------------------------------------ */
/* Driver                                                              */
/* ------------------------------------------------------------------ */
static double now_sec()
{
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

template <typename Cache>
static Stats simulate(const std::vector<Ref> &refs, uint32_t frames)
{
    Cache cache(frames);
    /* TODO: feed every reference into the cache */
    for(auto &r : refs) cache.access(r.page, r.write);
    return cache.stats();
}

template <typename Cache>
static void run_policy(const char *name, const std::vector<Ref> &refs)
{
    printf("%s policy:\n", name);
    printf("Frame\tHit\t\tMiss\t\tPage fault ratio\tWrite back count\n");

    double t0 = now_sec();
    for (int i = 0; i < NUM_FRAME_SIZES; i++) {
        Stats s = simulate<Cache>(refs, FRAME_SIZES[i]);
        double ratio = refs.empty() ? 0.0 : (double)s.miss / (double)refs.size();
        printf("%u\t%lu\t%lu\t\t%.10f\t\t%lu\n",
               FRAME_SIZES[i],
               (unsigned long)s.hit, (unsigned long)s.miss,
               ratio, (unsigned long)s.write_back);
    }
    double t1 = now_sec();
    printf("Total elapsed time %.6f sec\n\n", t1 - t0);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <trace_file>\n", argv[0]);
        return 1;
    }

    std::vector<Ref> refs = load_trace(argv[1]);

    run_policy<LRUCache>("LRU", refs);
    run_policy<CFLRUCache>("CFLRU", refs);
    return 0;
}
