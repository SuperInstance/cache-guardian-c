#include "cache_guardian.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>

/* ── Budget Tracker ──────────────────────────────────────────────── */

void budget_tracker_init(BudgetTracker *bt, Budget limit) {
    memset(bt, 0, sizeof(*bt));
    bt->limit = limit;
    bt->day_start = time(NULL);
}

bool budget_check_disk(const BudgetTracker *bt, uint64_t extra) {
    if (bt->limit.disk_bytes == 0) return true; /* unlimited */
    return (bt->disk_used + extra) <= bt->limit.disk_bytes;
}

bool budget_check_bandwidth(const BudgetTracker *bt, uint64_t extra) {
    if (bt->limit.bandwidth_bytes_per_day == 0) return true;
    return (bt->bandwidth_used_today + extra) <= bt->limit.bandwidth_bytes_per_day;
}

bool budget_check_time(const BudgetTracker *bt, uint64_t extra_seconds) {
    if (bt->limit.time_seconds_per_run == 0) return true;
    return (bt->time_used_run + extra_seconds) <= bt->limit.time_seconds_per_run;
}

bool budget_enforce(BudgetTracker *bt, uint64_t pending_bytes, uint64_t pending_seconds) {
    time_t now = time(NULL);
    /* Reset bandwidth day if >24h elapsed */
    if ((uint64_t)(now - bt->day_start) > 86400) {
        bt->bandwidth_used_today = 0;
        bt->day_start = now;
    }
    return budget_check_disk(bt, pending_bytes)
        && budget_check_bandwidth(bt, pending_bytes)
        && budget_check_time(bt, pending_seconds);
}

void budget_record_disk(BudgetTracker *bt, uint64_t bytes) {
    bt->disk_used += bytes;
}

void budget_record_bandwidth(BudgetTracker *bt, uint64_t bytes) {
    bt->bandwidth_used_today += bytes;
}

void budget_record_time(BudgetTracker *bt, uint64_t seconds) {
    bt->time_used_run += seconds;
}

void budget_reset_day(BudgetTracker *bt) {
    bt->bandwidth_used_today = 0;
    bt->day_start = time(NULL);
}

void budget_reset_run(BudgetTracker *bt) {
    bt->time_used_run = 0;
}

/* ── Dependency Profile & KL Divergence ──────────────────────────── */

void dep_profile_init(DependencyProfile *p) {
    memset(p, 0, sizeof(*p));
}

void dep_profile_add_hash(DependencyProfile *p, uint32_t hash) {
    size_t bin = hash % CG_HASH_BINS;
    p->bins[bin]++;
    p->total++;
}

void dep_profile_normalize(const DependencyProfile *p, double out[CG_HASH_BINS]) {
    if (p->total == 0) {
        for (size_t i = 0; i < CG_HASH_BINS; i++)
            out[i] = 1.0 / CG_HASH_BINS;
        return;
    }
    for (size_t i = 0; i < CG_HASH_BINS; i++)
        out[i] = (double)p->bins[i] / (double)p->total;
}

double kl_divergence(const double p[CG_HASH_BINS], const double q[CG_HASH_BINS]) {
    double kl = 0.0;
    double eps = 1e-10;
    for (size_t i = 0; i < CG_HASH_BINS; i++) {
        double pi = p[i] + eps;
        double qi = q[i] + eps;
        kl += pi * log(pi / qi);
    }
    return kl;
}

double symmetrized_kl(const double p[CG_HASH_BINS], const double q[CG_HASH_BINS]) {
    return 0.5 * (kl_divergence(p, q) + kl_divergence(q, p));
}

/* ── Eviction Ranking ────────────────────────────────────────────── */

static int cmp_eviction_desc(const void *a, const void *b) {
    double da = ((const EvictionCandidate *)a)->divergence;
    double db = ((const EvictionCandidate *)b)->divergence;
    if (da > db) return -1;
    if (da < db)  return 1;
    return 0;
}

