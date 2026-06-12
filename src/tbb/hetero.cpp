/*
    Copyright (c) 2026 CAWS research patch (Capacity-Aware Work Stealing)

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

        http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
*/

#include "hetero.h"
#include "environment.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#if defined(__linux__)
#include <sched.h>
#include <unistd.h>
#include <sys/syscall.h>
#define __TBB_HETERO_PLATFORM 1
#else
#define __TBB_HETERO_PLATFORM 0
#endif

namespace {
#if defined(__linux__) && defined(SYS_getcpu)
// Returns the CPU the calling thread runs on, or -1.
//
// We deliberately avoid glibc's sched_getcpu(): on glibc >= 2.35 it reads the
// CPU id from the rseq area (a per-thread cache the kernel keeps current). That
// is fast on real hardware, but under a simulator whose SE mode ignores the
// rseq registration syscall (e.g. gem5), the cached id is never updated and
// every thread reports CPU 0 -- which silently collapses all P/E classification
// to "performance". Issuing the raw getcpu syscall bypasses the rseq cache and
// is correct on real hardware and on simulators that implement getcpu.
inline int tbb_raw_getcpu() {
    unsigned cpu = 0;
    long r = ::syscall(SYS_getcpu, &cpu, nullptr, nullptr);
    if (r != 0) {
        int s = ::sched_getcpu(); // fall back if getcpu is unavailable
        return s;
    }
    return static_cast<int>(cpu);
}
#elif defined(__linux__)
inline int tbb_raw_getcpu() { return ::sched_getcpu(); }
#endif
} // anonymous namespace

namespace tbb {
namespace detail {
namespace r1 {

namespace {

struct hetero_stats_t {
    // steals[thief][victim], indexed by core_class (unknown/performance/efficiency)
    std::atomic<unsigned long long> steals[3][3];
    std::atomic<unsigned long long> endgame_declines;
};
hetero_stats_t g_stats;

void dump_stats() {
    const char* names = "UPE";
    std::fprintf(stderr, "TBB-CAWS steal stats:\n");
    for (int t = 0; t < 3; ++t) {
        for (int v = 0; v < 3; ++v) {
            unsigned long long n = g_stats.steals[t][v].load(std::memory_order_relaxed);
            if (n) {
                std::fprintf(stderr, "  steals %c<-%c : %llu\n", names[t], names[v], n);
            }
        }
    }
    std::fprintf(stderr, "  endgame declines : %llu\n",
                 g_stats.endgame_declines.load(std::memory_order_relaxed));
}

#if __TBB_HETERO_PLATFORM
// Parses a Linux cpulist string such as "0-3,8,10-11"; marks the listed CPUs in mask.
// Returns false if the string contains nothing parsable.
bool parse_cpulist(const char* s, std::vector<std::uint8_t>& mask) {
    bool any = false;
    while (*s) {
        char* end = nullptr;
        long first = std::strtol(s, &end, 10);
        if (end == s || first < 0) break;
        long last = first;
        s = end;
        if (*s == '-') {
            last = std::strtol(s + 1, &end, 10);
            if (end == s + 1 || last < first) break;
            s = end;
        }
        for (long cpu = first; cpu <= last; ++cpu) {
            if (cpu >= (long)mask.size()) mask.resize(cpu + 1, 0);
            mask[cpu] = 1;
            any = true;
        }
        if (*s == ',') ++s;
        else break;
    }
    return any;
}

bool read_file_cpulist(const char* path, std::vector<std::uint8_t>& mask) {
    std::FILE* f = std::fopen(path, "r");
    if (!f) return false;
    char buf[4096];
    std::size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    buf[n] = '\0';
    return parse_cpulist(buf, mask);
}

long read_cpu_capacity(int cpu) {
    char path[128];
    std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpu_capacity", cpu);
    std::FILE* f = std::fopen(path, "r");
    if (!f) return -1;
    long v = -1;
    if (std::fscanf(f, "%ld", &v) != 1) v = -1;
    std::fclose(f);
    return v;
}
#endif // __TBB_HETERO_PLATFORM

struct hetero_state {
    bool enabled = false;
    bool pin = false;
    bool stats = false;
    // True when the P-core set came from TBB_HETERO_PCORES, i.e. the user
    // declared "these are P, every other CPU is E". Lets classify() treat a CPU
    // beyond the detected count as E instead of unknown -- important under
    // simulators where sysconf(_SC_NPROCESSORS_CONF) under-reports the CPU count
    // (e.g. gem5 SE returned 8 for a 16-core guest, leaving CPUs 8-15 unknown).
    bool explicit_pcores = false;
    unsigned endgame = 0;
    unsigned patience = 8;
    unsigned p_count = 0;
    std::vector<std::uint8_t> klass; // per-CPU core_class value
    std::vector<int> cpu_order;      // P-core CPU ids first, then E-core ids

