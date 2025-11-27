#include <math.h>
#include "density.h"

// 计算 Epanechnikov 核函数权重
void estimate_weight(double *weight, int window_size) {
    int edge = window_size / 2;
    int d = edge + 1;                 // ceil(w/2)
    int idx = 0;
    for (int k = -edge; k <= edge; k++) {
        double u = (double)k / (double)d; // u_i ∈ [-1+1/d, ..., 1-1/d]
        u = u / (u + 1.0);                // Algorithm 1: u ← u/(u+1)
        weight[idx++] = 0.75 * (1.0 - u * u);
    }
}

// 计算 I/O 密度（窗口加权求和）
void get_IO_density(const StatRecord *records, int count, double *density, int window_size, double *weight) {
    int edge = window_size / 2;
    for (int t = 0; t < count; t++) {
        double sum = 0.0;
        for (int i = -edge; i <= edge; i++) {
            int idx = t + i;
            int widx = i + edge;
            if (idx >= 0 && idx < count) {
                sum += records[idx].delta_io * weight[widx];
            }
        }
        density[t] = sum;
    }
}

// 判断是否为局部最大值
int is_local_max(const double *arr, int idx, int count) {
    if (idx <= 0 || idx >= count - 1) return 0;
    return arr[idx] > arr[idx - 1] && arr[idx] > arr[idx + 1];
}

// 判断是否为局部最小值
int is_local_min(const double *arr, int idx, int count) {
    if (idx <= 0 || idx >= count - 1) return 0;
    return arr[idx] < arr[idx - 1] && arr[idx] < arr[idx + 1];
}