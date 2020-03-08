/* SPDX-License-Identifier: LGPL-2.1+ */

#include <sys/xattr.h>
#include <unistd.h>

#include "fd-util.h"
#include "format-util.h"
#include "oomd-util.h"
#include "parse-util.h"
#include "path-util.h"
#include "procfs-util.h"
#include "signal-util.h"
#include "sort-util.h"
#include "stat-util.h"
#include "stdio-util.h"

DEFINE_HASH_OPS_WITH_VALUE_DESTRUCTOR(
                oomd_cgroup_ctx_hash_ops,
                char,
                string_hash_func,
                string_compare_func,
                OomdCGroupContext,
                oomd_cgroup_context_free);

static int log_kill(pid_t pid, int sig, void *userdata) {
        log_debug("oomd attempting to kill " PID_FMT " with %s", pid, signal_to_string(sig));
        return 0;
}

static int increment_oomd_xattr(const char *path, const char *xattr, uint64_t num_procs_killed) {
        _cleanup_free_ char *value = NULL;
        char buf[DECIMAL_STR_MAX(uint64_t) + 1];
        uint64_t curr_count = 0;
        int r;

        assert(path);
        assert(xattr);

        r = cg_get_xattr_malloc(SYSTEMD_CGROUP_CONTROLLER, path, xattr, &value);
        if (r < 0 && r != -ENODATA)
                return r;

        if (!isempty(value)) {
                 r = safe_atou64(value, &curr_count);
                 if (r < 0)
                         return r;
        }

        if (curr_count > UINT64_MAX - num_procs_killed)
                return -EOVERFLOW;

        xsprintf(buf, "%"PRIu64, curr_count + num_procs_killed);
        r = cg_set_xattr(SYSTEMD_CGROUP_CONTROLLER, path, xattr, buf, strlen(buf), 0);
        if (r < 0)
                return r;

        return 0;
}

OomdCGroupContext *oomd_cgroup_context_free(OomdCGroupContext *ctx) {
        if (!ctx)
                return NULL;

        free(ctx->path);
        return mfree(ctx);
}

int oomd_pressure_above(Hashmap *h, usec_t duration, Set **ret) {
        _cleanup_set_free_ Set *targets = NULL;
        OomdCGroupContext *ctx;
        char *key;
        int r;

        assert(h);
        assert(ret);

        targets = set_new(NULL);
        if (!targets)
                return -ENOMEM;

        HASHMAP_FOREACH_KEY(ctx, key, h) {
                if (ctx->memory_pressure.avg10 > ctx->mem_pressure_limit) {
                        usec_t diff;

                        if (ctx->last_hit_mem_pressure_limit == 0)
                                ctx->last_hit_mem_pressure_limit = now(CLOCK_MONOTONIC);

                        diff = now(CLOCK_MONOTONIC) - ctx->last_hit_mem_pressure_limit;
                        if (diff >= duration) {
                                r = set_put(targets, ctx);
                                if (r < 0)
                                        return -ENOMEM;
                        }
                } else
                        ctx->last_hit_mem_pressure_limit = 0;
        }

        if (!set_isempty(targets)) {
                *ret = TAKE_PTR(targets);
                return 1;
        }

        *ret = NULL;
        return 0;
}

bool oomd_memory_reclaim(Hashmap *h) {
        uint64_t pgscan = 0, pgscan_of = 0, last_pgscan = 0, last_pgscan_of = 0;
        OomdCGroupContext *ctx;

        assert(h);

        /* If sum of all the current pgscan values are greater than the sum of all the last_pgscan values,
         * there was reclaim activity. Used along with pressure checks to decide whether to take action. */

        HASHMAP_FOREACH(ctx, h) {
                uint64_t sum;

                sum = pgscan + ctx->pgscan;
                if (sum < pgscan || sum < ctx->pgscan)
                        pgscan_of++; /* count overflows */
                pgscan = sum;

                sum = last_pgscan + ctx->last_pgscan;
                if (sum < last_pgscan || sum < ctx->last_pgscan)
                        last_pgscan_of++; /* count overflows */
                last_pgscan = sum;
        }

        /* overflow counts are the same, return sums comparison */
        if (last_pgscan_of == pgscan_of)
                return pgscan > last_pgscan;

        return pgscan_of > last_pgscan_of;
}

