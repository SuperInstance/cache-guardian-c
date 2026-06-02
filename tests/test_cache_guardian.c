#include "cache_guardian.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>

static int tests_passed = 0;
#define TEST(name) printf("  TEST %-50s ", name); tests_passed++
#define PASS() printf("PASS\n")
#define CHECK(cond) do { if (!(cond)) { printf("FAIL (%s:%d)\n", __FILE__, __LINE__); exit(1); } } while(0)

/* ── Budget Tests ────────────────────────────────────────────────── */

static void test_budget_init(void) {
    TEST("budget_tracker_init zeroes usage");
    BudgetTracker bt;
    Budget b = {1000, 2000, 3000};
    budget_tracker_init(&bt, b);
    CHECK(bt.disk_used == 0);
    CHECK(bt.bandwidth_used_today == 0);
    CHECK(bt.time_used_run == 0);
    CHECK(bt.limit.disk_bytes == 1000);
    PASS();
}

static void test_budget_check_disk_pass(void) {
    TEST("budget_check_disk passes under limit");
    BudgetTracker bt;
    budget_tracker_init(&bt, (Budget){1000, 0, 0});
    budget_record_disk(&bt, 500);
    CHECK(budget_check_disk(&bt, 400) == true);
    PASS();
}

static void test_budget_check_disk_fail(void) {
    TEST("budget_check_disk fails over limit");
    BudgetTracker bt;
    budget_tracker_init(&bt, (Budget){1000, 0, 0});
    budget_record_disk(&bt, 800);
    CHECK(budget_check_disk(&bt, 300) == false);
    PASS();
}

static void test_budget_check_bandwidth(void) {
    TEST("budget_check_bandwidth enforces daily limit");
    BudgetTracker bt;
    budget_tracker_init(&bt, (Budget){0, 500, 0});
    budget_record_bandwidth(&bt, 400);
    CHECK(budget_check_bandwidth(&bt, 50) == true);
    CHECK(budget_check_bandwidth(&bt, 200) == false);
    PASS();
}

static void test_budget_check_time(void) {
    TEST("budget_check_time enforces run limit");
    BudgetTracker bt;
    budget_tracker_init(&bt, (Budget){0, 0, 100});
    budget_record_time(&bt, 80);
    CHECK(budget_check_time(&bt, 10) == true);
    CHECK(budget_check_time(&bt, 30) == false);
    PASS();
}

static void test_budget_unlimited(void) {
    TEST("budget with zero limits is unlimited");
    BudgetTracker bt;
    budget_tracker_init(&bt, (Budget){0, 0, 0});
    CHECK(budget_check_disk(&bt, 999999) == true);
    CHECK(budget_check_bandwidth(&bt, 999999) == true);
    CHECK(budget_check_time(&bt, 999999) == true);
    PASS();
}

static void test_budget_enforce_combined(void) {
    TEST("budget_enforce checks all three budgets");
    BudgetTracker bt;
    budget_tracker_init(&bt, (Budget){1000, 1000, 100});
    budget_record_disk(&bt, 900);
    budget_record_bandwidth(&bt, 900);
    budget_record_time(&bt, 90);
    CHECK(budget_enforce(&bt, 50, 5) == true);
    CHECK(budget_enforce(&bt, 200, 5) == false);
    PASS();
}

static void test_budget_reset(void) {
    TEST("budget_reset_day clears bandwidth");
    BudgetTracker bt;
    budget_tracker_init(&bt, (Budget){0, 1000, 0});
    budget_record_bandwidth(&bt, 500);
    budget_reset_day(&bt);
    CHECK(bt.bandwidth_used_today == 0);
    PASS();
}

/* ── Dependency Profile & KL Tests ───────────────────────────────── */

static void test_dep_profile_init(void) {
    TEST("dep_profile_init zeroes all bins");
    DependencyProfile p;
    dep_profile_init(&p);
    for (size_t i = 0; i < CG_HASH_BINS; i++)
        CHECK(p.bins[i] == 0);
    CHECK(p.total == 0);
    PASS();
}

static void test_dep_profile_add(void) {
    TEST("dep_profile_add_hash increments bins");
    DependencyProfile p;
    dep_profile_init(&p);
    dep_profile_add_hash(&p, 42);
    dep_profile_add_hash(&p, 42 + CG_HASH_BINS); /* same bin */
    CHECK(p.bins[42 % CG_HASH_BINS] == 2);
    CHECK(p.total == 2);
    PASS();
}

