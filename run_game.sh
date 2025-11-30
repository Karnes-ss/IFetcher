#!/bin/bash
set -e

# ================= CONFIGURATION =================
SCRIPT_DIR="$(cd "$(dirname "$0")" 2>/dev/null && pwd || pwd)"
ROOT="${ROOT:-$SCRIPT_DIR}"

APP_BIN="$(which trigger-rally 2>/dev/null || echo /usr/games/trigger-rally)"
APP="${APP:-$APP_BIN}"
if command -v readlink >/dev/null 2>&1; then APP="$(readlink -f "$APP")"; fi

DATA_DIR="/usr/share/games/trigger-rally"

TRAIN_SEC=15
MAX_TIMEOUT=45

# Window detection settings
WINDOW_NAME_REGEX="[Tt]rigger"
WINDOW_CLASS="trigger-rally"

LOG_BASE="$ROOT/result_baseline.log"
LOG_PREFETCH="$ROOT/result_prefetch.log"
# =================================================

APP_NAME="$(basename "$APP")"

# 1. Check for xdotool
if ! command -v xdotool >/dev/null 2>&1; then
    echo "ERROR: 'xdotool' is not installed! Please run: sudo apt-get install xdotool"
    exit 1
fi

# 2. Cleanup Function
cleanup_env() {
    # Kill exact process name matches
    pkill -x "$APP_NAME" 2>/dev/null || true
    pkill -x "trigger-rally" 2>/dev/null || true
    # Kill prefetcher specifically
    pkill -f "prefetcher" 2>/dev/null || true
    rm -rf "$HOME/.local/share/trigger-rally" "$HOME/.config/trigger-rally" 2>/dev/null || true
}

# 3. Core Measurement Function
measure_execution() {
    local LOG_OUT="$1"
    shift
    local RUN_CMD="$@"

    mkdir -p "$(dirname "$LOG_OUT")"

    # === FIX APPLIED HERE ===
    # Added "|| true" at the end to prevent script from exiting when process is killed
    /usr/bin/time -v -o "$LOG_OUT" bash -c "
        $RUN_CMD &
        CMD_PID=\$!
        
        DETECTED=0
        for i in {1..450}; do
            if ! kill -0 \$CMD_PID 2>/dev/null; then 
                break
            fi
            
            # Check window
            if xdotool search --onlyvisible --name \"$WINDOW_NAME_REGEX\" >/dev/null 2>&1 || \
               xdotool search --onlyvisible --class \"$WINDOW_CLASS\" >/dev/null 2>&1; then
                
                DETECTED=1
                # Wait a tiny bit for render
                sleep 0.2
                
                # Force kill game
                kill -9 \$CMD_PID 2>/dev/null || true
                pkill -9 -x \"$APP_NAME\" 2>/dev/null || true
                pkill -9 -x \"trigger-rally\" 2>/dev/null || true
                break
            fi
            sleep 0.1
        done
        
        # Cleanup if loop ends
        kill -9 \$CMD_PID 2>/dev/null || true
        pkill -9 -x \"$APP_NAME\" 2>/dev/null || true
    " || true 
}

# ================= EXECUTION FLOW =================

echo "== Step 0: Global Cleanup & Build =="
cd "$ROOT"
rm -f "$LOG_BASE" "$LOG_PREFETCH"
sudo sh -c 'sync; echo 3 > /proc/sys/vm/drop_caches' 2>/dev/null || true

# Build
(cd profiler && make basic >/dev/null)
(cd analyzer && make analyzer_tight >/dev/null)
(cd prefetcher && make >/dev/null)
cleanup_env

echo "== Step 1: Profiling (Training Phase) =="
cd "$ROOT/profiler"
sudo sh -c 'sync; echo 3 > /proc/sys/vm/drop_caches' 2>/dev/null || true
(sleep "$TRAIN_SEC"; printf '\n') | ./proc_monitor --spawn "$APP" >/dev/null 2>&1 || true
cleanup_env

echo "== Step 2: Analyzing Logs =="
cd "$ROOT/analyzer"
IFETCHER_LOG_DIR=/tmp IFETCHER_ALLOW_MMAP_ONLY=1 IFETCHER_DATA_DIR="$DATA_DIR" IFETCHER_MAX_TRIGGERS=1 IFETCHER_PREFETCH_TOP_N=${IFETCHER_PREFETCH_TOP_N:-32} IFETCHER_MAX_PREFETCH_BYTES_KB=${IFETCHER_MAX_PREFETCH_BYTES_KB:-2048} IFETCHER_WINDOW_SEC=${IFETCHER_WINDOW_SEC:-10} IFETCHER_READ_THRESHOLD=${IFETCHER_READ_THRESHOLD:-512} IFETCHER_MIN_READS=${IFETCHER_MIN_READS:-1} IFETCHER_MIN_BYTES=${IFETCHER_MIN_BYTES:-8192} ./analyzer_tight >/dev/null
awk -F, '$1 !~ "^/(proc|sys|dev)/" {p=$1; cmd="[ -f \""p"\" ]"; if (system(cmd)==0) print}' trigger_log.txt > trigger_log.txt.tmp && mv trigger_log.txt.tmp trigger_log.txt
# Optional: select a specific single trigger segment by path
if [ -n "$SINGLE_TRIGGER_PATH" ]; then
  awk -F, -v sel="$SINGLE_TRIGGER_PATH" '
    BEGIN{keep=0}
    /^===TRIGGER===/ {if(keep){exit} getline; split($0,a,","); if(a[1]==sel){keep=1; print "===TRIGGER===" > "prefetch_log.txt.sel"; print $0 >> "prefetch_log.txt.sel"; next} }
    { if(keep) print >> "prefetch_log.txt.sel" }
  ' prefetch_log.txt
  if [ -s prefetch_log.txt.sel ]; then
    mv prefetch_log.txt.sel prefetch_log.txt
    echo "$SINGLE_TRIGGER_PATH,0,4096" > trigger_log.txt
  fi