bool oomd_swap_free_below(const OomdSystemContext *ctx, uint64_t threshold_percent) {
        uint64_t swap_threshold;

        assert(ctx);
        assert(threshold_percent <= 100);

        swap_threshold = ctx->swap_total * threshold_percent / ((uint64_t) 100);
        return (ctx->swap_total - ctx->swap_used) < swap_threshold;
}

int oomd_sort_cgroup_contexts(Hashmap *h, oomd_compare_t compare_func, const char *prefix, OomdCGroupContext ***ret) {
        _cleanup_free_ OomdCGroupContext **sorted = NULL;
        OomdCGroupContext *item;
        size_t k = 0;

        assert(h);
        assert(compare_func);
        assert(ret);

        sorted = new0(OomdCGroupContext*, hashmap_size(h));
        if (!sorted)
                return -ENOMEM;

        HASHMAP_FOREACH(item, h) {
                if (item->path && prefix && !path_startswith(item->path, prefix))
                        continue;

                sorted[k++] = item;
        }

        typesafe_qsort(sorted, k, compare_func);

        *ret = TAKE_PTR(sorted);

        assert(k <= INT_MAX);
        return (int) k;
}

int oomd_cgroup_kill(const char *path, bool recurse, bool dry_run) {
        _cleanup_set_free_ Set *pids_killed = NULL;
        int r;

        assert(path);

        if (dry_run) {
                _cleanup_free_ char *cg_path = NULL;

                r = cg_get_path(SYSTEMD_CGROUP_CONTROLLER, path, NULL, &cg_path);
                if (r < 0)
                        return r;

                log_debug("oomd dry-run: Would have tried to kill %s with recurse=%s", cg_path, true_false(recurse));
                return 0;
        }

        pids_killed = set_new(NULL);
        if (!pids_killed)
                return -ENOMEM;

        if (recurse)
                r = cg_kill_recursive(SYSTEMD_CGROUP_CONTROLLER, path, SIGKILL, CGROUP_IGNORE_SELF, pids_killed, log_kill, NULL);
        else
                r = cg_kill(SYSTEMD_CGROUP_CONTROLLER, path, SIGKILL, CGROUP_IGNORE_SELF, pids_killed, log_kill, NULL);
        if (r < 0)
                return r;

        r = increment_oomd_xattr(path, "user.systemd_oomd_kill", set_size(pids_killed));
        if (r < 0)
                log_debug_errno(r, "Failed to set user.systemd_oomd_kill on kill: %m");

        return set_size(pids_killed) != 0;
}

int oomd_kill_by_pgscan(Hashmap *h, const char *prefix, bool dry_run) {
        _cleanup_free_ OomdCGroupContext **sorted = NULL;
        int r;

        assert(h);

        r = oomd_sort_cgroup_contexts(h, compare_pgscan, prefix, &sorted);
        if (r < 0)
                return r;

        for (int i = 0; i < r; i++) {
                if (sorted[i]->pgscan == 0)
                        break;

                r = oomd_cgroup_kill(sorted[i]->path, true, dry_run);
                if (r > 0 || r == -ENOMEM)
                        break;
        }

        return r;
}

int oomd_kill_by_swap_usage(Hashmap *h, bool dry_run) {
        _cleanup_free_ OomdCGroupContext **sorted = NULL;
        int r;

        assert(h);

        r = oomd_sort_cgroup_contexts(h, compare_swap_usage, NULL, &sorted);
        if (r < 0)
                return r;

        /* Try to kill cgroups with non-zero swap usage until we either succeed in
         * killing or we get to a cgroup with no swap usage. */
        for (int i = 0; i < r; i++) {
                if (sorted[i]->swap_usage == 0)
                        break;

                r = oomd_cgroup_kill(sorted[i]->path, true, dry_run);
                if (r > 0 || r == -ENOMEM)
                        break;
        }

        return r;
}