static void test_kl_divergence_identical(void) {
    TEST("KL divergence of identical distributions is ~0");
    DependencyProfile p;
    dep_profile_init(&p);
    for (uint32_t i = 0; i < 200; i++) dep_profile_add_hash(&p, i);
    double norm[CG_HASH_BINS];
    dep_profile_normalize(&p, norm);
    double kl = kl_divergence(norm, norm);
    CHECK(kl < 1e-10);
    PASS();
}

static void test_kl_divergence_different(void) {
    TEST("KL divergence of different distributions is >0");
    DependencyProfile p, q;
    dep_profile_init(&p);
    dep_profile_init(&q);
    for (uint32_t i = 0; i < 100; i++) dep_profile_add_hash(&p, i % 4);
    for (uint32_t i = 0; i < 100; i++) dep_profile_add_hash(&q, i % 8);
    double pn[CG_HASH_BINS], qn[CG_HASH_BINS];
    dep_profile_normalize(&p, pn);
    dep_profile_normalize(&q, qn);
    double kl = kl_divergence(pn, qn);
    CHECK(kl > 0.0);
    PASS();
}

static void test_symmetrized_kl(void) {
    TEST("symmetrized KL is symmetric");
    DependencyProfile p, q;
    dep_profile_init(&p);
    dep_profile_init(&q);
    for (uint32_t i = 0; i < 50; i++) dep_profile_add_hash(&p, i);
    for (uint32_t i = 50; i < 100; i++) dep_profile_add_hash(&q, i);
    double pn[CG_HASH_BINS], qn[CG_HASH_BINS];
    dep_profile_normalize(&p, pn);
    dep_profile_normalize(&q, qn);
    double kl_pq = symmetrized_kl(pn, qn);
    double kl_qp = symmetrized_kl(qn, pn);
    CHECK(fabs(kl_pq - kl_qp) < 1e-10);
    PASS();
}

/* ── Eviction Tests ──────────────────────────────────────────────── */

static void test_eviction_rank_order(void) {
    TEST("eviction_rank sorts by divergence descending");
    /* Use entries with overlapping but different profiles */
    CacheEntry entries[3];
    memset(entries, 0, sizeof(entries));

    /* Entry 0: mostly hash 1, some hash 2 — close to ref */
    strncpy(entries[0].path, "/a", CG_PATH_MAX);
    entries[0].dependency_hash = 1;
    entries[0].dep_profile[1 % CG_HASH_BINS] = 8;
    entries[0].dep_profile[2 % CG_HASH_BINS] = 2;

    /* Entry 1: mostly hash 10, some hash 11 — far from ref */
    strncpy(entries[1].path, "/b", CG_PATH_MAX);
    entries[1].dependency_hash = 10;
    entries[1].dep_profile[10 % CG_HASH_BINS] = 8;
    entries[1].dep_profile[11 % CG_HASH_BINS] = 2;

    /* Entry 2: hash 1 only — closest to ref */
    strncpy(entries[2].path, "/c", CG_PATH_MAX);
    entries[2].dependency_hash = 1;
    entries[2].dep_profile[1 % CG_HASH_BINS] = 5;

    /* Reference: mostly hash 1, some hash 2 */
    DependencyProfile ref;
    dep_profile_init(&ref);
    for (int i = 0; i < 8; i++) dep_profile_add_hash(&ref, 1);
    for (int i = 0; i < 2; i++) dep_profile_add_hash(&ref, 2);

    size_t cnt = 0;
    EvictionCandidate *cands = eviction_rank(entries, 3, &ref, &cnt);
    CHECK(cnt == 3);
    /* Entry[1] has hash 10/11, most divergent from ref(hash 1/2) */
    CHECK(cands[0].index == 1);
    CHECK(cands[0].divergence > cands[1].divergence);
    free(cands);
    PASS();
}

static void test_eviction_rank_empty(void) {
    TEST("eviction_rank returns NULL for empty cache");
    DependencyProfile ref;
    dep_profile_init(&ref);
    size_t cnt = 99;
    EvictionCandidate *cands = eviction_rank(NULL, 0, &ref, &cnt);
    CHECK(cands == NULL);
    CHECK(cnt == 0);
    PASS();
}

