# gem5 SE-mode configuration: heterogeneous P-core / E-core x86 system
# for evaluating the CAWS oneTBB patch with PARSEC 3.0 (bodytrack, fluidanimate).
#
# Topology (default 4P + 8E, Raptor-Lake-like with the 1P:2E core ratio):
#
#   P0..P3 : 8-wide OoO @ 4.0 GHz, private L1I 32K / L1D 48K / L2 2M
#   E0..E7 : 4-wide OoO @ 2.8 GHz, private L1I 64K / L1D 32K,
#            one 4 MB L2 shared per 4-core E-cluster (2 clusters)
#   shared L3 16 MB + DDR4-2400
#
# CPU ids: 0..num_p-1 are P-cores, num_p..num_p+num_e-1 are E-cores,
# which matches TBB_HETERO_PCORES=0-<num_p-1> inside the workload env.
#
# Example:
#   build/X86/gem5.opt configs/hetero_pe_se.py \
#       --cmd .../bodytrack --options "sequenceB_1 4 1 1000 5 2 8" \
#       --env TBB_HETERO_PCORES=0-3 --env TBB_HETERO_STATS=1
#
# Tested target: gem5 v23.0/v24.0 classic memory system, SE mode.
# SE mode supports multithreaded workloads via futex/clone emulation.

import argparse
import os

import m5
from m5.objects import (
    AddrRange,
    Cache,
    DDR4_2400_16x4,
    L2XBar,
    MemCtrl,
    Process,
    Root,
    SEWorkload,
    SrcClockDomain,
    System,
    SystemXBar,
    TAGE_SC_L_64KB,
    TAGE_SC_L_8KB,
    VoltageDomain,
    X86O3CPU,
)

# --------------------------------------------------------------------------
# Core models
# --------------------------------------------------------------------------

class PCore(X86O3CPU):
    """Performance core: Golden/Raptor-Cove-class wide OoO."""
    fetchWidth = 8
    decodeWidth = 8
    renameWidth = 8
    dispatchWidth = 8
    issueWidth = 8
    wbWidth = 8
    commitWidth = 6
    squashWidth = 6
    numROBEntries = 1024
    numIQEntries = 320
    LQEntries = 256
    SQEntries = 160
    numPhysIntRegs = 384
    numPhysFloatRegs = 384
    branchPred = TAGE_SC_L_64KB()


class ECore(X86O3CPU):
    """Efficiency core: Gracemont-class narrow OoO."""
    fetchWidth = 4
    decodeWidth = 4
    renameWidth = 4
    dispatchWidth = 4
    issueWidth = 4
    wbWidth = 4
    commitWidth = 4
    squashWidth = 4
    numROBEntries = 256
    numIQEntries = 64
    LQEntries = 80
    SQEntries = 50
    numPhysIntRegs = 160
    numPhysFloatRegs = 160
    branchPred = TAGE_SC_L_8KB()


# --------------------------------------------------------------------------
# Cache models
# --------------------------------------------------------------------------

class L1Cache(Cache):
    tag_latency = 1
    data_latency = 3
    response_latency = 1
    mshrs = 16
    tgts_per_mshr = 20
    writeback_clean = True


class PL1I(L1Cache):
    size = "32kB"
    assoc = 8


class PL1D(L1Cache):
    size = "48kB"
    assoc = 12


class EL1I(L1Cache):
    size = "64kB"
    assoc = 8


class EL1D(L1Cache):
    size = "32kB"
    assoc = 8
    data_latency = 3


class PL2(Cache):
    """Private per-P-core L2 (Raptor-Cove-class, 2 MB)."""
    size = "2MB"
    assoc = 16
    tag_latency = 5
    data_latency = 15
    response_latency = 5
    mshrs = 32
    tgts_per_mshr = 20
    writeback_clean = False


class EL2(Cache):
    """L2 shared by the four cores of one E-cluster (Gracemont-class, 4 MB)."""
    size = "4MB"
    assoc = 16
    tag_latency = 6
    data_latency = 18
    response_latency = 6
    mshrs = 48
    tgts_per_mshr = 20
    writeback_clean = False


class L3(Cache):
    size = "16MB"
    assoc = 16
    tag_latency = 14
    data_latency = 42
    response_latency = 14
    mshrs = 64
    tgts_per_mshr = 24
    clusivity = "mostly_excl"


# --------------------------------------------------------------------------
# Argument parsing
# --------------------------------------------------------------------------

parser = argparse.ArgumentParser()
parser.add_argument("--cmd", required=True, help="binary to simulate (static link recommended)")
parser.add_argument("--options", default="", help="argv passed to the binary")
parser.add_argument("--env", action="append", default=[],
                    help="environment entry VAR=VALUE (repeatable)")
