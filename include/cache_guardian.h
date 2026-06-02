#ifndef CACHE_GUARDIAN_H
#define CACHE_GUARDIAN_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Budget ──────────────────────────────────────────────────────── */

typedef struct {
    uint64_t disk_bytes;
    uint64_t bandwidth_bytes_per_day;
    uint64_t time_seconds_per_run;
} Budget;

/* ── Cache Entry ─────────────────────────────────────────────────── */

#define CG_PATH_MAX 512
#define CG_HASH_BINS 64

typedef struct {
    char     path[CG_PATH_MAX];
    uint64_t size;
    uint64_t access_count;
    time_t   last_access;
    uint32_t dependency_hash;           /* fingerprint of deps for this entry */
    uint32_t dep_profile[CG_HASH_BINS]; /* histogram for KL divergence        */
} CacheEntry;

/* ── Budget Tracker ──────────────────────────────────────────────── */

typedef struct {
    Budget    limit;
    uint64_t  disk_used;
    uint64_t  bandwidth_used_today;
    uint64_t  time_used_run;
    time_t    day_start;                /* start of current bandwidth window  */
} BudgetTracker;

void     budget_tracker_init(BudgetTracker *bt, Budget limit);
bool     budget_check_disk(const BudgetTracker *bt, uint64_t extra);
bool     budget_check_bandwidth(const BudgetTracker *bt, uint64_t extra);
bool     budget_check_time(const BudgetTracker *bt, uint64_t extra_seconds);
bool     budget_enforce(BudgetTracker *bt, uint64_t pending_bytes, uint64_t pending_seconds);
void     budget_record_disk(BudgetTracker *bt, uint64_t bytes);
void     budget_record_bandwidth(BudgetTracker *bt, uint64_t bytes);
void     budget_record_time(BudgetTracker *bt, uint64_t seconds);
void     budget_reset_day(BudgetTracker *bt);
void     budget_reset_run(BudgetTracker *bt);

/* ── Dependency Profile & KL Divergence ──────────────────────────── */

typedef struct {
    uint32_t bins[CG_HASH_BINS];
    uint64_t total;
} DependencyProfile;

void     dep_profile_init(DependencyProfile *p);
void     dep_profile_add_hash(DependencyProfile *p, uint32_t hash);
void     dep_profile_normalize(const DependencyProfile *p, double out[CG_HASH_BINS]);
double   kl_divergence(const double p[CG_HASH_BINS], const double q[CG_HASH_BINS]);
double   symmetrized_kl(const double p[CG_HASH_BINS], const double q[CG_HASH_BINS]);

/* ── Eviction Ranking ────────────────────────────────────────────── */

typedef struct {
    size_t   index;
    double   divergence;
} EvictionCandidate;

/* Returns malloc'd array sorted by divergence descending (evict first).
 * Caller must free. Sets *out_count. */
EvictionCandidate *eviction_rank(const CacheEntry *entries, size_t count,
                                 const DependencyProfile *reference,
                                 size_t *out_count);

/* Evict entries until budget is satisfied. Returns number evicted.
 * evicted_indices (optional, caller-freed) lists evicted entry indices. */
size_t  eviction_run(CacheEntry *entries, size_t count,
                     BudgetTracker *bt,
                     const DependencyProfile *reference,
                     size_t **evicted_indices);

/* ── Phase Detection ─────────────────────────────────────────────── */

typedef enum {
    Phase_Stable        = 0,
    Phase_PreTransition = 1,
    Phase_Transitioning = 2,
    Phase_PostTransition= 3
} Phase;

typedef struct {
    Phase    current_phase;
    double   stability_score;   /* 0..1, 1 = very stable */
    double   divergence_rate;   /* recent change rate     */
    int      transition_count;  /* transitions in window  */
} PhaseState;

void     phase_state_init(PhaseState *ps);
Phase    phase_detect(PhaseState *ps, double recent_divergence);
const char *phase_name(Phase p);

/* ── CI Overlap Detection ────────────────────────────────────────── */

typedef struct {
    char     build_id[CG_PATH_MAX];
    uint32_t dep_hashes[256];
    size_t   dep_count;
} CIBuild;

typedef struct {
    char     shared_path[CG_PATH_MAX];
    size_t   build_a_index;
    size_t   build_b_index;
} CIOverlap;

/* Returns malloc'd array of overlaps. Caller frees. */
CIOverlap *ci_detect_overlaps(const CIBuild *builds, size_t build_count,
                              const CacheEntry *entries, size_t entry_count,
                              size_t *out_overlap_count);

/* ── Cache (owning collection) ───────────────────────────────────── */

#define CG_CACHE_MAX 4096

typedef struct {
    CacheEntry       entries[CG_CACHE_MAX];
    size_t           count;
    BudgetTracker    budget;
    DependencyProfile global_profile;
    PhaseState       phase;
} CacheGuardian;

void     cg_init(CacheGuardian *cg, Budget budget);
int      cg_add_entry(CacheGuardian *cg, const char *path, uint64_t size,
                      uint32_t dep_hash);
void     cg_access_entry(CacheGuardian *cg, size_t index);
size_t   cg_evict_to_budget(CacheGuardian *cg);
Phase    cg_update_phase(CacheGuardian *cg, double recent_divergence);
size_t   cg_ci_optimize(CacheGuardian *cg, const CIBuild *builds, size_t build_count);

#ifdef __cplusplus
}
#endif

#endif /* CACHE_GUARDIAN_H */
