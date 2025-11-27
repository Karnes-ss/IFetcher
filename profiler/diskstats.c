#include "diskstats.h"
#include "profiler_common.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

void monitor_disk_stats(void) {
    static unsigned long long last_reads[256] = {0};
    static unsigned long long last_sectors[256] = {0};
    static unsigned long long last_read_time_ms[256] = {0};
    static unsigned long long last_writes[256] = {0};
    static unsigned long long last_sectors_written[256] = {0};
    static unsigned long long last_write_time_ms[256] = {0};
    static unsigned long long last_io_time_ms[256] = {0};

    char line[1024];
    FILE* fp = fopen("/proc/diskstats", "r");
    if (fp == NULL) {
        perror("Failed to open /proc/diskstats");
        return;
    }

    int disk_idx = 0;
    while (fgets(line, sizeof(line), fp) != NULL && disk_idx < 256) {
        unsigned int major, minor;
        char dev_name[64];
        unsigned long long reads, rmerge, sectors_read, read_time_ms;
        unsigned long long writes, wmerge, sectors_written, write_time_ms;
        unsigned long long in_flight, io_time_ms, weighted_io_time_ms;

        int parsed = sscanf(line,
                            "%u %u %63s "
                            "%llu %llu %llu %llu "
                            "%llu %llu %llu %llu "
                            "%llu %llu %llu",
                            &major, &minor, dev_name,
                            &reads, &rmerge, &sectors_read, &read_time_ms,
                            &writes, &wmerge, &sectors_written, &write_time_ms,
                            &in_flight, &io_time_ms, &weighted_io_time_ms);
        if (parsed >= 14) {
            int is_nvme = strncmp(dev_name, "nvme", 4) == 0;
            int has_partition_suffix = strstr(dev_name, "p") != NULL;
            int ends_with_digit = isdigit((unsigned char)dev_name[strlen(dev_name) - 1]);
            int whole_device = is_nvme ? (has_partition_suffix == 0) : (ends_with_digit == 0);

            if (whole_device) {
                unsigned long long reads_delta = 0, sectors_read_delta = 0, read_time_delta = 0;
                unsigned long long writes_delta = 0, sectors_written_delta = 0, write_time_delta = 0;
                unsigned long long io_time_delta = 0;

                if (last_reads[disk_idx] > 0) {
                    reads_delta            = (reads            >= last_reads[disk_idx])            ? (reads            - last_reads[disk_idx])            : 0;
                    sectors_read_delta     = (sectors_read     >= last_sectors[disk_idx])         ? (sectors_read     - last_sectors[disk_idx])         : 0;
                    read_time_delta        = (read_time_ms     >= last_read_time_ms[disk_idx])    ? (read_time_ms     - last_read_time_ms[disk_idx])    : 0;

                    writes_delta           = (writes           >= last_writes[disk_idx])          ? (writes           - last_writes[disk_idx])          : 0;
                    sectors_written_delta  = (sectors_written  >= last_sectors_written[disk_idx]) ? (sectors_written  - last_sectors_written[disk_idx]) : 0;
                    write_time_delta       = (write_time_ms    >= last_write_time_ms[disk_idx])   ? (write_time_ms    - last_write_time_ms[disk_idx])   : 0;

                    io_time_delta          = (io_time_ms       >= last_io_time_ms[disk_idx])      ? (io_time_ms       - last_io_time_ms[disk_idx])      : 0;

                    if (reads_delta > 0 || sectors_read_delta > 0 || read_time_delta > 0 ||
                        writes_delta > 0 || sectors_written_delta > 0 || write_time_delta > 0 ||
                        io_time_delta > 0) {
                        profiler_log_diskstat(dev_name,
                                              reads_delta, sectors_read_delta, read_time_delta,
                                              writes_delta, sectors_written_delta, write_time_delta,
                                              io_time_delta, in_flight);
                    }
                }

                last_reads[disk_idx]            = reads;
                last_sectors[disk_idx]          = sectors_read;
                last_read_time_ms[disk_idx]     = read_time_ms;

                last_writes[disk_idx]           = writes;
                last_sectors_written[disk_idx]  = sectors_written;
                last_write_time_ms[disk_idx]    = write_time_ms;
                last_io_time_ms[disk_idx]       = io_time_ms;

                disk_idx++;
            }
        }
    }

    fclose(fp);
}