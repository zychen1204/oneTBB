#!/usr/bin/env bash
# VM-side: launches the four production gem5 simulations concurrently
# (bodytrack/fluidanimate x stock/CAWS) on the 4P+8E hetero system.
#
# usage: vm_run_sims.sh <bt_particles> <bt_layers> <fl_frames> [poweroff]
#   e.g. vm_run_sims.sh 1000 5 5 poweroff     # full simsmall
set -euo pipefail

BT_PARTICLES=${1:?particles}
BT_LAYERS=${2:?layers}
FL_FRAMES=${3:?frames}
POWEROFF=${4:-no}

GEM5=$HOME/gem5/build/X86/gem5.opt
CFG=$HOME/hetero_pe_se.py
BIN=$HOME/static
RES=$HOME/results
BT_IN=$HOME/parsec-inputs/bodytrack/data-small/sequenceB_1
FL_IN=$HOME/parsec-inputs/fluidanimate/data-small/in_35K.fluid
NP=4; NE=8; THREADS=$((NP+NE))

mkdir -p "$RES"

launch() { # $1 name, $2 variant, $3 cmd, $4 options
    local out="$RES/$1_$2"
    mkdir -p "$out"
    local env_args=()
    if [[ $2 == caws ]]; then
        env_args=(--env TBB_HETERO_PCORES=0-$((NP-1)) --env TBB_HETERO_STATS=1)
    fi
    nohup "$GEM5" --outdir "$out" "$CFG" --num-p $NP --num-e $NE \
        --cmd "$3" --options "$4" "${env_args[@]}" > "$out/sim.log" 2>&1 &
    echo "launched $1_$2 (pid $!)"
}

# bodytrack args: <dataset> <cameras> <frames> <particles> <layers> <threadmodel:1=TBB> <threads>
BT_ARGS="$BT_IN 4 1 $BT_PARTICLES $BT_LAYERS 1 $THREADS"
# fluidanimate args: <threads(pow2)> <frames> <input> <output>
FL_ARGS_S="8 $FL_FRAMES $FL_IN $RES/fluidanimate_stock/out.fluid"
FL_ARGS_C="8 $FL_FRAMES $FL_IN $RES/fluidanimate_caws/out.fluid"

mkdir -p "$RES/fluidanimate_stock" "$RES/fluidanimate_caws"
launch bodytrack stock "$BIN/bin_stock/bodytrack" "$BT_ARGS"
launch bodytrack caws  "$BIN/bin_caws/bodytrack"  "$BT_ARGS"
launch fluidanimate stock "$BIN/bin_stock/fluidanimate" "$FL_ARGS_S"
launch fluidanimate caws  "$BIN/bin_caws/fluidanimate"  "$FL_ARGS_C"

# Waits for all four sims, prints the summary, then optionally powers off
# so an unattended overnight run does not keep billing.
nohup bash -c '
wait_pids=$(pgrep -f hetero_pe_se.py || true)
while pgrep -f hetero_pe_se.py > /dev/null; do sleep 60; done
{
  echo "==== gem5 simSeconds summary ($(date)) ===="
  for d in '"$RES"'/*/; do
    printf "%-24s " "$(basename "$d")"
    grep -m1 "^simSeconds" "$d/stats.txt" 2>/dev/null | awk "{print \$2}" || echo "NO_STATS"
  done
} > '"$RES"'/SUMMARY.txt 2>&1
'"$( [[ $POWEROFF == poweroff ]] && echo 'sudo poweroff' )"'
' > "$RES/watchdog.log" 2>&1 &
echo "watchdog started; summary will be at $RES/SUMMARY.txt"
