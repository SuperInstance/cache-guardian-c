# cache-guardian-c

A C port of **uv-cache-guardian** — resource-aware caching with disk/bandwidth/time budgets, phase detection, KL-divergence eviction, and CI optimization.

Zero external dependencies. C11. `-Wall -Wextra` clean.

## Features

- **Budget tracking** — enforce limits on disk space, daily bandwidth, and per-run time
- **Dependency profiles** — hash-based fingerprints for cache entries
- **KL-divergence eviction** — evict entries least similar to the reference profile first
- **Phase detection** — classify workload as Stable → PreTransition → Transitioning → PostTransition
- **CI overlap detection** — identify shared packages across builds for cache optimization

## Building

```sh
make            # builds libcacheguardian.a
make test       # compiles and runs test suite
make clean      # remove build artifacts
```

## API Overview

```c
#include "cache_guardian.h"

/* Create a cache guardian with budgets */
CacheGuardian cg;
cg_init(&cg, (Budget){ .disk_bytes = 1ULL << 30,       /* 1 GiB */
                        .bandwidth_bytes_per_day = 5ULL << 30,
                        .time_seconds_per_run = 300 });

/* Add entries */
cg_add_entry(&cg, "/path/to/pkg", 4096, hash_deps(...));

/* Access (updates LRU) */
cg_access_entry(&cg, 0);

/* Evict until budget is satisfied (KL-divergence ranked) */
size_t evicted = cg_evict_to_budget(&cg);

/* Phase detection */
Phase p = cg_update_phase(&cg, recent_kl);

/* CI optimization — boost entries shared across builds */
CIBuild builds[N] = { ... };
size_t overlaps = cg_ci_optimize(&cg, builds, N);
```

## Running Tests

```sh
make test
```

26 tests covering budgets, KL divergence, eviction ranking, phase detection, CI overlap, and integration.

## License

MIT
