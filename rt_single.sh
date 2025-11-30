#!/bin/bash
set -e
export LC_ALL=C
export LIBGL_ALWAYS_SOFTWARE=1
export SDL_VIDEODRIVER=x11
export SDL_AUDIODRIVER=dummy

# ================= 配置区域 =================
APP_BIN="/usr/games/trigger-rally"
APP="${APP:-$APP_BIN}"
DATA_DIR="/usr/share/games/trigger-rally"
WINDOW_CLASS="trigger-rally"

# 日志路径 (使用绝对路径，防止找不到)
ROOT_DIR="$(pwd)"
RAW_LOG="$ROOT_DIR/trigger_full.log"
RESULT_DIR="$ROOT_DIR/results_runtime"

# 测试参数
TEST_ROUNDS=1           # 测试轮数
PREFETCH_BATCH_SIZE=20  # 每次预取文件数 (避免IO拥堵)

# ================= 工具函数 =================

cleanup_env() {
    pkill -9 -x "trigger-rally" 2>/dev/null || true
    pkill -9 -f "prefetcher" 2>/dev/null || true
    rm -rf "$HOME/.local/share/trigger-rally" 2>/dev/null || true
    
    # 清理缓存 (需要 sudo，非交互式环境可能跳过)
    if [ "$EUID" -eq 0 ]; then
        sync; echo 3 > /proc/sys/vm/drop_caches
    else
        sudo sh -c 'sync; echo 3 > /proc/sys/vm/drop_caches' 2>/dev/null || true
    fi
    sleep 1
}

start_virtual_display() {
    if [ -z "${DISPLAY:-}" ] && command -v Xvfb >/dev/null 2>&1; then
        for d in 99 98 97; do
            Xvfb ":$d" -screen 0 1280x800x24 >/dev/null 2>&1 &
            XVFB_PID=$!
            export DISPLAY=":$d"
            sleep 0.5
            break
        done
    fi
}

stop_virtual_display() {
    if [ -n "${XVFB_PID:-}" ]; then
        kill -TERM "$XVFB_PID" 2>/dev/null || true
        unset XVFB_PID
        unset DISPLAY
    fi
}


# 读取 /tmp/read_log 并用 I/O 间隙检测计算地图加载时间
calc_smart_load_time() {
  local READ_LOG="/tmp/read_log"
  local START_TS_FILE="$ROOT_DIR/.drive_start_ts"
  if [[ ! -f "$READ_LOG" || ! -f "$START_TS_FILE" ]]; then echo "0.0000"; return; fi
  local TRIG_TS; TRIG_TS=$(cat "$START_TS_FILE")
  gawk -v TRIG_TS="$TRIG_TS" '
    function get_epoch_float(ts){
      if (match(ts,/^[0-9]+\.[0-9]+$/)) return ts+0;
      if (match(ts,/[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}/)) {
        d=substr(ts,RSTART,RLENGTH); gsub(/[- :]/," ",d); s=mktime(d);
        if (match(ts,/\.[0-9]+/)) { f=substr(ts,RSTART,RLENGTH)+0.0; return s+f }
        return s
      }
      return 0
    }
    function get_ts(line){ if (match(line,/^\[[^]]+\]/)) return substr(line,RSTART+1,RLENGTH-2); return "" }
    BEGIN{start_t=0; last_t=0; end_t=0; io_count=0; GAP=0.5}
    {
      if ($0!~/Type:(READ|FREAD|MMAP)/) next;
      if ($0!~/\.(png|jpg|xml|level|wav|ogg|tga|texture)/) next;
      t=get_ts($0); ct=get_epoch_float(t);
      if (ct<=0) next; if (ct<TRIG_TS) next; if (ct>TRIG_TS+15.0) next;
      if (start_t==0){ start_t=ct; last_t=ct; end_t=ct; io_count++; next }
      diff=ct-last_t; if (diff>GAP && io_count>5) { exit }
      last_t=ct; end_t=ct; io_count++
    }
    END{
      if (start_t>0 && end_t>=start_t){ d=end_t-start_t; if (d<0.01) d=0.01; printf("%.4f",d) }
      else { print "0.0000" }
    }
  ' "$READ_LOG"
}



