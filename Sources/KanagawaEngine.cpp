// Copyright (C) 2025 Kanagawa Yamada

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>

#define MONITOR_INTERVAL_SEC 5
#define CPU_USAGE_THRESHOLD 40
#define MAX_PATH 256
#define MAX_FREQ_BUF 4096

// Path definitions
const char *SYS_CPU_BASE = "/sys/devices/system/cpu/cpufreq";

// Helper to write string to file
void write_to_file(const char *path, const char *value) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) return; 
    
    write(fd, value, strlen(value));
    close(fd);
}

// Helper to write integer to file
void write_int_to_file(const char *path, int value) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", value);
    write_to_file(path, buf);
}

// Read CPU stats from /proc/stat
void get_cpu_times(unsigned long long *idle, unsigned long long *total) {
    FILE *fp = fopen("/proc/stat", "r");
    if (!fp) return;

    char label[10];
    unsigned long long user, nice, system, idl, iowait, irq, softirq, steal;
    
    // Read the first line (aggregate cpu)
    fscanf(fp, "%s %llu %llu %llu %llu %llu %llu %llu %llu", 
           label, &user, &nice, &system, &idl, &iowait, &irq, &softirq, &steal);
    
    *idle = idl + iowait;
    *total = user + nice + system + idl + iowait + irq + softirq + steal;
    
    fclose(fp);
}

// Parse available frequencies and return the target freq based on percentage
// percent: 0.75 for 75%, 0.50 for 50%
// return_max: if 1, returns the absolute max freq found. If 0, returns the absolute min.
int get_target_freq(const char *policy_path, float percent, int return_abs_max, int return_abs_min) {
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s/scaling_available_frequencies", policy_path);
    
    FILE *fp = fopen(path, "r");
    // Fallback: If available_frequencies not readable, try cpuinfo bounds (simplification)
    if (!fp) {
        if (return_abs_max) {
             snprintf(path, sizeof(path), "%s/cpuinfo_max_freq", policy_path);
             fp = fopen(path, "r");
             if (fp) { int val; fscanf(fp, "%d", &val); fclose(fp); return val; }
        }
        return 0; 
    }

    char buf[MAX_FREQ_BUF];
    if (!fgets(buf, sizeof(buf), fp)) { fclose(fp); return 0; }
    fclose(fp);

    // Parse frequencies into array
    int freqs[100];
    int count = 0;
    char *token = strtok(buf, " \n");
    while (token && count < 100) {
        freqs[count++] = atoi(token);
        token = strtok(NULL, " \n");
    }

    if (count == 0) return 0;

    // Sort (Bubble sort) to ensure ascending order
    for (int i = 0; i < count - 1; i++) {
        for (int j = 0; j < count - i - 1; j++) {
            if (freqs[j] > freqs[j + 1]) {
                int temp = freqs[j];
                freqs[j] = freqs[j + 1];
                freqs[j + 1] = temp;
            }
        }
    }

    if (return_abs_max) return freqs[count - 1];
    if (return_abs_min) return freqs[0];

    // Calculate index based on percentage
    int index = (int)(count * percent);
    if (index >= count) index = count - 1;
    
    return freqs[index];
}

void apply_profile(int is_high_load) {
    DIR *dir;
    struct dirent *ent;
    
    dir = opendir(SYS_CPU_BASE);
    if (!dir) return;

    while ((ent = readdir(dir)) != NULL) {
        // Iterate through policy0, policy1, etc.
        if (strncmp(ent->d_name, "policy", 6) == 0) {
            char policy_path[MAX_PATH];
            snprintf(policy_path, sizeof(policy_path), "%s/%s", SYS_CPU_BASE, ent->d_name);

            // Calculate targets
            int absolute_max = get_target_freq(policy_path, 1.0, 1, 0);
            int absolute_min = get_target_freq(policy_path, 0.0, 0, 1);
            
            // Failsafe if frequencies couldn't be read
            if (absolute_max == 0 || absolute_min == 0) continue;

            char min_node[MAX_PATH];
            char max_node[MAX_PATH];
            snprintf(min_node, sizeof(min_node), "%s/scaling_min_freq", policy_path);
            snprintf(max_node, sizeof(max_node), "%s/scaling_max_freq", policy_path);

            if (is_high_load) {
                // LOGIC: High Load (>40%)
                // 1. Unlock MaxFreq to Absolute Max
                // 2. Increase MinFreq to 75%
                int target_75 = get_target_freq(policy_path, 0.75, 0, 0);
                
                // Order matters: Write MAX first so MIN doesn't error if it exceeds current max
                write_int_to_file(max_node, absolute_max);
                write_int_to_file(min_node, target_75);

            } else {
                // LOGIC: Low Load (<=40%)
                // 1. Drop MinFreq to Absolute Min
                // 2. Cap MaxFreq to 50%
                int target_50 = get_target_freq(policy_path, 0.50, 0, 0);

                // Order matters: Write MIN first so MAX doesn't error if it goes below current min
                write_int_to_file(min_node, absolute_min);
                write_int_to_file(max_node, target_50);
            }
        }
    }
    closedir(dir);
}

int main() {
    unsigned long long prev_idle = 0, prev_total = 0;
    unsigned long long curr_idle = 0, curr_total = 0;
    
    // Initial read to set baseline
    get_cpu_times(&prev_idle, &prev_total);

    // Simple logging to stdout (can be viewed via logcat if wrapped properly)
    printf("Kanagawa Engine (Standard) Started.\n");

    while (1) {
        sleep(MONITOR_INTERVAL_SEC);

        get_cpu_times(&curr_idle, &curr_total);

        unsigned long long diff_total = curr_total - prev_total;
        unsigned long long diff_idle = curr_idle - prev_idle;

        double usage = 0.0;
        if (diff_total > 0) {
            usage = (100.0 * (diff_total - diff_idle)) / diff_total;
        }

        // Update previous values for next delta calculation
        prev_idle = curr_idle;
        prev_total = curr_total;

        if (usage > CPU_USAGE_THRESHOLD) {
            apply_profile(1); // Apply High Load Settings
        } else {
            apply_profile(0); // Apply Low/Normal Settings
        }
    }
    return 0;
}