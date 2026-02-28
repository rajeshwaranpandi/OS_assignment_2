#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

/*
 * Benchmark for Assignment-2: Prefaulting and content-aware mapping.
 *
 * Intended high-level flow:
 *   buff1 = alloc()
 *   buff2 = alloc()
 *   Syscall1(buff1, buff2, ...)     // buff1 faults should also establish buff2 mappings
 *   compute(buff1)                  // touch random subset of pages in buff1
 *   Syscall2(buff1, buff2, ...)     // (extension) deduplicate identical pages
 *   compute(buff2)                  // touch pages in buff2 (phase1 pages + some new pages)
 */

enum { NPAGES = 256 };


// Names of the system calls are representative
#ifndef SYS_vm_prefault_map1
#define SYS_vm_prefault_map1 451 
#endif

#ifndef SYS_vm_prefault_map2
#define SYS_vm_prefault_map2 452
#endif

enum {
    OP_ARM   = 1,
    OP_DEDUP = 2,
};


static long syscall1_enable_prefault(void *buff1, void *buff2, size_t len) {
    return syscall(SYS_vm_prefault_map1, buff1, buff2, len, OP_ARM);
}

static long syscall2_dedup(void *buff1, void *buff2, size_t len) {
    return syscall(SYS_vm_prefault_map2, buff1, buff2, len, OP_DEDUP);
}

typedef struct {
    int a;
    int b;
} Pair;

typedef struct {
    unsigned seed;
    long pg;
    size_t len;

    int n_phase1;
    int phase1_pages[NPAGES];

    int n_phase2_new;
    int phase2_new_pages[NPAGES];

    int n_dup_pairs;
    Pair dup_pairs[NPAGES / 2];
} Plan;

typedef struct {
    uint64_t phase2_fault_delta;
    int phase1_mismatches;
    int newpage_nonzero;
    int cow_ok;
} Result;

/* ----------------- small helpers ----------------- */

static uint64_t faults_now(void) {
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) != 0) {
        perror("getrusage");
        exit(1);
    }
    return (uint64_t)ru.ru_minflt + (uint64_t)ru.ru_majflt;
}

static void *alloc_buffer(size_t len) {
    void *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }
    return p;
}

static void free_buffer(void *p, size_t len) {
    munmap(p, len);
}

static void fill_page(uint8_t *base, size_t pg, int page_idx, uint8_t byte) {
    memset(base + (size_t)page_idx * pg, byte, pg);
}