fi
# Fallback: synthesize single trigger when analyzer produced none
if [ $(grep -cv '^APP=' trigger_log.txt 2>/dev/null || echo 0) -eq 0 ]; then
  awk 'BEGIN{found=0}
       /Type:MMAP/ {
         if(found) next;
         if (match($0, /File:[^|]*/)) {
           p=substr($0, RSTART+5, RLENGTH-5);
           if (p ~ /^\// && index(p, "/maps/")>0) {
             print p ",0,4096" > "trigger_log.txt";
             print "===TRIGGER===" > "prefetch_log.txt";
             print p ",0,4096" >> "prefetch_log.txt";
             found=1;
           }
         }
       }' /tmp/mmap_log
fi

echo "== Step 3: Measuring BASELINE =="
cd "$ROOT"
sudo sh -c 'sync; echo 3 > /proc/sys/vm/drop_caches' 2>/dev/null || true
cleanup_env
measure_execution "$LOG_BASE" "$APP"
echo "   -> Baseline Finished."

echo "== Step 4: Measuring PREFETCHER =="
cd "$ROOT/prefetcher"
sudo sh -c 'sync; echo 3 > /proc/sys/vm/drop_caches' 2>/dev/null || true
cleanup_env
PREF_CMD="env EVENT_LOOP_POLL_MS=100 PREFETCH_COOLDOWN_MS=0 PREFETCH_TOUCH_KB=1024 PREFETCH_CONCURRENCY=4 PREFETCH_READ_FULL=1 PREFETCH_INCLUDE_TRIGGER=1 PREFETCH_TOP_N=${PREFETCH_TOP_N:-24} ./prefetcher --trigger-log ../analyzer/trigger_log.txt --prefetch-log ../analyzer/prefetch_log.txt --app \"$APP\""
bash -c "$PREF_CMD" >/dev/null 2>&1 &
PREF_PID=$!
sleep 2.0
cd "$ROOT"
measure_execution "$LOG_PREFETCH" "$APP"
kill -TERM "$PREF_PID" 2>/dev/null || true
pkill -f "prefetcher" 2>/dev/null || true
echo "   -> Prefetcher Finished."

# ================= REPORTING =================
echo ""
echo "========================================================"
echo "                 PERFORMANCE SUMMARY"
echo "========================================================"

get_time_sec() {
    if [ ! -f "$1" ]; then echo "N/A"; return; fi
    raw=$(grep "Elapsed (wall clock) time" "$1" | awk '{print $NF}')
    echo "$raw" | awk -F: '{if(NF==2){print $1*60+$2}else{print $1}}'
}

get_val() {
    if [ ! -f "$1" ]; then echo "N/A"; return; fi
    # $2 是搜索关键词, $3 是冒号分隔后的第几列
    awk -F: "/$2/{print \$$3}" "$1" | tr -d ' ' | head -n1
}

# ... 在 Step 4 之后 ...

# 1. 获取时间
BASE_T=$(get_time_sec "$LOG_BASE")
PREF_T=$(get_time_sec "$LOG_PREFETCH")

# 2. 获取 Major Page Faults
BASE_PF=$(get_val "$LOG_BASE" "Major .* page faults" 2)
PREF_PF=$(get_val "$LOG_PREFETCH" "Major .* page faults" 2)

# 3. [新增] 获取 File System Inputs
BASE_FS=$(get_val "$LOG_BASE" "File system inputs" 2)
PREF_FS=$(get_val "$LOG_PREFETCH" "File system inputs" 2)

# --- 打印表格 ---
printf "%-20s | %-15s | %-15s\n" "Metric" "Baseline" "Prefetcher"
echo "---------------------|-----------------|-----------------"
printf "%-20s | %-15s | %-15s\n" "Load Time (sec)" "${BASE_T}s" "${PREF_T}s"
printf "%-20s | %-15s | %-15s\n" "Major Page Faults" "$BASE_PF" "$PREF_PF"
printf "%-20s | %-15s | %-15s\n" "File System Inputs" "$BASE_FS" "$PREF_FS" 
echo "----------------========================================"
if [[ "$BASE_T" != "N/A" ]] && [[ "$PREF_T" != "N/A" ]]; then
    SPEEDUP=$(awk "BEGIN {if ($BASE_T > 0) print ($BASE_T - $PREF_T) / $BASE_T * 100; else print 0}")
    printf ">>> SPEEDUP (Time): %.2f%%\n" "$SPEEDUP"
fi
echo "========================================================"