static void test_eviction_run_frees_budget(void) {
    TEST("eviction_run evicts until budget is satisfied");
    CacheEntry entries[3];
    memset(entries, 0, sizeof(entries));
    for (int i = 0; i < 3; i++) {
        snprintf(entries[i].path, CG_PATH_MAX, "/e%d", i);
        entries[i].size = 500;
        entries[i].dependency_hash = (uint32_t)(i * 10);
        entries[i].dep_profile[entries[i].dependency_hash % CG_HASH_BINS] = 1;
    }
    BudgetTracker bt;
    budget_tracker_init(&bt, (Budget){800, 0, 0});
    budget_record_disk(&bt, 1500);

    DependencyProfile ref;
    dep_profile_init(&ref);
    dep_profile_add_hash(&ref, 0);

    size_t *evicted = NULL;
    size_t count = eviction_run(entries, 3, &bt, &ref, &evicted);
    CHECK(count >= 1);
    CHECK(bt.disk_used <= 800);
    free(evicted);
    PASS();
}

/* ── Phase Detection Tests ───────────────────────────────────────── */

static void test_phase_init_stable(void) {
    TEST("phase_state_init starts Stable");
    PhaseState ps;
    phase_state_init(&ps);
    CHECK(ps.current_phase == Phase_Stable);
    CHECK(ps.stability_score == 1.0);
    PASS();
}

static void test_phase_transitions(void) {
    TEST("phase transitions through states");
    PhaseState ps;
    phase_state_init(&ps);
    /* Feed increasing divergence to drive phase down */
    Phase p = phase_detect(&ps, 0.0);
    CHECK(p == Phase_Stable);
    /* Feed high divergence repeatedly */
    for (int i = 0; i < 20; i++)
        p = phase_detect(&ps, 5.0);
    CHECK(p != Phase_Stable);
    CHECK(ps.transition_count >= 1);
    PASS();
}

static void test_phase_recover(void) {
    TEST("phase recovers to Stable with low divergence");
    PhaseState ps;
    phase_state_init(&ps);
    for (int i = 0; i < 20; i++) phase_detect(&ps, 5.0);
    for (int i = 0; i < 30; i++) phase_detect(&ps, 0.0);
    CHECK(ps.current_phase == Phase_Stable);
    PASS();
}

static void test_phase_name(void) {
    TEST("phase_name returns correct strings");
    CHECK(strcmp(phase_name(Phase_Stable), "Stable") == 0);
    CHECK(strcmp(phase_name(Phase_PreTransition), "PreTransition") == 0);
    CHECK(strcmp(phase_name(Phase_Transitioning), "Transitioning") == 0);
    CHECK(strcmp(phase_name(Phase_PostTransition), "PostTransition") == 0);
    PASS();
}

/* ── CI Overlap Tests ────────────────────────────────────────────── */

static void test_ci_overlap_detects_shared(void) {
    TEST("ci_detect_overlaps finds shared deps");
    CacheEntry entries[2];
    memset(entries, 0, sizeof(entries));
    strncpy(entries[0].path, "/pkg-a", CG_PATH_MAX);
    entries[0].dependency_hash = 42;
    strncpy(entries[1].path, "/pkg-b", CG_PATH_MAX);
    entries[1].dependency_hash = 99;

    CIBuild builds[2];
    memset(builds, 0, sizeof(builds));
    strncpy(builds[0].build_id, "build-a", CG_PATH_MAX);
    builds[0].dep_hashes[0] = 42;
    builds[0].dep_count = 1;
    strncpy(builds[1].build_id, "build-b", CG_PATH_MAX);
    builds[1].dep_hashes[0] = 42;
    builds[1].dep_count = 1;

    size_t oc = 0;
    CIOverlap *overlaps = ci_detect_overlaps(builds, 2, entries, 2, &oc);
    CHECK(oc == 1);
    CHECK(strcmp(overlaps[0].shared_path, "/pkg-a") == 0);
    free(overlaps);
    PASS();
}