EvictionCandidate *eviction_rank(const CacheEntry *entries, size_t count,
                                 const DependencyProfile *reference,
                                 size_t *out_count) {
    *out_count = count;
    if (count == 0) return NULL;

    double ref_norm[CG_HASH_BINS];
    dep_profile_normalize(reference, ref_norm);

    EvictionCandidate *cands = malloc(count * sizeof(EvictionCandidate));
    if (!cands) return NULL;

    for (size_t i = 0; i < count; i++) {
        DependencyProfile ep;
        dep_profile_init(&ep);
        dep_profile_add_hash(&ep, entries[i].dependency_hash);
        /* Also fold in the stored dep_profile bins */
        for (size_t b = 0; b < CG_HASH_BINS; b++) {
            for (uint32_t k = 0; k < entries[i].dep_profile[b]; k++) {
                dep_profile_add_hash(&ep, (uint32_t)b);
            }
        }

        double e_norm[CG_HASH_BINS];
        dep_profile_normalize(&ep, e_norm);
        cands[i].index = i;
        cands[i].divergence = symmetrized_kl(ref_norm, e_norm);
    }

    qsort(cands, count, sizeof(EvictionCandidate), cmp_eviction_desc);
    return cands;
}

size_t eviction_run(CacheEntry *entries, size_t count,
                    BudgetTracker *bt,
                    const DependencyProfile *reference,
                    size_t **evicted_indices) {
    size_t evicted = 0;
    size_t cap = 64;
    size_t *evicted_idx = malloc(cap * sizeof(size_t));
    if (!evicted_idx) return 0;

    while (!budget_enforce(bt, 0, 0) && count > 0) {
        size_t rank_count = 0;
        EvictionCandidate *cands = eviction_rank(entries, count, reference, &rank_count);
        if (!cands || rank_count == 0) { free(cands); break; }

        /* Evict the most divergent entry (index 0 after sort) */
        size_t victim = cands[0].index;
        uint64_t freed = entries[victim].size;

        /* Swap-remove */
        if (evicted >= cap) {
            cap *= 2;
            size_t *tmp = realloc(evicted_idx, cap * sizeof(size_t));
            if (!tmp) { free(cands); break; }
            evicted_idx = tmp;
        }
        evicted_idx[evicted++] = victim;

        if (victim < count - 1) {
            memmove(&entries[victim], &entries[victim + 1],
                    (count - victim - 1) * sizeof(CacheEntry));
        }
        count--;

        /* Reduce tracked disk usage */
        if (bt->disk_used >= freed) bt->disk_used -= freed;
        else bt->disk_used = 0;

        free(cands);
    }

    if (evicted_indices) *evicted_indices = evicted_idx;
    else free(evicted_idx);

    return evicted;
}

/* ── Phase Detection ─────────────────────────────────────────────── */

void phase_state_init(PhaseState *ps) {
    memset(ps, 0, sizeof(*ps));
    ps->current_phase = Phase_Stable;
    ps->stability_score = 1.0;
}

Phase phase_detect(PhaseState *ps, double recent_divergence) {
    /* Update divergence rate with exponential moving average */
    double alpha = 0.3;
    ps->divergence_rate = alpha * recent_divergence + (1.0 - alpha) * ps->divergence_rate;

    /* Update stability score: low divergence → stable */
    if (ps->divergence_rate < 0.1)
        ps->stability_score = fmin(1.0, ps->stability_score + 0.1);
    else
        ps->stability_score = fmax(0.0, ps->stability_score - 0.1);

    Phase prev = ps->current_phase;

    if (ps->stability_score > 0.7) {
        ps->current_phase = Phase_Stable;
    } else if (ps->stability_score > 0.4) {
        ps->current_phase = Phase_PreTransition;
    } else if (ps->stability_score > 0.15) {
        ps->current_phase = Phase_Transitioning;
    } else {
        ps->current_phase = Phase_PostTransition;
    }

    if (ps->current_phase != prev)
        ps->transition_count++;

    return ps->current_phase;
}

const char *phase_name(Phase p) {
    switch (p) {
        case Phase_Stable:         return "Stable";
        case Phase_PreTransition:  return "PreTransition";
        case Phase_Transitioning:  return "Transitioning";
        case Phase_PostTransition: return "PostTransition";
    }
    return "Unknown";
}

/* ── CI Overlap Detection ────────────────────────────────────────── */

