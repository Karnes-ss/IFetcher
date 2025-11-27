#ifndef DENSITY_H
#define DENSITY_H
#include "reader.h"

// 计算 Epanechnikov 核函数权重
void estimate_weight(double *weight, int window_size);
// 计算 I/O 密度（加权窗口求和）
void get_IO_density(const StatRecord *records, int count, double *density, int window_size, double *weight);
// 判断是否为局部最大值
int is_local_max(const double *arr, int idx, int count);
// 判断是否为局部最小值
int is_local_min(const double *arr, int idx, int count);

#endif