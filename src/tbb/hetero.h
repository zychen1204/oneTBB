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

#ifndef _TBB_hetero_H
#define _TBB_hetero_H

#include <cstdint>

namespace tbb {
namespace detail {
namespace r1 {

//! Static class of a logical CPU on a hybrid (P-core/E-core) machine.
/** Values are stable across the library: they are stored in 8-bit atomics
    inside arena slots, so keep them small. **/
enum class core_class : std::uint8_t {
    unknown     = 0,  // not yet classified, or topology unavailable
    performance = 1,  // P-core (e.g. Intel Golden Cove / Lion Cove, ARM big)
    efficiency  = 2   // E-core (e.g. Intel Gracemont / Skymont, ARM LITTLE)
};

//! Detection of the hybrid CPU topology and tunables of the CAWS policy.
/** Topology sources, in priority order:
      1. TBB_HETERO_PCORES=<cpulist>      explicit override, e.g. "0-3" or "0-7,16"
                                          (the listed CPUs are P-cores, all other
                                          online CPUs are E-cores)
      2. /sys/devices/cpu_core/cpus and /sys/devices/cpu_atom/cpus  (Intel hybrid)
      3. /sys/devices/system/cpu/cpuN/cpu_capacity                  (ARM DynamIQ)
    If no asymmetric topology is found the whole CAWS machinery is disabled and
    the scheduler behaves exactly like stock oneTBB.

    Tunables (environment variables):
      TBB_HETERO_DISABLE=1   force-disable CAWS even on a hybrid machine
      TBB_HETERO_PIN=1       pin worker threads to cores, P-cores first by slot index
      TBB_HETERO_ENDGAME=n   E-thieves stand down when the visible pending task
                             count in the arena is <= n  (default: #P-cores)
      TBB_HETERO_PATIENCE=n  after n consecutive declined rounds an E-thief steals
                             anyway, which bounds the stand-down latency (default: 8)
      TBB_HETERO_STATS=1     dump steal statistics at process exit **/
class hetero_topology {
public:
    //! True iff an asymmetric topology was detected and the feature is on.
    static bool enabled();

    //! Class of the given logical CPU (unknown if out of range / not hybrid).
    static core_class classify(int cpu);

    //! Class of the CPU the calling thread is currently running on.
    static core_class classify_current();

    static unsigned p_core_count();

    static bool pinning_enabled();
    static bool stats_enabled();

    //! Pending-task threshold below which E-thieves stand down.
    static unsigned endgame_threshold();

    //! Number of declined steal rounds after which an E-thief steals anyway.
    static unsigned patience();

    //! Pin the calling worker thread to the CPU assigned to the given arena slot
    //! (P-cores are assigned to lower slot indices first). Returns the class of
    //! the CPU the thread was pinned to.
    static core_class pin_current_thread(unsigned slot_index);

    // -------- statistics (all relaxed; only meaningful when stats_enabled) ----
    static void note_steal(core_class thief, core_class victim);
    static void note_endgame_decline();
};

} // namespace r1
} // namespace detail
} // namespace tbb

#endif // _TBB_hetero_H
