#!/bin/bash
set -e

ROOT="${ROOT:-/mnt/hgfs/OS_lab/IFetcher}"
PROFILE="${PROFILE:-/tmp/ifecther_profile}"
URL="${URL:-about:home}"
HEADLESS="${HEADLESS:-1}"
ITER="${ITER:-10}"

clean_profile() { rm -f "$PROFILE/sessionstore.jsonlz4" "$PROFILE/previous.jsonlz4" "$PROFILE/recovery.jsonlz4" 2>/dev/null || true; rm -rf "$PROFILE/sessionstore-backups" 2>/dev/null || true; }
kill_firefox() { pkill -f firefox 2>/dev/null || true; }

echo "== Step00: Build & Global Clean =="
cd "$ROOT"
rm -f /tmp/read_log /tmp/mmap_log /tmp/stat_log
rm -f analyzer/trigger_log.txt analyzer/prefetch_log.txt
sudo sh -c 'sync; echo 3 > /proc/sys/vm/drop_caches' 2>/dev/null || true
cd profiler && make basic && cd ../analyzer && make analyzer_tight && cd ../prefetcher && make
mkdir -p "$PROFILE"
kill_firefox
clean_profile

echo "== Step01: Profile (Training, cold cache, 10s) =="
cd "$ROOT/profiler"
sudo sh -c 'sync; echo 3 > /proc/sys/vm/drop_caches' 2>/dev/null || true
# 修改为弹窗训练：使用图形模式而不是headless模式
(sleep 10; printf '\n') | ./proc_monitor --spawn /usr/bin/firefox --no-remote --profile "$PROFILE" "$URL"

kill_firefox
clean_profile

echo "== Step02: Analyze (tight analyzer) =="
cd "$ROOT/analyzer"
./analyzer_tight
awk -F, '$1 !~ "^/(proc|sys|dev)/" {p=$1; cmd="[ -f \""p"\" ]"; if (system(cmd)==0) print}' trigger_log.txt > trigger_log.txt.tmp && mv trigger_log.txt.tmp trigger_log.txt

echo "== Step03: Measure (cold cache, $ITER iterations; interleaving baseline & prefetch) =="
cd "$ROOT"
for i in $(seq 1 "$ITER"); do
  echo "-- Iteration $i: Baseline --"
  sudo sh -c 'sync; echo 3 > /proc/sys/vm/drop_caches' 2>/dev/null || true
  kill_firefox; clean_profile
  if [ "$HEADLESS" = "1" ]; then
    /usr/bin/time -v -o "$ROOT/time_base_$i.log" /usr/bin/firefox --headless --screenshot "$PROFILE/shot_base_$i.png" --no-remote --profile "$PROFILE" "$URL" || true
  else
    /usr/bin/time -v -o "$ROOT/time_base_$i.log" timeout 60 /usr/bin/firefox --no-remote --profile "$PROFILE" "$URL" || true
  fi

  echo "-- Iteration $i: Prefetch --"
  sudo sh -c 'sync; echo 3 > /proc/sys/vm/drop_caches' 2>/dev/null || true
  cd "$ROOT/prefetcher"
  rm -f time_summary.log "prefetcher_run_$i.log" 2>/dev/null || true
  kill_firefox; clean_profile
  if [ "$HEADLESS" = "1" ]; then
    ./prefetcher --app /usr/bin/firefox -- --headless --screenshot "$PROFILE/shot_prefetch_$i.png" --no-remote --profile "$PROFILE" "$URL" > "$ROOT/prefetcher_run_$i.log" 2>&1 || true
  else
    ./prefetcher --app /usr/bin/firefox -- --no-remote --profile "$PROFILE" "$URL" > "$ROOT/prefetcher_run_$i.log" 2>&1 || true
  fi
  if [ -f "$ROOT/prefetcher/time_summary.log" ]; then cp "$ROOT/prefetcher/time_summary.log" "$ROOT/time_prefetch_$i.log"; fi
  cd "$ROOT"
done

echo "== Step04: Summary (Averages & Variance) =="
if ls "$ROOT"/time_base_*.log >/dev/null 2>&1; then
  awk '/Elapsed/{split($NF,t,":");sec=t[1]*60+t[2];s+=sec;n++}END{printf("Baseline Avg Elapsed: %.2fs\n",s/n)}' "$ROOT"/time_base_*.log
  awk '/Major \(requiring I\/O\) page faults:/{s+=$NF;n++}END{print "Baseline Avg Major PF:",s/n}' "$ROOT"/time_base_*.log
  awk '/File system inputs:/{s+=$NF;n++}END{print "Baseline Avg FS inputs:",s/n}' "$ROOT"/time_base_*.log
  awk '/Elapsed/{split($NF,t,":");sec=t[1]*60+t[2];a[++n]=sec;s+=sec}END{m=s/n;for(i=1;i<=n;i++)v+=(a[i]-m)^2;print "Baseline Var Elapsed:",v/n}' "$ROOT"/time_base_*.log
fi
if ls "$ROOT"/time_prefetch_*.log >/dev/null 2>&1; then
  awk '/Elapsed/{split($NF,t,":");sec=t[1]*60+t[2];s+=sec;n++}END{printf("Prefetch Avg Elapsed: %.2fs\n",s/n)}' "$ROOT"/time_prefetch_*.log
  awk '/Major \(requiring I\/O\) page faults:/{s+=$NF;n++}END{print "Prefetch Avg Major PF:",s/n}' "$ROOT"/time_prefetch_*.log
  awk '/File system inputs:/{s+=$NF;n++}END{print "Prefetch Avg FS inputs:",s/n}' "$ROOT"/time_prefetch_*.log
  awk '/Elapsed/{split($NF,t,":");sec=t[1]*60+t[2];a[++n]=sec;s+=sec}END{m=s/n;for(i=1;i<=n;i++)v+=(a[i]-m)^2;print "Prefetch Var Elapsed:",v/n}' "$ROOT"/time_prefetch_*.log
fi

baseline_avg=$(awk '/Elapsed/{split($NF,t,":");sec=t[1]*60+t[2];s+=sec;n++}END{if(n) printf("%.2f", s/n)}' "$ROOT"/time_base_*.log 2>/dev/null)
prefetch_avg=$(awk '/Elapsed/{split($NF,t,":");sec=t[1]*60+t[2];s+=sec;n++}END{if(n) printf("%.2f", s/n)}' "$ROOT"/time_prefetch_*.log 2>/dev/null)
if [ -n "$baseline_avg" ] && [ -n "$prefetch_avg" ]; then
  awk -v b="$baseline_avg" -v p="$prefetch_avg" 'BEGIN{if (b>0) printf("Prefetch Speedup vs Baseline: %.1f%%\n", (b-p)/b*100); else print "Prefetch Speedup vs Baseline: N/A"}'
fi
echo "== Done =="