parser.add_argument("--num-p", type=int, default=4)
parser.add_argument("--num-e", type=int, default=8)
parser.add_argument("--p-clock", default="4GHz")
parser.add_argument("--e-clock", default="2.8GHz")
parser.add_argument("--mem-size", default="4GB")
parser.add_argument("--max-insts", type=int, default=0,
                    help="optional per-cpu instruction limit")
args = parser.parse_args()

num_cpus = args.num_p + args.num_e

# --------------------------------------------------------------------------
# System
# --------------------------------------------------------------------------

system = System()
system.voltage_domain = VoltageDomain()
system.clk_domain = SrcClockDomain(clock="2GHz", voltage_domain=system.voltage_domain)
system.p_clk_domain = SrcClockDomain(clock=args.p_clock, voltage_domain=system.voltage_domain)
system.e_clk_domain = SrcClockDomain(clock=args.e_clock, voltage_domain=system.voltage_domain)

system.mem_mode = "timing"
system.mem_ranges = [AddrRange(args.mem_size)]

system.cpu = [PCore(cpu_id=i) for i in range(args.num_p)] + \
             [ECore(cpu_id=args.num_p + i) for i in range(args.num_e)]

system.membus = SystemXBar()
system.l3bus = L2XBar()
system.l3 = L3()
system.l3.cpu_side = system.l3bus.mem_side_ports
system.l3.mem_side = system.membus.cpu_side_ports

# Shared L2 per 4-core E-cluster (e.g. 8 E-cores -> 2 clusters).
num_clusters = (args.num_e + 3) // 4
system.e_l2bus = [L2XBar() for _ in range(num_clusters)]
system.e_l2 = [EL2() for _ in range(num_clusters)]
for bus, l2 in zip(system.e_l2bus, system.e_l2):
    l2.cpu_side = bus.mem_side_ports
    l2.mem_side = system.l3bus.cpu_side_ports

for i, cpu in enumerate(system.cpu):
    is_p = i < args.num_p
    cpu.clk_domain = system.p_clk_domain if is_p else system.e_clk_domain

    cpu.icache = PL1I() if is_p else EL1I()
    cpu.dcache = PL1D() if is_p else EL1D()
    cpu.icache.cpu_side = cpu.icache_port
    cpu.dcache.cpu_side = cpu.dcache_port

    if is_p:
        # Private L2 behind a per-core bus.
        cpu.l2bus = L2XBar()
        cpu.l2 = PL2()
        cpu.icache.mem_side = cpu.l2bus.cpu_side_ports
        cpu.dcache.mem_side = cpu.l2bus.cpu_side_ports
        cpu.l2.cpu_side = cpu.l2bus.mem_side_ports
        cpu.l2.mem_side = system.l3bus.cpu_side_ports
    else:
        # E-core L1s go straight to their cluster's shared L2.
        cluster_bus = system.e_l2bus[(i - args.num_p) // 4]
        cpu.icache.mem_side = cluster_bus.cpu_side_ports
        cpu.dcache.mem_side = cluster_bus.cpu_side_ports

    cpu.createInterruptController()
    cpu.interrupts[0].pio = system.membus.mem_side_ports
    cpu.interrupts[0].int_requestor = system.membus.cpu_side_ports
    cpu.interrupts[0].int_responder = system.membus.mem_side_ports

    if args.max_insts:
        cpu.max_insts_any_thread = args.max_insts

system.mem_ctrl = MemCtrl()
system.mem_ctrl.dram = DDR4_2400_16x4(range=system.mem_ranges[0])
system.mem_ctrl.port = system.membus.mem_side_ports

system.system_port = system.membus.cpu_side_ports

# --------------------------------------------------------------------------
# Workload (single multithreaded SE process across all CPUs)
# --------------------------------------------------------------------------

system.workload = SEWorkload.init_compatible(args.cmd)

process = Process()
process.cmd = [args.cmd] + ([a for a in args.options.split(" ") if a] if args.options else [])
process.env = args.env
process.cwd = os.getcwd()

for cpu in system.cpu:
    cpu.workload = process
    cpu.createThreads()

root = Root(full_system=False, system=system)
m5.instantiate()

print(f"**** hetero system: {args.num_p}P @ {args.p_clock} + {args.num_e}E @ {args.e_clock}")
print(f"**** running: {process.cmd}  env={args.env}")
event = m5.simulate()
print(f"**** exited @ tick {m5.curTick()}: {event.getCause()}")
