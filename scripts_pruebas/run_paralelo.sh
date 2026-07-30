#!/bin/bash
# Corre las 7 pruebas oficiales EN PARALELO, cada una aislada en su sandbox:
# - rango de puertos propio por prueba (BASE_PORT + slot*20)
# - copia de binarios y configs generadas -> logs y swap.bin aislados
# Resultado en $PAR/<prueba>/  (logs de todos los modulos)
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SCRATCH="$REPO/scripts_pruebas"
PRUEBAS=$REPO/pruebas_oficiales/plug-n-pray-pruebas
PAR=$SCRATCH/paralelo
rm -rf "$PAR"
mkdir -p "$PAR"
export LIBRARY_PATH=/tmp/rl

# ---------- armado de un sandbox ----------
# args: nombre slot script duracion "alg quantum preempt susp_timeout" "queues" seg_max strategy instr_delay "sticks: tam tam..." [stdin_file]
preparar_y_correr() {
    local nombre=$1 slot=$2 script=$3 dur=$4
    local ks_alg=$5 ks_queues=$6 ks_quantum=$7 ks_susp=$8
    local seg_max=$9 strategy=${10} instr_delay=${11} sticks=(${12}) stdin_file=${13}

    local P=$((9000 + slot * 20))          # rango de puertos del sandbox
    local KS_PORT=$P KM_PORT=$((P+2))
    local D=$PAR/$nombre

    for m in kernel_scheduler kernel_memory memory_stick cpu io swap; do
        mkdir -p "$D/$m/bin" "$D/$m/config" "$D/$m/logs"
        cp "$REPO/$m/bin/"* "$D/$m/bin/"
    done

    cat > "$D/kernel_scheduler/config/ks.cfg" << EOF
LOG_LEVEL=INFO
PLANIFICATION_ALGORITHM=$ks_alg
QUEUES_ALGORITHMS=$ks_queues
RR_QUANTUM=$ks_quantum
QUEUE_PREEMPTION=TRUE
SUSPENSION_TIMEOUT=$ks_susp
LISTEN_IP=0.0.0.0
LISTEN_PORT=$KS_PORT
IP_MEMORY=127.0.0.1
PORT_MEMORY=$KM_PORT
EOF
    cat > "$D/kernel_memory/config/km.cfg" << EOF
LOG_LEVEL=INFO
SEGMENT_MAX_SIZE=$seg_max
ALLOCATION_STRATEGY=$strategy
INSTRUCTION_DELAY=$instr_delay
COMPACTION_DELAY=30000
SCRIPTS_BASEPATH=$PRUEBAS
LISTEN_IP=0.0.0.0
LISTEN_PORT=$KM_PORT
EOF
    cat > "$D/cpu/config/cpu.cfg" << EOF
LOG_LEVEL=INFO
IP_KERNEL_SCHEDULER=127.0.0.1
PORT_KERNEL_SCHEDULER=$KS_PORT
IP_KERNEL_MEMORY=127.0.0.1
PORT_KERNEL_MEMORY=$KM_PORT
EOF
    cat > "$D/io/config/io.cfg" << EOF
LOG_LEVEL=INFO
IP_KERNEL_SCHEDULER=127.0.0.1
PORT_KERNEL_SCHEDULER=$KS_PORT
EOF
    cat > "$D/swap/config/swap.cfg" << EOF
LOG_LEVEL=INFO
SWAP_FILE_PATH=./swap.bin
SWAP_FILE_SIZE=1048576
BLOCK_SIZE=4096
IP_KERNEL_MEMORY=127.0.0.1
PORT_KERNEL_MEMORY=$KM_PORT
EOF

    # --- arranque (mismo orden que la guia de deploy) ---
    cd "$D/kernel_memory" && setsid "$D/kernel_memory/bin/kernel_memory" config/km.cfg > /dev/null 2>&1 &
    sleep 0.7
    local i=0
    for tam in "${sticks[@]}"; do
        local SP=$((P + 3 + i))
        cat > "$D/memory_stick/config/stick_$i.cfg" << EOF
LOG_LEVEL=INFO
MEMORY_DELAY=1500
LISTEN_IP=127.0.0.1
LISTEN_PORT=$SP
IP_KERNEL_MEMORY=127.0.0.1
PORT_KERNEL_MEMORY=$KM_PORT
EOF
        # cada stick con su propio dir de logs para no pisarse entre si
        mkdir -p "$D/memory_stick/s$i/bin" "$D/memory_stick/s$i/logs" "$D/memory_stick/s$i/config"
        cp "$D/memory_stick/bin/"* "$D/memory_stick/s$i/bin/"
        cp "$D/memory_stick/config/stick_$i.cfg" "$D/memory_stick/s$i/config/"
        cd "$D/memory_stick/s$i" && setsid "$D/memory_stick/s$i/bin/memory_stick" config/stick_$i.cfg "$tam" > /dev/null 2>&1 &
        sleep 0.3
        i=$((i+1))
    done
    cd "$D/swap" && setsid "$D/swap/bin/swap" config/swap.cfg > /dev/null 2>&1 &
    sleep 0.3
    cd "$D/kernel_scheduler" && setsid "$D/kernel_scheduler/bin/kernel_scheduler" config/ks.cfg "$script" > /dev/null 2>&1 &
    sleep 0.6
    cd "$D/io" || exit 1
    setsid "$D/io/bin/io" config/io.cfg SLEEP  > /dev/null 2>&1 &
    setsid "$D/io/bin/io" config/io.cfg STDOUT > /dev/null 2>&1 &
    if [ -n "$stdin_file" ]; then
        setsid "$D/io/bin/io" config/io.cfg STDIN < "$stdin_file" > /dev/null 2>&1 &
    else
        setsid "$D/io/bin/io" config/io.cfg STDIN < /dev/null > /dev/null 2>&1 &
    fi
    sleep 0.3
    cd "$D/cpu" && setsid "$D/cpu/bin/cpu" config/cpu.cfg 1 > /dev/null 2>&1 &

    sleep "$dur"
    # matar SOLO los procesos de este sandbox (el path del sandbox esta en su cmdline)
    pkill -f "$D" 2>/dev/null
    echo ">>> [$(date +%H:%M:%S)] $nombre terminada (${dur}s)"
}