static uint64_t hash_page(const uint8_t *p, size_t pg) {
    /* cheap per-page hash: FNV-1a */
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < pg; i++) {
        h ^= (uint64_t)p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static int is_all_zero(const uint8_t *p, size_t pg) {
    for (size_t i = 0; i < pg; i++) {
        if (p[i] != 0) return 0;
    }
    return 1;
}

static void shuffle_ints(int *a, int n) {
    for (int i = n - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        int tmp = a[i];
        a[i] = a[j];
        a[j] = tmp;
    }
}

static int rand_range(int lo, int hi) {
    /* inclusive */
    return lo + (rand() % (hi - lo + 1));
}

/* ----------------- plan generation ----------------- */

static unsigned choose_seed(void) {
    /* Optional reproducibility: SEED=123 ./bench */
    const char *s = getenv("SEED");
    if (s && *s) {
        char *end = NULL;
        unsigned long v = strtoul(s, &end, 10);
        if (end && *end == '\0') return (unsigned)v;
    }
    return (unsigned)time(NULL) ^ (unsigned)getpid();
}

static void initialize(Plan *p) {
    memset(p, 0, sizeof(*p));

    p->seed = choose_seed();
    srand(p->seed);

    p->pg = sysconf(_SC_PAGESIZE);
    if (p->pg <= 0) {
        fprintf(stderr, "Failed to get page size\n");
        exit(1);
    }
    p->len = (size_t)NPAGES * (size_t)p->pg;

    /* Phase-1: choose random number of pages in [1..256], then choose those pages randomly */
    p->n_phase1 = rand_range(1, NPAGES);

    int pool[NPAGES];
    for (int i = 0; i < NPAGES; i++) pool[i] = i;
    shuffle_ints(pool, NPAGES);

    for (int i = 0; i < p->n_phase1; i++) p->phase1_pages[i] = pool[i];

    /* Phase-2 new pages: choose from pages NOT in phase1 */
    int remaining = NPAGES - p->n_phase1;
    if (remaining == 0) {
        p->n_phase2_new = 0;
    } else {
        p->n_phase2_new = rand_range(1, remaining);

        int start = p->n_phase1;
        for (int i = 0; i < p->n_phase2_new; i++) {
            p->phase2_new_pages[i] = pool[start + i];
        }
    }

    /* Extension: choose random duplicate pairs within phase1 pages.
       Keep at least 1 pair when possible so the extension is exercised. */
    int max_pairs = p->n_phase1 / 2;
    if (max_pairs == 0) {
        p->n_dup_pairs = 0;
        return;
    }

    int cap = max_pairs;
    if (cap > 8) cap = 8; /* small, readable stress */
    p->n_dup_pairs = rand_range(1, cap);

    /* pick 2*npairs pages from a shuffled copy of phase1 list */
    int tmp[NPAGES];
    for (int i = 0; i < p->n_phase1; i++) tmp[i] = p->phase1_pages[i];
    shuffle_ints(tmp, p->n_phase1);

    for (int i = 0; i < p->n_dup_pairs; i++) {
        p->dup_pairs[i].a = tmp[2 * i];
        p->dup_pairs[i].b = tmp[2 * i + 1];
    }
}


static void compute_phase1(uint8_t *buff1, const Plan *p) {
    /* Write to chosen pages (forces buff1 faults). */
    for (int i = 0; i < p->n_phase1; i++) {
        int page = p->phase1_pages[i];
        uint8_t val = (uint8_t)rand_range(1, 200);
        fill_page(buff1, (size_t)p->pg, page, val);
    }

    /* Overwrite duplicate pairs with identical content. */
    for (int i = 0; i < p->n_dup_pairs; i++) {
        uint8_t val = (uint8_t)rand_range(201, 255);
        fill_page(buff1, (size_t)p->pg, p->dup_pairs[i].a, val);
        fill_page(buff1, (size_t)p->pg, p->dup_pairs[i].b, val);
    }
}

static void compute_phase2(uint8_t *buff2, const Plan *p) {
    /* Read from phase1 pages, then from new pages. */
    volatile uint64_t sink = 0;

    for (int i = 0; i < p->n_phase1; i++) {
        int page = p->phase1_pages[i];
        uint8_t *ptr = buff2 + (size_t)page * (size_t)p->pg;
        sink ^= ptr[0];
    }

    for (int i = 0; i < p->n_phase2_new; i++) {
        int page = p->phase2_new_pages[i];
        uint8_t *ptr = buff2 + (size_t)page * (size_t)p->pg;
        sink ^= ptr[0];
    }

    printf("%llu\n", (unsigned long long)sink);
}


static int count_phase1_mismatches(uint8_t *buff1, uint8_t *buff2, const Plan *p) {
    int mism = 0;
    for (int i = 0; i < p->n_phase1; i++) {
        int page = p->phase1_pages[i];
        const uint8_t *b1 = buff1 + (size_t)page * (size_t)p->pg;
        const uint8_t *b2 = buff2 + (size_t)page * (size_t)p->pg;
        if (hash_page(b1, (size_t)p->pg) != hash_page(b2, (size_t)p->pg)) mism++;
    }
    return mism;
}

static int count_new_pages_nonzero(uint8_t *buff2, const Plan *p) {
    int bad = 0;
    for (int i = 0; i < p->n_phase2_new; i++) {
        int page = p->phase2_new_pages[i];
        const uint8_t *b2 = buff2 + (size_t)page * (size_t)p->pg;
        if (!is_all_zero(b2, (size_t)p->pg)) bad++;
    }
    return bad;
}

static int cow_sanity(uint8_t *buff, const Plan *p) {
    for (int i = 0; i < p->n_dup_pairs; i++) {
        int a_page = p->dup_pairs[i].a;
        int b_page = p->dup_pairs[i].b;

        uint8_t *a = buff + (size_t)a_page * (size_t)p->pg;
        uint8_t *b = buff + (size_t)b_page * (size_t)p->pg;

        uint8_t orig = b[0];
        a[0] ^= 0xFF;          // write to a
        if (b[0] != orig) {    // b must NOT change
            return 0;          // FAIL: aliasing / missing COW
        }
    }
    return 1;                  // PASS
}

static Result validation(uint8_t *buff1, uint8_t *buff2, const Plan *p) {
    Result r;
    memset(&r, 0, sizeof(r));
    uint64_t f0 = faults_now();
    compute_phase2(buff2, p);
    uint64_t f1 = faults_now();
    r.phase2_fault_delta = f1 - f0;

    r.phase1_mismatches = count_phase1_mismatches(buff1, buff2, p);
    r.newpage_nonzero = count_new_pages_nonzero(buff2, p);
    r.cow_ok = cow_sanity(buff2, p) && cow_sanity(buff1, p);
    return r;
}
static Result run(uint8_t *buff1, uint8_t *buff2, const Plan *p) {


    /* Syscall1: arm kernel before phase1 */
    errno = 0;
    long rc = syscall1_enable_prefault(buff1, buff2, p->len);
    printf("rc=%ld errno=%d (%s)\n", rc, errno, strerror(errno));

    compute_phase1(buff1, p);

    /* Syscall2: dedup after phase1 */
    errno = 0;
    rc = syscall2_dedup(buff1, buff2, p->len);
    printf("rc=%ld errno=%d (%s)\n", rc, errno, strerror(errno));

    Result r = validation(buff1, buff2, p);
    return r;
}

int main(void) {
    Plan plan;
    initialize(&plan);

    uint8_t *buff1 = (uint8_t *)alloc_buffer(plan.len);
    uint8_t *buff2 = (uint8_t *)alloc_buffer(plan.len);

    Result r = run(buff1, buff2, &plan);

    int ok = (r.phase1_mismatches == 0) && (r.newpage_nonzero == 0) && (r.cow_ok != 0);

    printf("seed=%u  phase1=%d  phase2_new=%d  dup_pairs=%d  phase2_faults=%" PRIu64
           "  mism=%d  new_nonzero=%d  cow=%s\n",
           plan.seed, plan.n_phase1, plan.n_phase2_new, plan.n_dup_pairs,
           r.phase2_fault_delta, r.phase1_mismatches, r.newpage_nonzero,
           r.cow_ok ? "OK" : "FAIL");

    printf("RESULT: %s\n", ok ? "PASS" : "FAIL");

    free_buffer(buff2, plan.len);
    free_buffer(buff1, plan.len);
    return 0;
}