wait_for_window() {
    echo "   [Bot] Waiting for window..."
    for i in {1..300}; do
        if xdotool search --onlyvisible --class "$WINDOW_CLASS" >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.1
    done
    return 1
}

# 核心：机器人逻辑 (菜单 -> 选图 -> 开车)
run_game_bot() {
    if ! wait_for_window; then
        echo "   [Bot] Error: Window never appeared."
        return 1
    fi
    
    # 获取窗口ID以便发送按键
    WIN_ID=$(xdotool search --onlyvisible --class "$WINDOW_CLASS" | head -n 1)
    
    # 1. 启动阶段 (等待 Intro 播放)
    sleep 3.0
    
    # 2. 菜单阶段
    # 主菜单默认第一项通常是 "Single Race"，直接回车
    echo "   [Bot] Menu: Single Race"
    xdotool windowactivate --sync $WIN_ID key Return
    sleep 1.5
    
    # 选车界面：默认第一辆车，直接回车
    echo "   [Bot] Menu: Select Car"
    xdotool windowactivate --sync $WIN_ID key Return
    sleep 1.0
    
    # 选图界面：默认第一张地图，直接回车 -> 触发加载
    echo "   [Bot] Menu: Select Map (Start Loading)"
    xdotool windowactivate --sync $WIN_ID key Return
    
    # 3. 地图加载阶段
    # 等待加载完成 (根据机器性能调整，这里给6秒)
    echo "   [Bot] Loading Map..."
    sleep 6.0
    
    # 4. 自动驾驶阶段
    echo "   [Bot] DRIVING! (Holding Throttle)"
    # 按住上箭头 (油门) 5秒
    xdotool windowactivate --sync $WIN_ID keydown Up
    sleep 5.0
    xdotool windowactivate --sync $WIN_ID keyup Up
    
    echo "   [Bot] Session finished."
    pkill -x "trigger-rally"
}

# ================= 阶段 0: 清理与编译 (Clean & Build) =================
echo "===================================================="
echo " PHASE 0: Clean & Build"
echo "===================================================="
cleanup_env
make -C profiler >/dev/null 2>&1 || make -C profiler
make -C analyzer >/dev/null 2>&1 || make -C analyzer
make -C prefetcher >/dev/null 2>&1 || make -C prefetcher

# ================= 阶段 1: 录制全流程 (Profiling) =================
echo "==================================================="
echo " PHASE 1: Profiling (Recording Full Session)"
echo "==================================================="

# 检查 profiler 是否存在
if [ ! -f "profiler/proc_monitor" ]; then
    echo "Error: profiler/proc_monitor not found. Please compile it first."
    exit 1
fi

cleanup_env


cd profiler
# 启动 monitor，输出到绝对路径
start_virtual_display
./proc_monitor --spawn "$APP" >/dev/null 2>&1 &
MON_PID=$!
cd ..

# 运行机器人
run_game_bot &
BOT_PID=$!

wait $BOT_PID || true
# 确保 Monitor 有时间写入文件
sleep 2
kill $MON_PID 2>/dev/null || true
stop_virtual_display
echo "   -> Profiling finished."

# ================= 阶段 2: 生成事件列表 =================
echo ""
echo " PHASE 2: Analyzer (Triggers & Prefetch Lists)"
echo "==================================================="
cd analyzer
rm -f trigger_log.txt prefetch_log.txt
IFETCHER_LOG_DIR=/tmp \
IFETCHER_ALLOW_MMAP_ONLY=1 \
IFETCHER_DATA_DIR="$DATA_DIR" \
IFETCHER_MAX_TRIGGERS=${IFETCHER_MAX_TRIGGERS:-3} \
IFETCHER_PREFETCH_TOP_N=${IFETCHER_PREFETCH_TOP_N:-12} \
IFETCHER_WINDOW_SEC=${IFETCHER_WINDOW_SEC:-5} \
IFETCHER_READ_THRESHOLD=${IFETCHER_READ_THRESHOLD:-1024} \
IFETCHER_MIN_READS=${IFETCHER_MIN_READS:-1} \
IFETCHER_MIN_BYTES=${IFETCHER_MIN_BYTES:-16384} \
./analyzer_tight >/dev/null
awk -F, '$1 !~ "^/(proc|sys|dev)/" {p=$1; cmd="[ -f \""p"\" ]"; if (system(cmd)==0) print}' trigger_log.txt > trigger_log.txt.tmp && mv trigger_log.txt.tmp trigger_log.txt
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
trig_cnt=$(grep -cv '^APP=' trigger_log.txt 2>/dev/null || echo 0)
seg_cnt=$(grep -c '^===TRIGGER===' prefetch_log.txt 2>/dev/null || echo 0)
echo "   -> Analyzer triggers: $trig_cnt segments: $seg_cnt"
cd ..