static void test_ci_overlap_no_overlap(void) {
    TEST("ci_detect_overlaps returns 0 for no shared deps");
    CacheEntry entries[2];
    memset(entries, 0, sizeof(entries));
    entries[0].dependency_hash = 10;
    entries[1].dependency_hash = 20;

    CIBuild builds[2];
    memset(builds, 0, sizeof(builds));
    builds[0].dep_hashes[0] = 10;
    builds[0].dep_count = 1;
    builds[1].dep_hashes[0] = 20;
    builds[1].dep_count = 1;

    size_t oc = 99;
    CIOverlap *overlaps = ci_detect_overlaps(builds, 2, entries, 2, &oc);
    CHECK(oc == 0);
    free(overlaps);
    PASS();
}

/* ── CacheGuardian Integration Tests ─────────────────────────────── */

static void test_cg_init(void) {
    TEST("cg_init initializes all fields");
    CacheGuardian cg;
    cg_init(&cg, (Budget){1000, 2000, 100});
    CHECK(cg.count == 0);
    CHECK(cg.budget.limit.disk_bytes == 1000);
    CHECK(cg.phase.current_phase == Phase_Stable);
    PASS();
}

static void test_cg_add_entry(void) {
    TEST("cg_add_entry adds entries and updates budget");
    CacheGuardian cg;
    cg_init(&cg, (Budget){10000, 0, 0});
    int r = cg_add_entry(&cg, "/foo", 500, 42);
    CHECK(r == 0);
    CHECK(cg.count == 1);
    CHECK(cg.budget.disk_used == 500);
    CHECK(strcmp(cg.entries[0].path, "/foo") == 0);
    PASS();
}

static void test_cg_add_too_many(void) {
    TEST("cg_add_entry returns -1 when full");
    CacheGuardian cg;
    cg_init(&cg, (Budget){UINT64_MAX, 0, 0});
    for (size_t i = 0; i < CG_CACHE_MAX; i++) {
        char path[CG_PATH_MAX];
        snprintf(path, sizeof(path), "/e%zu", i);
        cg_add_entry(&cg, path, 1, (uint32_t)i);
    }
    int r = cg_add_entry(&cg, "/overflow", 1, 0);
    CHECK(r == -1);
    PASS();
}

static void test_cg_access_entry(void) {
    TEST("cg_access_entry increments access count");
    CacheGuardian cg;
    cg_init(&cg, (Budget){0, 0, 0});
    cg_add_entry(&cg, "/bar", 100, 7);
    cg_access_entry(&cg, 0);
    cg_access_entry(&cg, 0);
    CHECK(cg.entries[0].access_count == 3); /* 1 from add + 2 accesses */
    PASS();
}

static void test_cg_ci_optimize(void) {
    TEST("cg_ci_optimize boosts shared entries");
    CacheGuardian cg;
    cg_init(&cg, (Budget){UINT64_MAX, 0, 0});
    cg_add_entry(&cg, "/shared", 100, 42);

    CIBuild builds[2];
    memset(builds, 0, sizeof(builds));
    builds[0].dep_hashes[0] = 42;
    builds[0].dep_count = 1;
    builds[1].dep_hashes[0] = 42;
    builds[1].dep_count = 1;

    size_t overlaps = cg_ci_optimize(&cg, builds, 2);
    CHECK(overlaps == 1);
    CHECK(cg.entries[0].access_count >= 2); /* boosted */
    PASS();
}

/* ── Main ────────────────────────────────────────────────────────── */

int main(void) {
    printf("=== Cache Guardian C Test Suite ===\n\n");

    test_budget_init();
    test_budget_check_disk_pass();
    test_budget_check_disk_fail();
    test_budget_check_bandwidth();
    test_budget_check_time();
    test_budget_unlimited();
    test_budget_enforce_combined();
    test_budget_reset();
    test_dep_profile_init();
    test_dep_profile_add();
    test_kl_divergence_identical();
    test_kl_divergence_different();
    test_symmetrized_kl();
    test_eviction_rank_order();
    test_eviction_rank_empty();
    test_eviction_run_frees_budget();
    test_phase_init_stable();
    test_phase_transitions();
    test_phase_recover();
    test_phase_name();
    test_ci_overlap_detects_shared();
    test_ci_overlap_no_overlap();
    test_cg_init();
    test_cg_add_entry();
    test_cg_add_too_many();
    test_cg_access_entry();
    test_cg_ci_optimize();

    printf("\n=== %d tests passed ===\n", tests_passed);
    return 0;
}
