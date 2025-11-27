CC = gcc
CFLAGS = -Wall -fPIC -shared -pthread
LDFLAGS = -ldl

all: clean_logs libwrapper.so proc_monitor

basic: clean_logs libwrapper.so proc_monitor
	@echo "Profiler basic build done."

clean_logs:
	rm -f /tmp/read_log /tmp/mmap_log /tmp/stat_log

# 编译 libwrapper.so（预加载库）
libwrapper.so: libwrapper.c profiler_common.c
	$(CC) $(CFLAGS) -o $@ libwrapper.c profiler_common.c $(LDFLAGS)

# 编译 proc_monitor（独立监控程序）
proc_monitor: proc_monitor.c profiler_common.c
	gcc -Wall -pthread -o proc_monitor proc_monitor.c profiler_common.c maps_monitor.c diskstats.c

clean:
	rm -f libwrapper.so proc_monitor /tmp/read_log /tmp/mmap_log /tmp/stat_log