int oomd_cgroup_context_acquire(const char *path, OomdCGroupContext **ret) {
        _cleanup_(oomd_cgroup_context_freep) OomdCGroupContext *ctx = NULL;
        _cleanup_free_ char *p = NULL, *val = NULL;
        bool is_root;
        int r;

        assert(path);
        assert(ret);

        ctx = new0(OomdCGroupContext, 1);
        if (!ctx)
                return -ENOMEM;

        is_root = empty_or_root(path);

        r = cg_get_path(SYSTEMD_CGROUP_CONTROLLER, path, "memory.pressure", &p);
        if (r < 0)
                return log_debug_errno(r, "Error getting cgroup memory pressure path from %s: %m", path);

        r = read_resource_pressure(p, PRESSURE_TYPE_FULL, &ctx->memory_pressure);
        if (r < 0)
                return log_debug_errno(r, "Error parsing memory pressure from %s: %m", p);

        if (is_root) {
                r = procfs_memory_get_used(&ctx->current_memory_usage);
                if (r < 0)
                        return log_debug_errno(r, "Error getting memory used from procfs: %m");
        } else {
                r = cg_get_attribute_as_uint64(SYSTEMD_CGROUP_CONTROLLER, path, "memory.current", &ctx->current_memory_usage);
                if (r < 0)
                        return log_debug_errno(r, "Error getting memory.current from %s: %m", path);

                r = cg_get_attribute_as_uint64(SYSTEMD_CGROUP_CONTROLLER, path, "memory.min", &ctx->memory_min);
                if (r < 0)
                        return log_debug_errno(r, "Error getting memory.min from %s: %m", path);

                r = cg_get_attribute_as_uint64(SYSTEMD_CGROUP_CONTROLLER, path, "memory.low", &ctx->memory_low);
                if (r < 0)
                        return log_debug_errno(r, "Error getting memory.low from %s: %m", path);

                r = cg_get_attribute_as_uint64(SYSTEMD_CGROUP_CONTROLLER, path, "memory.swap.current", &ctx->swap_usage);
                if (r < 0)
                        return log_debug_errno(r, "Error getting memory.swap.current from %s: %m", path);

                r = cg_get_keyed_attribute(SYSTEMD_CGROUP_CONTROLLER, path, "memory.stat", STRV_MAKE("pgscan"), &val);
                if (r < 0)
                        return log_debug_errno(r, "Error getting pgscan from memory.stat under %s: %m", path);

                r = safe_atou64(val, &ctx->pgscan);
                if (r < 0)
                        return log_debug_errno(r, "Error converting pgscan value to uint64_t: %m");
        }

        ctx->path = strdup(empty_to_root(path));
        if (!ctx->path)
                return -ENOMEM;

        *ret = TAKE_PTR(ctx);
        return 0;
}

int oomd_system_context_acquire(const char *proc_swaps_path, OomdSystemContext *ret) {
        _cleanup_fclose_ FILE *f = NULL;
        OomdSystemContext ctx = {};
        int r;

        assert(proc_swaps_path);
        assert(ret);

        f = fopen(proc_swaps_path, "re");
        if (!f)
                return -errno;

        (void) fscanf(f, "%*s %*s %*s %*s %*s\n");

        for (;;) {
                uint64_t total, used;

                r = fscanf(f,
                           "%*s "          /* device/file */
                           "%*s "          /* type of swap */
                           "%" PRIu64 " "  /* swap size */
                           "%" PRIu64 " "  /* used */
                           "%*s\n",        /* priority */
                           &total, &used);

                if (r == EOF && feof(f))
                         break;

                if (r != 2) {
                        if (ferror(f))
                                return log_debug_errno(errno, "Error reading from %s: %m", proc_swaps_path);

                        return log_debug_errno(SYNTHETIC_ERRNO(EIO),
                                               "Failed to parse values from %s: %m", proc_swaps_path);
                }

                ctx.swap_total += total * 1024U;
                ctx.swap_used += used * 1024U;
        }

        *ret = ctx;
        return 0;
}

int oomd_insert_cgroup_context(Hashmap *old_h, Hashmap *new_h, const char *path) {
        _cleanup_(oomd_cgroup_context_freep) OomdCGroupContext *curr_ctx = NULL;
        OomdCGroupContext *old_ctx, *ctx;
        int r;

        assert(new_h);
        assert(path);

        r = oomd_cgroup_context_acquire(path, &curr_ctx);
        if (r < 0)
                return log_debug_errno(r, "Failed to get OomdCGroupContext for %s: %m", path);

        old_ctx = hashmap_get(old_h, path);
        if (old_ctx) {
                curr_ctx->last_pgscan = old_ctx->pgscan;
                curr_ctx->mem_pressure_limit = old_ctx->mem_pressure_limit;
                curr_ctx->last_hit_mem_pressure_limit = old_ctx->last_hit_mem_pressure_limit;
        }

        ctx = TAKE_PTR(curr_ctx);
        r = hashmap_put(new_h, ctx->path, ctx);
        if (r < 0)
                return r;

        return 0;
}
