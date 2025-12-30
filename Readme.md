# IFetcher - 应用启动预取优化测试

本项目旨在通过 `IFetcher` 预取机制优化应用程序的冷启动性能。通过记录并重放应用启动过程中的缺页（Page Fault）和 I/O 行为，减少主缺页（Major Page Faults）次数与磁盘读取量，从而提升启动速度并降低系统抖动。

## 1. 环境准备 (Environment Setup)

在运行测试脚本前，请确保虚拟机已安装以下基础工具链和监控工具。

### 一、基础构建工具

用于编译测试程序及相关依赖。

```bash
sudo apt update && sudo apt install -y build-essential
# 包含 gcc, make 等
```

### 二、性能计数与监控

用于精确统计时间、缺页中断及文件系统 I/O。

```bash
# 安装 time 命令 (注意不是 shell 内置的 time)
sudo apt install -y time

# 安装 procfs 相关工具 (ps, killall 等)
sudo apt install -y procps psmisc
```

### 三、测试目标应用与自动化工具

根据你选择的测试脚本，需要安装相应的应用和工具：

1. **Firefox 测试 (`run_tight_flow.sh`)**:
   ```bash
   sudo apt install -y firefox
   ```

2. **Trigger Rally 游戏测试 (`run_game.sh` / `rt_single.sh`)**:
   ```bash
   # 安装游戏本体
   sudo apt install -y trigger-rally
   
   # 安装窗口自动化工具 (仅 run_game.sh 需要)
   sudo apt install -y xdotool
   ```

> **注意**: 预取器依赖 Linux 内核的 `inotify` 机制，这通常已内置于现代内核中。

## 2. 运行测试 (Usage)

本项目提供三种不同场景的测试脚本，请根据需求选择：

### 场景 A: Firefox 浏览器自动化测试 (Headless)
**脚本**: `launch_firefox.sh`
**特点**: 无需图形界面 (Headless 模式)，适合快速验证预取器对大型应用启动的影响。

```bash
cd IFetcher
bash launch_firefox.sh
```

### 场景 B: Trigger Rally 游戏自动化测试
**脚本**: `launch_game.sh`
**特点**: 启动 3D 赛车游戏，使用 `xdotool` 自动检测窗口出现并关闭，模拟真实用户启动过程。

```bash
cd IFetcher
bash launch_game.sh
```

### 场景 C: Trigger Rally 游戏手动测试
**脚本**: `run_game.sh`
**特点**: 需要人工介入操作。适合在自动化脚本失效或需要人工观察启动过程时使用。
- 脚本运行后会提示用户手动启动游戏进行 Training（训练）。
- 之后会提示用户手动进行多次启动以测量性能。

```bash
cd IFetcher
bash run_game.sh
```

> **提示**: 所有脚本在运行过程中都需要 `sudo` 权限以清除页面缓存 (drop caches) 和捕获系统级事件，请在提示时输入密码。

---

## 3. 结果解读 (Metrics & Interpretation)

无论使用哪种脚本，测试结束后都会输出类似格式的对比数据：

- **前 4 行**: **未开启** 预取器时的冷启动数据（Baseline）。
    
- **后 4 行**: **开启** 预取器后的冷启动数据（Optimized）。
    

### 关键指标说明

|**指标 (Metric)**|**全称**|**说明 (Description)**|**优化目标**|
|---|---|---|---|
|**Elapsed**|Elapsed Time|应用启动总耗时（秒）。|**越小越好**|
|**Major PF**|Major Page Faults|主缺页中断次数。表示数据必须从磁盘读取到内存的次数，是造成启动卡顿的主要原因。|**越少越好**|
|**FS inputs**|File System Inputs|实际发生的文件系统输入次数（磁盘块读取）。|**越少越好**|
|**Var**|Variance|方差。衡量多次运行结果的波动程度，数值越小表示性能越稳定。|**越小越稳定**|

---

## 4. 实验现象与分析 (Observations)

### 性能优化趋势

通常情况下，开启预取（后 4 行）相比未开启（前 4 行），应当观察到：

- **Major PF 大幅下降**：预取器提前将数据载入内存，避免了启动时的阻塞性 I/O。
    
- **FS inputs 减少**：I/O 请求被合并或提前处理。
    

### 关于结果波动的说明

在多次测试中，可能会出现 **Elapsed Time（耗时）优化不明显或持平** 的情况，这属于正常现象，原因如下：

1. **系统噪声**：虚拟机中的后台进程或宿主机资源调度可能影响 CPU 时间片。
    
2. **I/O 瓶颈**：即使 I/O 次数减少，如果磁盘带宽瞬间饱和，时间收益可能被压缩。
    

重要结论：

即便时间偶尔持平，只要观察到 Major PF 和 FS inputs 有显著下降，即证明预取器 有效工作。因为这代表应用对底层磁盘的依赖被物理性地减少了，系统的整体吞吐量和并发承载能力得到了提升。