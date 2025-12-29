#!/bin/bash
# Do not exit immediately on error
set +e 

# ================= Configuration =================
APP_BIN="$(which trigger-rally 2>/dev/null || echo /usr/games/trigger-rally)"
APP="${APP:-$APP_BIN}"
RESULT_DIR="$(cd "$(dirname "$0")" && pwd)/results"
mkdir -p "$RESULT_DIR"
ITERATIONS=3
PREFETCH_BATCH=32
export LC_ALL=C

# ================= Utilities =================

cleanup_env() {
    echo "   [System] Cleaning up RAM & Caches..."
    pkill -9 -x "trigger-rally" 2>/dev/null
    pkill -9 -f "prefetcher" 2>/dev/null
    sudo -v 
    sync
    sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches'
    sleep 1.0
}

# 强力清理输入缓冲区
flush_input() {
    # 循环读取，直到0.1秒内没有任何输入为止
    while read -r -t 0.1 -n 1000; do :; done
}

# ================= Main Workflow =================

echo "=== Trigger Rally Manual Benchmark (Final) ==="
echo "App: $APP"

# Sudo keep-alive
sudo -v
( while true; do sudo -v; sleep 60; done; ) &
SUDO_PID=$!
trap "kill $SUDO_PID 2>/dev/null" EXIT

# --- Step 0: Check Builds ---
echo -e "\n[Step 0] Checking builds..."
(cd profiler && make basic >/dev/null)
(cd analyzer && make analyzer_tight >/dev/null)
(cd prefetcher && make >/dev/null)

# --- Step 1: Profiling ---
echo -e "\n[Step 1] Profiling (Training)..."
cleanup_env
echo ">>> Ready to record Trace <<<"
flush_input
echo "Press ENTER to launch game for TRAINING. Play once, then close."
read -r < /dev/tty

cd profiler
./proc_monitor --spawn "$APP" >/dev/null 2>&1 &
MON_PID=$!
cd ..

sleep 2
while pgrep -f "trigger-rally" > /dev/null; do sleep 1; done
kill -TERM "$MON_PID" 2>/dev/null || true
wait "$MON_PID" 2>/dev/null || true

# --- Step 2: Analyze ---
echo -e "\n[Step 2] Analyzing Logs..."
cd analyzer
rm -f trigger_log.txt prefetch_log.txt
IFETCHER_LOG_DIR=/tmp IFETCHER_DATA_DIR="/" IFETCHER_PREFETCH_TOP_N=50 \
./analyzer_tight > analyzer_debug.log 2>&1
cd ..

# --- Step 3: Benchmark ---
echo -e "\n[Step 3] Running Benchmarks..."
SUMMARY_FILE="$RESULT_DIR/summary.csv"
echo "Type,Run,Time,PageFaults" > "$SUMMARY_FILE"

run_manual_round() {
    local TYPE="$1"
    local RUN_IDX="$2"
    local PREF_CMD="$3"

    echo ""
    echo "=================================================="
    echo "   Running: $TYPE (Round $RUN_IDX / $ITERATIONS)"
    echo "=================================================="
    
    sudo -v 
    cleanup_env

    echo "--------------------------------------------------"
    echo "   WAITING FOR USER..."
    echo "--------------------------------------------------"
    
    # 1. 先清理键盘缓存
    flush_input
    
    # 2. 等待用户按回车
    echo ">>> READY. Press ENTER to LAUNCH EVERYTHING. <<<"
    read -r < /dev/tty
    
    # ================= 启动区 =================
    
    local P_PID=""
    
    # A. 如果有预取器，现在立即启动
    if [ -n "$PREF_CMD" ]; then
        echo "   -> [Go] Launching Prefetcher..."
        bash -c "$PREF_CMD" >/dev/null 2>&1 &
        P_PID=$!
        sleep 0.1 
    fi

    # B. 立即启动游戏
    echo "   -> [Go] Launching Game..."
    LOG_FILE="$RESULT_DIR/${TYPE}_${RUN_IDX}.log"
    /usr/bin/time -v -o "$LOG_FILE" "$APP" >/dev/null 2>&1 &
    local TIME_PID=$!
    
    # =========================================

    # 等待游戏窗口出现
    sleep 2
    # 循环检查游戏是否还在运行 (Wait for the time wrapper process to exit)
    while kill -0 "$TIME_PID" 2>/dev/null; do sleep 0.2; done
    
    echo "   (Game closed)"

    # 游戏结束后，杀掉预取器
    if [ -n "$P_PID" ]; then 
        kill -9 "$P_PID" 2>/dev/null || true
    fi
    wait "$TIME_PID" 2>/dev/null || true
    
    # 记录数据
    if [ -f "$LOG_FILE" ]; then
        ELAPSED=$(grep "Elapsed" "$LOG_FILE" | awk '{print $NF}' | awk -F: '{if(NF==2)print $1*60+$2; else print $1}')
        FAULTS=$(grep "Major .* page faults" "$LOG_FILE" | awk -F: '{print $2}' | tr -d ' ')
        echo "   -> Result: ${ELAPSED}s | ${FAULTS} Page Faults"
        echo "$TYPE,$RUN_IDX,$ELAPSED,$FAULTS" >> "$SUMMARY_FILE"
    fi
}
# === 注意：这里必须要有右大括号结束函数，之前的错误就是缺了这个 ===

for ((i=1; i<=ITERATIONS; i++)); do
    # 1. Base (无预取)
    run_manual_round "Base" "$i" ""
    
    # 2. Prefetch (有预取)
    P_CMD="cd prefetcher && env PREFETCH_CONCURRENCY=4 IFETCHER_PREFETCH_TOP_N=$PREFETCH_BATCH ./prefetcher --trigger-log ../analyzer/trigger_log.txt --prefetch-log ../analyzer/prefetch_log.txt --app \"$APP\""
    run_manual_round "Prefetch" "$i" "$P_CMD"
done

# ================= 报告结果 =================
echo -e "\n========================================================"
echo "                 FINAL RESULTS"
echo "========================================================"
if [ -f "$SUMMARY_FILE" ]; then
    awk -F, '
    BEGIN { b_t=0; b_f=0; b_n=0; p_t=0; p_f=0; p_n=0 }
    NR>1 {
        if ($1=="Base") { b_t+=$3; b_f+=$4; b_n++ }
        if ($1=="Prefetch") { p_t+=$3; p_f+=$4; p_n++ }
    }
    END {
        if (b_n==0 || p_n==0) exit;
        avg_b_t = b_t/b_n; avg_b_f = b_f/b_n;
        avg_p_t = p_t/p_n; avg_p_f = p_f/p_n;
        
        printf "%-15s | %-12s | %-12s | %s\n", "Metric", "Baseline", "Prefetch", "Improvement"
        print "----------------|--------------|--------------|-------------"
        
        diff_t = (avg_b_t - avg_p_t) / avg_b_t * 100
        printf "%-15s | %-11.2fs | %-11.2fs | %+.2f%%\n", "Time", avg_b_t, avg_p_t, diff_t
        
        diff_f = (avg_b_f - avg_p_f) / avg_b_f * 100
        printf "%-15s | %-12d | %-12d | %+.2f%%\n", "Page Faults", avg_b_f, avg_p_f, diff_f
    }
    ' "$SUMMARY_FILE"
fi
echo "========================================================"