echo ">>> [$(date +%H:%M:%S)] Lanzando las 7 pruebas en paralelo..."

#                 nombre     slot script             dur  alg  queues                          quantum susp     segmax strat instr sticks           stdin
preparar_y_correr base1      0 PLANI_PRE_0.prc   215 CMN "[FIFO,RR,FIFO,RR]"             600  60000   128 BEST  250 "256" "" &
preparar_y_correr base3      1 MEMORIA_PRE_0.prc 150 CMN "[FIFO,RR,FIFO,RR]"             600  60000   128 BEST  250 "256" $SCRATCH/stdin_input.txt &
preparar_y_correr pcp        2 PCP.prc           120 CMN "[FIFO,RR,RR,RR]"               1500 35000   256 BEST  500 "256" "" &
preparar_y_correr mem_best   3 PLANI_MEM.prc     110 RR  "[FIFO]"                        1500 35000   128 BEST  500 "16 32 64 128" "" &
preparar_y_correr mem_worst  4 PLANI_MEM.prc     180 RR  "[FIFO]"                        1500 35000   128 WORST 500 "16 32 64 128" "" &
preparar_y_correr php        5 PHP.prc           420 CMN "[FIFO,FIFO,FIFO,FIFO,FIFO,FIFO]" 1500 1000000 128 BEST 500 "16 16" "" &
preparar_y_correr pmp        6 PMP.prc           210 CMN "[FIFO,FIFO,FIFO,FIFO]"         1500 10000   128 BEST  500 "16 16 32 64" $SCRATCH/stdin_pmp.txt &

wait
echo "PARALELO_COMPLETO [$(date +%H:%M:%S)]"