    hetero_state() {
#if __TBB_HETERO_PLATFORM
        if (GetBoolEnvironmentVariable("TBB_HETERO_DISABLE")) return;

        long nconf = sysconf(_SC_NPROCESSORS_CONF);
        if (nconf <= 1) return;

        std::vector<std::uint8_t> p_mask, e_mask;
        bool have_p = false, have_e = false;

        // 1. Explicit override (used for gem5 and virtualized environments).
        if (const char* s = std::getenv("TBB_HETERO_PCORES")) {
            have_p = parse_cpulist(s, p_mask);
            // Everything not listed is an E-core.
            if (have_p) {
                e_mask.assign((std::size_t)nconf, 1);
                for (std::size_t i = 0; i < p_mask.size() && i < e_mask.size(); ++i)
                    if (p_mask[i]) e_mask[i] = 0;
                have_e = true;
                explicit_pcores = true;
            }
        }
        // 2. Intel hybrid sysfs interface.
        if (!have_p) {
            have_p = read_file_cpulist("/sys/devices/cpu_core/cpus", p_mask);
            have_e = read_file_cpulist("/sys/devices/cpu_atom/cpus", e_mask);
        }
        // 3. ARM cpu_capacity: CPUs at the maximum capacity are P-cores.
        if (!have_p) {
            long max_cap = -1;
            std::vector<long> caps((std::size_t)nconf, -1);
            for (long i = 0; i < nconf; ++i) {
                caps[i] = read_cpu_capacity((int)i);
                if (caps[i] > max_cap) max_cap = caps[i];
            }
            if (max_cap > 0) {
                p_mask.assign((std::size_t)nconf, 0);
                e_mask.assign((std::size_t)nconf, 0);
                for (long i = 0; i < nconf; ++i) {
                    if (caps[i] == max_cap) { p_mask[i] = 1; have_p = true; }
                    else if (caps[i] >= 0)  { e_mask[i] = 1; have_e = true; }
                }
            }
        }
        if (!have_p || !have_e) return;

        klass.assign((std::size_t)nconf, (std::uint8_t)core_class::unknown);
        for (long i = 0; i < nconf; ++i) {
            if (i < (long)p_mask.size() && p_mask[i]) {
                klass[i] = (std::uint8_t)core_class::performance;
                ++p_count;
            } else if (i < (long)e_mask.size() && e_mask[i]) {
                klass[i] = (std::uint8_t)core_class::efficiency;
            }
        }
        if (p_count == 0 || p_count == (unsigned)nconf) return; // not asymmetric

        for (long i = 0; i < nconf; ++i)
            if (klass[i] == (std::uint8_t)core_class::performance) cpu_order.push_back((int)i);
        for (long i = 0; i < nconf; ++i)
            if (klass[i] == (std::uint8_t)core_class::efficiency) cpu_order.push_back((int)i);

        enabled = true;
        pin = GetBoolEnvironmentVariable("TBB_HETERO_PIN");
        stats = GetBoolEnvironmentVariable("TBB_HETERO_STATS");

        long v = GetIntegralEnvironmentVariable("TBB_HETERO_ENDGAME");
        endgame = v >= 0 ? (unsigned)v : p_count;
        v = GetIntegralEnvironmentVariable("TBB_HETERO_PATIENCE");
        if (v >= 0) patience = (unsigned)v;

        if (stats) std::atexit(dump_stats);
#endif // __TBB_HETERO_PLATFORM
    }
};

hetero_state& state() {
    static hetero_state s;
    return s;
}

} // anonymous namespace

bool hetero_topology::enabled() { return state().enabled; }

core_class hetero_topology::classify(int cpu) {
    const hetero_state& s = state();
    if (!s.enabled || cpu < 0) return core_class::unknown;
    if (cpu >= (int)s.klass.size()) {
        // Beyond the detected CPU count. With an explicit P-core list, anything
        // not declared P is E by definition (the detected array just happened to
        // be too short); otherwise we genuinely do not know.
        return s.explicit_pcores ? core_class::efficiency : core_class::unknown;
    }
    return (core_class)s.klass[cpu];
}

core_class hetero_topology::classify_current() {
#if __TBB_HETERO_PLATFORM
    return classify(tbb_raw_getcpu());
#else
    return core_class::unknown;
#endif
}

unsigned hetero_topology::p_core_count() { return state().p_count; }
bool hetero_topology::pinning_enabled() { return state().pin; }
bool hetero_topology::stats_enabled() { return state().stats; }
unsigned hetero_topology::endgame_threshold() { return state().endgame; }
unsigned hetero_topology::patience() { return state().patience; }

core_class hetero_topology::pin_current_thread(unsigned slot_index) {
#if __TBB_HETERO_PLATFORM
    const hetero_state& s = state();
    if (!s.enabled || s.cpu_order.empty()) return classify_current();
    int cpu = s.cpu_order[slot_index % s.cpu_order.size()];
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        // Pinning may be forbidden (containers, restricted cpusets); fall back
        // to plain classification of wherever we happen to run.
        return classify_current();
    }
    return classify(cpu);
#else
    (void)slot_index;
    return core_class::unknown;
#endif
}

void hetero_topology::note_steal(core_class thief, core_class victim) {
    if (!state().stats) return;
    g_stats.steals[(int)thief][(int)victim].fetch_add(1, std::memory_order_relaxed);
}

void hetero_topology::note_endgame_decline() {
    if (!state().stats) return;
    g_stats.endgame_declines.fetch_add(1, std::memory_order_relaxed);
}

} // namespace r1
} // namespace detail
} // namespace tbb