CIOverlap *ci_detect_overlaps(const CIBuild *builds, size_t build_count,
                              const CacheEntry *entries, size_t entry_count,
                              size_t *out_overlap_count) {
    size_t cap = 64;
    size_t count = 0;
    CIOverlap *overlaps = malloc(cap * sizeof(CIOverlap));
    if (!overlaps) { *out_overlap_count = 0; return NULL; }

    for (size_t a = 0; a < build_count; a++) {
        for (size_t b = a + 1; b < build_count; b++) {
            for (size_t da = 0; da < builds[a].dep_count; da++) {
                for (size_t db = 0; db < builds[b].dep_count; db++) {
                    if (builds[a].dep_hashes[da] == builds[b].dep_hashes[db]) {
                        /* Find matching cache entry */
                        for (size_t e = 0; e < entry_count; e++) {
                            if (entries[e].dependency_hash == builds[a].dep_hashes[da]) {
                                if (count >= cap) {
                                    cap *= 2;
                                    CIOverlap *tmp = realloc(overlaps, cap * sizeof(CIOverlap));
                                    if (!tmp) { *out_overlap_count = count; return overlaps; }
                                    overlaps = tmp;
                                }
                                strncpy(overlaps[count].shared_path, entries[e].path, CG_PATH_MAX - 1);
                                overlaps[count].shared_path[CG_PATH_MAX - 1] = '\0';
                                overlaps[count].build_a_index = a;
                                overlaps[count].build_b_index = b;
                                count++;
                                break;
                            }
                        }
                    }
                }
            }
        }
    }

    *out_overlap_count = count;
    return overlaps;
}

/* ── CacheGuardian (owning collection) ───────────────────────────── */

void cg_init(CacheGuardian *cg, Budget budget) {
    memset(cg, 0, sizeof(*cg));
    budget_tracker_init(&cg->budget, budget);
    dep_profile_init(&cg->global_profile);
    phase_state_init(&cg->phase);
}

int cg_add_entry(CacheGuardian *cg, const char *path, uint64_t size, uint32_t dep_hash) {
    if (cg->count >= CG_CACHE_MAX) return -1;
    CacheEntry *e = &cg->entries[cg->count];
    strncpy(e->path, path, CG_PATH_MAX - 1);
    e->path[CG_PATH_MAX - 1] = '\0';
    e->size = size;
    e->access_count = 1;
    e->last_access = time(NULL);
    e->dependency_hash = dep_hash;
    /* dep_profile already zeroed; set the bin for this hash */
    e->dep_profile[dep_hash % CG_HASH_BINS] = 1;

    /* Also update global profile */
    dep_profile_add_hash(&cg->global_profile, dep_hash);

    budget_record_disk(&cg->budget, size);
    cg->count++;
    return 0;
}

void cg_access_entry(CacheGuardian *cg, size_t index) {
    if (index >= cg->count) return;
    cg->entries[index].access_count++;
    cg->entries[index].last_access = time(NULL);
}

size_t cg_evict_to_budget(CacheGuardian *cg) {
    size_t *evicted = NULL;
    size_t count = eviction_run(cg->entries, cg->count,
                                &cg->budget, &cg->global_profile, &evicted);
    /* Compact entries: we need to rebuild since eviction_run does swap-remove */
    /* eviction_run already modifies the array, just update count */
    /* Actually eviction_run removes in-place, but count isn't returned from it.
     * We know how many were evicted, so subtract. */
    cg->count -= count;
    free(evicted);
    return count;
}

Phase cg_update_phase(CacheGuardian *cg, double recent_divergence) {
    return phase_detect(&cg->phase, recent_divergence);
}

size_t cg_ci_optimize(CacheGuardian *cg, const CIBuild *builds, size_t build_count) {
    size_t overlap_count = 0;
    CIOverlap *overlaps = ci_detect_overlaps(builds, build_count,
                                             cg->entries, cg->count,
                                             &overlap_count);
    /* For shared packages, increase access count (prioritize keeping) */
    for (size_t i = 0; i < overlap_count; i++) {
        for (size_t e = 0; e < cg->count; e++) {
            if (strcmp(cg->entries[e].path, overlaps[i].shared_path) == 0) {
                cg->entries[e].access_count++;
            }
        }
    }
    free(overlaps);
    return overlap_count;
}