# ================= 阶段 3: 循环测试 (Benchmarking) =================

run_test_round() {
    local mode="$1"  # "base" or "pref"
    local round="$2"
    local outfile="$RESULT_DIR/${mode}_r${round}.csv"
    mkdir -p "$RESULT_DIR"
    
    if [ "$mode" = "base" ]; then
        echo "   Running BASELINE..."
    else
        echo "   Running PREFETCH..."
    fi
    cleanup_env
    start_virtual_display
    
    # 启动游戏
    $APP >/dev/null 2>&1 &
    GAME_PID=$!
    
    if [ "$mode" == "pref" ]; then
        (cd prefetcher && env PREFETCH_CONCURRENCY=1 IFETCHER_PREFETCH_TOP_N=${PREFETCH_BATCH_SIZE} ionice -c 2 -n 7 nice -n 10 ./prefetcher --trigger-log ../analyzer/trigger_log.txt --prefetch-log ../analyzer/prefetch_log.txt --app "$APP") >/dev/null 2>&1 &
    fi
    
    wait_for_window
    rm -f /tmp/read_log /tmp/mmap_log /tmp/stat_log
    (cd profiler && IFETCHER_LOG_DIR=/tmp IFETCHER_MONITOR_INTERVAL_MS=50 ./proc_monitor "$GAME_PID" >/dev/null 2>&1) &
    MON_PID=$!
    
    # Bot Action: Select Single Race -> Car
    WIN_ID=$(xdotool search --onlyvisible --class "$WINDOW_CLASS" | head -n 1)
    xdotool windowactivate --sync $WIN_ID key Return
    sleep 1.5
    xdotool windowactivate --sync $WIN_ID key Return
    sleep 1.0
    
    # 触发加载
    date +%s.%N > "$ROOT_DIR/.drive_start_ts"
    xdotool windowactivate --sync $WIN_ID key Return
    
    # 采集 I/O 一小段时间
    sleep 12.0
    LOAD_T=$(calc_smart_load_time)
    
    # 结束
    kill -TERM "$MON_PID" 2>/dev/null || true
    wait "$MON_PID" 2>/dev/null || true
    pkill -9 -x "trigger-rally"
    wait $GAME_PID 2>/dev/null || true
    stop_virtual_display
    
    echo "$LOAD_T" > "$outfile"
}

echo ""
echo " PHASE 3: Benchmarking ($TEST_ROUNDS Rounds)"
echo "==================================================="

for i in $(seq 1 $TEST_ROUNDS); do
    run_test_round "base" "$i"
    run_test_round "pref" "$i"
done

# ================= 阶段 4: 统计报告 =================
echo ""
echo " PHASE 4: Final Report"
echo "==================================================="

calc_avg() {
    local mode="$1"
    local col="$2"
    local file_pattern="$RESULT_DIR/${mode}_r*.csv"
    if ls $file_pattern 1> /dev/null 2>&1; then
        cat $file_pattern | awk -F, -v col="$col" '{sum+=$col} END {if (NR>0) printf "%.1f", sum/NR; else print "0"}'
    else
        echo "0"
    fi
}

AVG_B_T=$(calc_avg "base" 1)
AVG_P_T=$(calc_avg "pref" 1)

printf "%-20s | %-15s | %-15s | %-10s\n" "METRIC" "BASELINE" "PREFETCH" "IMPROVE"
echo "---------------------|-----------------|-----------------|----------"
impr_t=$(awk "BEGIN {if ($AVG_B_T>0) print ($AVG_B_T - $AVG_P_T)/$AVG_B_T*100; else print 0}")
printf "%-20s | %-15s | %-15s | %-6.2f%%\n" "Load Time (sec)" "$AVG_B_T" "$AVG_P_T" "$impr_t"
