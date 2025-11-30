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

get_pf() {
    local pid=$1
    if [ -f "/proc/$pid/stat" ]; then
        awk '{print $12}' "/proc/$pid/stat"
    else
        echo "0"
    fi
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
IFETCHER_MAX_TRIGGERS=${IFETCHER_MAX_TRIGGERS:-3} IFETCHER_PREFETCH_TOP_N=${IFETCHER_PREFETCH_TOP_N:-12} ./analyzer_tight >/dev/null
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
    PF_0=$(get_pf $GAME_PID)
    
    sleep 2.5 # Intro Wait
    
    PF_1=$(get_pf $GAME_PID)
    
    # （预取器持续运行，不再按 Event 切分）
    
    # Bot Action: Select Single Race -> Car
    WIN_ID=$(xdotool search --onlyvisible --class "$WINDOW_CLASS" | head -n 1)
    xdotool windowactivate --sync $WIN_ID key Return
    sleep 1.5
    xdotool windowactivate --sync $WIN_ID key Return
    sleep 1.0
    
    PF_2=$(get_pf $GAME_PID)
    TS_L_S=$(date +%s%N)
    xdotool windowactivate --sync $WIN_ID key Return
    sleep 6.0
    TS_L_E=$(date +%s%N)
    PF_S=$(get_pf $GAME_PID)
    
    # 结束
    pkill -9 -x "trigger-rally"
    wait $GAME_PID 2>/dev/null || true
    stop_virtual_display
    
    LOAD_PF=$((PF_S - PF_2))
    LOAD_T=$(awk -v s="$TS_L_S" -v e="$TS_L_E" 'BEGIN{print (e-s)/1000000000}')
    echo "$LOAD_PF,$LOAD_T" > "$outfile"
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

AVG_B_PF=$(calc_avg "base" 1); AVG_B_T=$(calc_avg "base" 2)
AVG_P_PF=$(calc_avg "pref" 1); AVG_P_T=$(calc_avg "pref" 2)

printf "%-20s | %-15s | %-15s | %-10s\n" "METRIC" "BASELINE" "PREFETCH" "IMPROVE"
echo "---------------------|-----------------|-----------------|----------"
impr_t=$(awk "BEGIN {if ($AVG_B_T>0) print ($AVG_B_T - $AVG_P_T)/$AVG_B_T*100; else print 0}")
printf "%-20s | %-15s | %-15s | %-6.2f%%\n" "Load Time (sec)" "$AVG_B_T" "$AVG_P_T" "$impr_t"
printf "%-20s | %-15s | %-15s | %-10s\n" "Load PF" "$AVG_B_PF" "$AVG_P_PF" "-"
