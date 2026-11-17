# TB371FC 4.19.325 内核性能优化报告（纯代码层面 · 零 ADB）

- 日期：2026-11-17
- 源码：`/home/yuki/videos/kernel-tb371fc-work/kernel-tb371fc`（git，HEAD `71e03dc2e86d`）
- 设备背景：Lenovo Xiaoxin Pad Pro 12.7 (TB371FC)，Qualcomm SM8250 kona（1×A77 prime + 3×A77 big + 4×A55 LITTLE），arm64，144Hz 屏（帧预算 6.9ms），UFS（单硬件队列）
- 目标场景：应用冷启动 / 滑动流畅度 / 内存压力
- 约束：允许改 core kernel；允许 backport（单独登记 + 重点审查）；非 GKI，关注 EXPORT_SYMBOL 与厂商模块依赖
- 方法：只读代码 + 主机侧编译/静态检查；**未执行任何 adb/fastboot/设备命令**
- 结论分级：【代码可证实】= 可从代码直接推导；【推断，待真机验证】= 一切量化收益/延迟/吞吐。本报告不编造 benchmark 数字。
- 审查方式：四路并行深度审查（MGLRU / 调度·cpufreq / I/O·binder·verity / BBRv3）+ 主审逐项核实关键论断 + 第二遍交叉审核（第 7 节）。

---

## 1. 摘要（Top 发现，按 影响 × 置信度 排序）

| 排序 | ID | 子系统 | 级别 | 一句话 | 置信度 | 处置 |
|---|---|---|---|---|---|---|
| 1 | PERF-I01 | dm-verity | P0 | kverityd 改 per-cpu+HIGHPRI 后钉死 IRQ 核、与 kblockd 共享 highpri 池，冷启动校验吞吐受单核限制 | 代码可证实+推断 | **已出补丁 P-A（回退 UNBOUND）** |
| 2 | PERF-S01 | schedutil/WALT | P1 | WALT 下唤醒不升频，PL 通道默认 pl=0 被丢弃；144Hz 窗口 8ms>6.9ms 帧预算 | 代码可证实 | L1 运行时 pl=1 先验证；L2 草案 |
| 3 | PERF-S02 | WALT | P1 | hardirq 持全部 8 把 rq 锁期间做 cpufreq MMIO+频率标度，125Hz | 代码可证实 | L3 草案（需 util 快照，禁裸移锁） |
| 4 | PERF-M01 | MGLRU | P0(条件) | 厂商多 kswapd（kswapd[16]）共享单 pgdat->mm_walk，>1 线程竞态 | 代码可证实 | L1 保持 kswapd_threads=1；L2 守卫草案 |
| 5 | PERF-S03 | EAS | P1 | 每 tick 在全局 migration_lock 内跑完整 EAS 搜索 | 代码可证实 | L2 节流草案（EAS 是否生效待真机） |
| 6 | PERF-S04 | WALT | P1 | hardirq 内 O(N) 无早退遍历 top-app 线程链表 | 代码可证实 | L2 增量维护草案 |
| 7 | PERF-I02 | blk-mq | P1 | 厂商硬编码 BFQ 为单队列默认（elevator.c:1011），sync idling 开销 | 代码可证实 | L1 评估 io.weight 依赖后切 mq-deadline |
| 8 | PERF-I03 | UFS | P1 | 时钟门控 50ms，空闲后首请求 ungate+3ms 重排队 | 代码可证实 | L1 启动窗口调 clkgate |
| 9 | PERF-N01 | BBRv3 | P1 | CA_EVENT_TLP_RECOVERY 无发射点，loss-probe-recovery 永不可达 | 代码可证实 | L2 接线草案（非目标场景，登记） |
| 10 | PERF-N02 | BBRv3 | P1 | WANTS_CE_EVENTS→NEEDS_ECN 强制 ECN 协商，握手行为改变 | 代码可证实 | L2 门控草案（非目标场景，登记） |
| 11 | PERF-M02 | MGLRU | P2 | lru_gen_refault 在 min_seq 匹配前无条件计 REFAULT | 代码可证实 | **已出补丁 P-B** |
| 12 | PERF-S05 | sched | P2 | WALT 下 UTIL_EST 维护是纯开销（读者全旁路） | 代码可证实 | L1 NO_UTIL_EST（已被 sched_feat 门控，无需改码） |
| 13 | PERF-S13a | WALT | P3 | sched_get_cpu_util 死加载 rq->cfs.avg.util_avg | 代码可证实 | **已出补丁 P-C** |

**已落地并编译验证的补丁（3 项，见第 5.1 节）**：P-A（dm-verity 回退）、P-B（workingset 计数时序）、P-C（sched_avg 死加载）。
**重要核实修正**：PERF-N04（tcp_skb_cb 尺寸断言）**已存在**于 [tcp.c:3926](file:///home/yuki/videos/kernel-tb371fc-work/kernel-tb371fc/net/ipv4/tcp.c#L3926)，无需新增（子代理漏查 tcp.c）；PERF-S05 的 UTIL_EST 已被 `sched_feat(UTIL_EST)` 门控，L1 运行时关闭即可，**不做冗余代码改动**。

---

## 2. 版本 / 特性 / backport 登记表

### 2.1 版本确认
- `Makefile`: VERSION=4 PATCHLEVEL=19 SUBLEVEL=325 → **4.19.325**，厂商内核（非 GKI）。
- 构建：llvm-21 clang + GNU ld，`CROSS_COMPILE=aarch64-linux-gnu-`，`DYNAMIC_SINGLE_CHIP=qca6390`，force-include `drivers/staging/fw-api/fw/p226_compat.h`。
- 基线编译复核：相同 flag 增量 `make Image` **exit 0**（`build-baseline-check.log`，2026-11-17）。

### 2.2 特性存在性表（一律以源码为准）

| 特性 | 状态 | 证据 |
|---|---|---|
| WALT 调度 | 存在，启用 | `CONFIG_SCHED_WALT=y`；`kernel/sched/walt.c` |
| EAS（mainline fair.c 式） | 代码在，**是否生效待真机** | `fair.c:7769 find_energy_efficient_cpu`；注册链 `qcom-cpufreq-hw.c:370→topology.c:257`；DT `dynamic-power-coefficient` 无法核实（vendor dts 悬空符号链接） |
| energy_model | 存在 | `CONFIG_ENERGY_MODEL=y`；`topology.c`、`qcom-cpufreq-hw.c` |
| util_est | 存在（厂商 backport），WALT 下读者旁路 | `fair.c:3775`；`sched_feat(UTIL_EST)` 门控（features.h:92） |
| uclamp | 代码存在但**关闭**，已桥接 schedtune | `sched.h:884 #ifdef CONFIG_UCLAMP_TASK`；`core.c:1269` 回退桩；**建议保持关闭**（见 S14） |
| schedutil fast_switch | 存在 | `cpufreq_schedutil.c:242/720` |
| schedutil↔WALT | 存在 | `cpufreq_schedutil.c:600 sugov_walt_adjust`、`:442 stune_util` |
| core_ctl / cpu-boost / schedtune | 存在，启用 | `CONFIG_SCHED_CORE_CTL=y`/`CPU_BOOST=y`/`SCHED_TUNE=y` |
| CFS_BANDWIDTH | **新启用**（commit 4030a1ac103f） | throttle 路径成为新热路径分支，需真机回归 |
| MGLRU | **backport，启用** | `CONFIG_LRU_GEN=y/ENABLED=y/STATS=y`；`vmscan.c` 159 处 lru_gen |
| PSI | **backport，启用** | `CONFIG_PSI=y`；`kernel/sched/psi.c`（初始厂商提交 9b07f14fc025，上游 4.20+） |
| per-VMA lock / proactive compaction / PREEMPT_DYNAMIC / io_uring | 均不存在 | 4.19；源码无对应定义 |
| THP | 关闭 | `# CONFIG_TRANSPARENT_HUGEPAGE is not set` |
| zram writeback / dedup | **均关闭** | `# CONFIG_ZRAM_WRITEBACK/ZRAM_DEDUP`；默认压缩 `lzo`（zram_drv.c:44） |
| blk-mq 默认调度器 | **BFQ**（厂商改） | `elevator.c:1011 IS_ENABLED(CONFIG_IOSCHED_BFQ)` 覆盖 mq-deadline |
| arm64 HW_AFDBM | 启用 | `CONFIG_ARM64_HW_AFDBM=y`；TCR_HA 运行时按 ID_AA64MMFR1（proc.S:534） |
| 抢占/HZ | PREEMPT=y, HZ=250, NO_HZ_IDLE | `.config` |

### 2.3 backport 登记表（重点审查对象）

| ID | 特性 | 提交 | 规模 | 审查结论 |
|---|---|---|---|---|
| BP-1 | MGLRU（6.1+→4.19） | `ae701d9ef34a` | 33 文件，vmscan.c +3274 | 核心适配（lru_lock/位域/pagevec/生命周期/pagewalk/arm64 young）**忠实正确**；隐患见 M01/M02/M04/M05/M06/M07 |
| BP-2 | BBRv3（→4.19） | `c0a9b6cb5470` | 9 文件 | **无 P0 ABI bug**；cb[56] 恰好容纳（断言已在 tcp.c:3926）；sk_buff 248→256B 但 slab 对象不变；uapi 纯追加；无 EXPORT_SYMBOL 变更；隐患 N01/N02/N03 |
| BP-3 | PSI（4.20+→4.19） | 初始厂商提交 | — | 与 MGLRU 压力上报兼容（vmpressure/PSI 路径保留，vmscan.c:5681/6195） |
| BP-4 | util_est（4.20+→4.19） | 厂商树 | — | WALT 下读者旁路，纯开销（S05） |
| BP-5 | uclamp（半成品） | 厂商树 | — | 已桥接 schedtune，建议保持关闭（S14） |

非 backport 相关改动：`d936a991331c` dm-verity kverityd 调优（**复核建议回退**，I01）；`4030a1ac103f` CFS_BANDWIDTH 启用（新热路径，需回归）。

### 2.4 关键 config 快照对比（config-pre-bbr.bak → 当前 .config）
来源：`config-pre-bbr.bak`（2026-11-16 15:27）与 `.config`（2026-11-16 17:59）。
- 新增：`LRU_GEN=y/ENABLED=y/STATS=y`、`TCP_CONG_BBR=y`+`DEFAULT_TCP_CONG="bbr"`、`DEFAULT_NET_SCH="fq"`、`MSM_IDLE_STATS=y`。
- 释放版残留调试项：`DEBUG_LIST=y`、`SCHED_DEBUG=y`、`SCHEDSTATS=y`、`DEBUG_INFO=y`。
- 安全加固（保留，不建议关）：`SLAB_FREELIST_RANDOM/HARDENED`、`HARDENED_USERCOPY`、`VMAP_STACK`、`STRICT_KERNEL/MODULE_RWX`。
- `vm_swappiness=150`（vmscan.c:189）；`extra_free_kbytes=300000`（page_alloc.c:344，≈293MB）。

---

## 3. 场景 → 热路径调用链（文件:行号）

### 3.1 应用冷启动
```
execve → copy_process → sched_fork(core.c:2868) → init_new_task_load(walt.c:2140, 2×__GFP_NOFAIL kcalloc)
 → wake_up_new_task(core.c:3077) → select_task_rq_fair(fair.c:7961)
   →[EAS生效时] find_energy_efficient_cpu(fair.c:7769) → find_best_target(6969) → compute_energy(7551) → em_pd_energy(energy_model.h:103 线性扫OPP)
 → enqueue_task_fair(fair.c:5520) → util_est_enqueue(3804) / inc_rq_walt_stats(walt.c:213)
 → exec_mmap: lru_gen_use_mm(fs/exec.c:1053)
mmap/缺页 → f2fs_filemap_fault(fs/f2fs/file.c:35) → page_cache_sync_readahead(mm/readahead.c:510, 受 blk_cgroup_congested 门控:515)
 → f2fs_mpage_readpages(data.c:2361) → f2fs_map_blocks(1535) →[extent空] f2fs_get_dnode_of_data(node.c:539, NAT+node读)
 → submit_bio → blk_mq_make_request(blk-mq.c:1911) →[BFQ默认] ufshcd_queuecommand(ufshcd.c:3660)
   → ufshcd_hold(2174, 空闲>50ms ungate async→-EAGAIN→3ms重排队 blk-mq.c:1141/1291)
 → verity_end_io(dm-verity-target.c:583) → queue_work(verify_wq) → verity_work(576) → SHA256(QCE/sha256-ce)
并行 IPC: binder_transaction(binder.c:2989) → binder_alloc_new_buf(binder_alloc.c:585, mutex内GFP_KERNEL分配) → 唤醒 system_server
```

### 3.2 滑动流畅度（144Hz）
```
input IRQ → sched_account_irqstart(walt.c:383)
binder_wakeup_thread_ilocked(binder.c:1105, 置 low_latency)
 → try_to_wake_up(core.c:2648, pi_lock+IRQoff 2660→2759)
   → walt_try_to_wake_up(core.c:2599) → update_task_ravg×2(walt.c:2087) → update_top_tasks(1242)
   → update_preferred_cluster(2806) → _set_preferred_cluster(2741, 遍历grp->tasks, 有早退2777)
   → select_task_rq → find_energy_efficient_cpu（重复 schedtune_task_boost≥4次/per_task_boost≥3次, S06）
   →[sched_predl=1] do_pl_notif(walt.c:1451) → cpufreq_update_util(WALT|PL)
     →[pl默认0被丢弃 cpufreq_schedutil.c:786/663, S01]
窗口滚动(hardirq, 144Hz=8ms): walt_irq_work(walt.c:3353)
 → 锁全部possible rq(3368-3374) → update_task_ravg → cpufreq_update_util循环(3423-3453, MMIO在锁内, S02)
 → 窗口切换(3466-3480) → 解锁全部rq(3482) → core_ctl_check(3486, 已在锁外)
   → walt_fill_ta_data(walt.c:3514, O(N)无早退遍历top-app, S04)
tick(4ms): scheduler_tick(core.c:3779) → check_for_migration(fair.c:13028)
 →[misfit] raw_spin_lock(migration_lock全局, 13043) → find_energy_efficient_cpu(完整EAS, S03)
```

### 3.3 内存压力
```
分配 → __alloc_pages_slowpath → direct reclaim / kswapd
kswapd(可多线程 kswapd[16], mmzone.h:906) → balance_pgdat → lru_gen_age_node
 → set_mm_walk(vmscan.c:3731, kswapd共享&pgdat->mm_walk, 多walker竞态 M01)
 → walk_pmd_range → isolate_folios(vmscan.c:4395 计PGSCAN_KSWAPD gated / PGSCAN_ANON无条件 M07)
 → evict_folios(4573 计PGSTEAL) → shrink_page_list → zram写(zram_drv.c, lzo默认, slot锁外压缩)
 → lru_gen_refault(workingset.c:276 REFAULT计数偏前 M02)
压力上报: shrink_node→vmpressure(vmscan.c:5681) / PSI memstall(__perform_reclaim) → LMKD
```

---

## 4. 发现清单表

| ID | 子系统 | 位置 | 级别 | 类型 | 置信度 |
|---|---|---|---|---|---|
| PERF-I01 | dm-verity | dm-verity-target.c:1193 | P0 | 工作队列绑定回归 | 代码可证实+推断 |
| PERF-M01 | MGLRU | vmscan.c:3731; mmzone.h:906 | P0(条件) | backport 多walker竞态 | 代码可证实 |
| PERF-S01 | schedutil | cpufreq_schedutil.c:786/663; sched.h:2584 | P1 | 升频滞后 | 代码可证实 |
| PERF-S02 | WALT | walt.c:3368-3453 | P1 | hardirq长临界区 | 代码可证实 |
| PERF-S03 | EAS | fair.c:13043 | P1 | 全局锁内重扫 | 代码可证实 |
| PERF-S04 | WALT | walt.c:3514-3527 | P1 | hardirq O(N)遍历 | 代码可证实 |
| PERF-I02 | blk-mq | elevator.c:1011 | P1 | 默认调度器取舍 | 代码可证实 |
| PERF-I03 | UFS | ufshcd.c:261/2174; blk-mq.c:1141 | P1 | 首请求延迟 | 代码可证实 |
| PERF-N01 | BBRv3 | tcp_input.c:3759; tcp_bbr.c:553 | P1 | backport 接线缺失 | 代码可证实 |
| PERF-N02 | BBRv3 | tcp_bbr_v3_compat.h:36; tcp_output.c:312 | P1 | ECN 语义偏移 | 代码可证实 |
| PERF-M02 | MGLRU | workingset.c:276 | P2 | 统计时序 | 代码可证实 |
| PERF-M03 | MGLRU | page_alloc.c:344; vmscan.c:4043 | P2 | 工作集保护缺失 | 推断 |
| PERF-M07 | MGLRU | vmscan.c:4402/4577 | P2 | 统计口径不一致 | 代码可证实 |
| PERF-S05 | sched | fair.c:3804/3842 | P2 | 纯开销 | 代码可证实 |
| PERF-S06 | EAS | fair.c:7787/7535/6980 | P2 | 重复不变量查询 | 代码可证实 |
| PERF-S07 | EAS | fair.c:7969-7990 | P2 | 死块+无回落 | 代码可证实 |
| PERF-S08 | EAS | energy_model.h:103 | P2 | 线性扫OPP | 代码可证实 |
| PERF-S09 | EAS | fair.c:7083/3195 | P2 | 无锁写远端curr | 代码可证实+推断 |
| PERF-S10 | schedutil | cpufreq_schedutil.c:1266/750 | P2 | stale_ns不随窗口更新 | 代码可证实 |
| PERF-I04 | zram | zram_drv.c:44 | P2 | 默认压缩器 | 代码可证实 |
| PERF-I05 | f2fs | data.c:1716/1740 | P2 | 缓冲读不回填extent | 代码可证实 |
| PERF-I06 | binder | binder_alloc.c:183/256 | P2 | 锁内GFP_KERNEL | 代码可证实+推断 |
| PERF-N03 | BBRv3 | tcp.h:881; tcp_rate.c:68 | P2 | lost:12回绕 | 代码可证实+推断 |
| PERF-N04 | BBRv3 | tcp.c:3926（断言已在） | P3 | 陈旧注释 | 代码可证实（修正） |
| PERF-M04 | MGLRU | swap.c:570-571 | P3 | SetPageReclaim时序 | 代码可证实 |
| PERF-M05 | MGLRU | arch/arm64/pgtable.h:795 | P3 | 编译期vs运行时young | 代码可证实（本SoC无碍） |
| PERF-M06 | MGLRU | mmzone.h NR_HIST_GENS; vmscan.c:5357 | P3 | zombie memcg bloom累积 | 代码可证实 |
| PERF-S11 | MGLRU/sched | core.c:3551; mm_types.h:587 | P3 | ctx钩子无runtime守卫 | 代码可证实 |
| PERF-S12 | WALT | walt.c:2151-2154 | P3 | fork 2×NOFAIL分配 | 代码可证实 |
| PERF-S13 | WALT | sched_avg.c:206; walt.c:162; fair.c:180 | P3 | 死加载/granule/阈值 | 代码可证实 |
| PERF-I07 | f2fs | node.c:539; segment.c:511 | P3 | NAT/SIT vs GC | 推断 |
| PERF-I08 | readahead | readahead.c:515; ufs-qcom.c:1647 | P3 | 拥塞门控/QoS时序 | 代码可证实+推断 |
| PERF-N05 | BBRv3 | tcp.h:386-387 | P3 | 死标志 | 代码可证实 |
| PERF-N06 | BBRv3 | tcp.h:1086 | P3 | cong_ops中部插成员 | 代码可证实 |
| PERF-N07 | BBRv3 | tcp_rate.c:96-98 | P3 | in_flight字节→包近似 | 代码可证实 |
| PERF-C01 | config | .config DEBUG_LIST/SCHEDSTATS | P2 | 释放版调试残留 | 代码可证实 |
| PERF-S14 | uclamp | sched.h:966; core.c:1269 | 建议 | 保持关闭 | 代码可证实 |

---

## 5. 逐项详述

### 5.1 已落地并编译验证的补丁（3 项）

补丁文件：`patches/perf-20261117/00-all-perf-fixes.patch`（合并）及 01/02/03 分项。
编译验证：当前 config（LRU_GEN=y, WALT=y, BBR=y）增量 `make Image` **exit 0，零新增告警**（`build-perf-patchset-current.log`）；`CONFIG_LRU_GEN=n` 变体见第 6 节。

#### PERF-I01 [P0][dm-verity] kverityd 工作队列绑定回归（补丁 P-A）
- 位置：drivers/md/dm-verity-target.c:1193
- 证据：commit d936a991331c 将 `WQ_CPU_INTENSIVE|WQ_MEM_RECLAIM|WQ_UNBOUND, num_online_cpus()` 改为 `WQ_HIGHPRI|WQ_CPU_INTENSIVE|WQ_MEM_RECLAIM, 0`。
- 机理（基于本树 workqueue.c 语义，均已核实）：
  1. `max_active=0`→`WQ_DFL_ACTIVE=256`（workqueue.c:4156, workqueue.h:357），非"无限并发"。
  2. per-cpu wq 的 `__queue_work` 取 `raw_smp_processor_id()`（workqueue.c:1432）；verity_end_io 在 bio 完成 softirq（IRQ 亲和 CPU，UFS 单队列常固定/LITTLE）queue_work → SHA256 校验钉死单核。原 WQ_UNBOUND 经 `wq_select_unbound_cpu`（workqueue.c:1380）在 unbound cpumask 摊载到 8 核（含大核）。
  3. per-cpu+HIGHPRI 用 `cpu_worker_pools[1]`（workqueue.c:4051），与 kblockd（blk-core.c:3933 `WQ_MEM_RECLAIM|WQ_HIGHPRI`）**同池**；WQ_CPU_INTENSIVE 使 worker 不计 nr_running → 可链式拉起多个 -20 kworker，与 kblockd 及同核用户线程争抢。
  4. "避免 big.LITTLE 迁移"理由不成立：unbound kworker 由 CFS/EAS 调度倾向大核；per-cpu 绑定才强制落 IRQ 核。
- 方案：L1（回退）。恢复原 UNBOUND 配置（原注释明示 ramdisk 性能收益）。如需降调度延迟，可另试 `WQ_UNBOUND|WQ_HIGHPRI|WQ_CPU_INTENSIVE|WQ_MEM_RECLAIM`（保留跨核摊载，highpri 仅作用 unbound 池），但 HIGHPRI 对大核用户线程的抢占需真机权衡。
- 补丁：
  ```diff
  -	/* WQ_HIGHPRI cuts verify scheduling latency; WQ_UNBOUND dropped for arm64 big.LITTLE. */
  -	v->verify_wq = alloc_workqueue("kverityd", WQ_HIGHPRI | WQ_CPU_INTENSIVE | WQ_MEM_RECLAIM, 0);
  +	/* WQ_UNBOUND greatly improves performance when running on ramdisk */
  +	v->verify_wq = alloc_workqueue("kverityd", WQ_CPU_INTENSIVE | WQ_MEM_RECLAIM | WQ_UNBOUND, num_online_cpus());
  ```
- 正确性论证：回退到上游推荐配置，无新增同步语义；WQ_MEM_RECLAIM 保留 rescuer，内存回收路径安全；num_online_cpus() 为原 max_active，并发上限语义恢复。不涉导出符号。
- 预期影响：冷启动读 system/vendor（verity 保护）校验吞吐恢复多核摊载，首屏延迟定性下降【推断，待真机验证】。
- 测试：编译矩阵（第 6 节）；真机 A/B 见第 8 节。
- 状态：语义【代码可证实】；延迟收益【推断，待真机验证】。
- **注**：此补丁修订用户近期提交 d936a991331c，证据充分但是否采纳由用户决定（git revert 可独立回退）。

#### PERF-M02 [P2][MGLRU] lru_gen_refault 计数时序（补丁 P-B）
- 位置：mm/workingset.c:276（CONFIG_LRU_GEN 分支）
- 证据：`mod_lruvec_state(lruvec, WORKINGSET_REFAULT, delta)` 在 min_seq token 匹配检查（:279）之前无条件累加；上游仅在匹配后计。
- 机理：非匹配 token（goto unlock）也计 REFAULT，污染 vmstat 与 legacy `inactive_list_is_low` 启发式；MGLRU 自身反馈用 atomics（refaulted[]）不受影响。亦使 A/B 证据中 MGLRU-on/off 的 lru_gen_refault 口径不可直接比较。
- 方案：L2。将计数移至匹配检查之后。
- 补丁：
  ```diff
    	lruvec = mem_cgroup_lruvec(pgdat, memcg);
    	lrugen = &lruvec->lrugen;
    
  -	/* 4.19 has no per-type workingset counters; use the shared ones */
  -	mod_lruvec_state(lruvec, WORKINGSET_REFAULT, delta);
  -
    	min_seq = READ_ONCE(lrugen->min_seq[type]);
    	if ((token >> LRU_REFS_WIDTH) != (min_seq & (EVICTION_MASK >> LRU_REFS_WIDTH)))
    		goto unlock;
    
  +	/* 4.19 has no per-type workingset counters; use the shared ones */
  +	mod_lruvec_state(lruvec, WORKINGSET_REFAULT, delta);
  +
    	hist = lru_hist_from_seq(min_seq);
  ```
- 正确性论证：纯统计位置调整，仍在 rcu_read_lock 临界区内、lruvec 有效；不改变页回收/反馈逻辑（refaulted[] atomic 与 WORKINGSET_ACTIVATE/RESTORE 不变）；与上游语义对齐。
- 预期影响：WORKINGSET_REFAULT 统计准确，legacy 启发式不被污染【代码可证实】；无直接性能收益。
- 测试：编译矩阵 LRU_GEN=y/n；逻辑推演（第 6 节）。
- 状态：【代码可证实】。

#### PERF-S13a [P3][WALT] sched_get_cpu_util 死加载（补丁 P-C）
- 位置：kernel/sched/sched_avg.c:206（仅 CONFIG_SCHED_WALT 编译，Makefile:23）
- 证据：`util = rq->cfs.avg.util_avg;`（:206）紧接被 `util = rq->prev_runnable_sum + rq->grp_time.prev_runnable_sum;`（:209）覆盖，中间无读取。
- 机理：WALT 路径下白白触碰一条冷的 PELT cacheline（rq->cfs.avg.util_avg），增加一致性流量。
- 方案：L2（删死代码）。
- 补丁：
  ```diff
    	raw_spin_lock_irqsave(&rq->lock, flags);
    
  -	util = rq->cfs.avg.util_avg;
    	capacity = capacity_orig_of(cpu);
    
    	util = rq->prev_runnable_sum + rq->grp_time.prev_runnable_sum;
  ```
- 正确性论证：util 在 :209 重新赋值后才使用（:214），删除 :206 不影响任何读取；仍在 rq->lock 内；不涉导出符号（sched_get_cpu_util 非导出）。
- 预期影响：每次 sched_get_cpu_util 省一次冷 cacheline 加载【代码可证实】；量级小【估算】。
- 测试：编译矩阵 WALT=y（n 时文件不编译）。
- 状态：【代码可证实】。

### 5.2 高优先提案（行为/并发敏感，出草案但不盲改，需真机验证）

#### PERF-S01 [P1] WALT 唤醒不升频 / PL 默认关闭
- 证据：sched.h:2584 `if (!(flags & SCHED_CPUFREQ_WALT)) return;`；全树仅 walt.c:3431/core.c:3807/core.c:2765 产生该位 → cfs_rq_util_change/iowait/rt/deadline 通知空转。PL 是唯一唤醒即升频通道，但 cpufreq_schedutil.c:786/663 `if (!tunables->pl && flags & SCHED_CPUFREQ_PL) return;`，tunables kzalloc（:1139）且 sugov_init 未设 pl → **默认 0**。
- 机理：同簇唤醒（滑动 UI/RenderThread 常驻 gold）既不滚窗也不跨簇，频率最长滞后一个窗口；144Hz 窗口=8ms（walt.c:3735, msm_notifier.c:55 映射 FPS144）> 6.9ms 帧预算。
- 方案：**L1 先验证** userspace 写 `schedutil/pl=1`（确认 powerhal 是否已写）；L2 sugov_init 默认 `tunables->pl=1`；L3 enqueue_task_fair WALT 分支补 SCHED_CPUFREQ_WALT 通知。
- 正确性/风险：pl=1 增加升频 aggressiveness，功耗上升需真机权衡；不改签名/导出符号。
- 状态：路径【代码可证实】；pl 是否已被 userspace 置 1 与收益【推断，待真机验证】。

#### PERF-S02 [P1] hardirq 持全部 rq 锁调频
- 证据：walt.c:3368-3374 顺序锁全部 possible rq，:3423-3453 锁内对每 CPU cpufreq_update_util→sugov_fast_switch→writel_relaxed(REG_PERF_STATE)+arch_set_freq_scale，:3482 才解锁。125Hz。
- 机理：关中断+8 rq 锁+MMIO+跨簇频率标度串联；任何 CPU 的 ttwu/schedule 期间自旋 → 帧抖动。
- 方案：L3。**禁止裸移 cpufreq 循环到解锁后**（会缩小锁保护范围：notif_pending 清除与 util 读取失去 rq 锁保护）。正确做法：锁内快照 per-cpu {util, notif_pending} 到数组，解锁后用快照调频；需 sugov 接受快照的接口或确认 sugov update_lock 序列化充分。core_ctl_check 已在锁外（:3486，无需改）。
- 状态：临界区【代码可证实】；改造需并发论证，本次仅草案。

#### PERF-M01 [P0 条件] 多 kswapd 共享 mm_walk
- 证据：vmscan.c:3731 set_mm_walk kswapd 用 `&pgdat->mm_walk` + `VM_WARN_ON_ONCE(walk)`；厂商 kswapd[MAX_KSWAPD_THREADS=16]（mmzone.h:40/906），sysctl vm/kswapd_threads 默认 1（vmscan.c:154）。
- 机理：MGLRU 假设每 node 单 aging walker；kswapd_threads>1 时多线程并发 balance_pgdat→lru_gen_age_node 共用 mm_walk 与 mm_state.head/tail → nr_pages/batched 竞态、计数损坏。
- 方案：**L1 保持 kswapd_threads=1**（默认即安全）；L2 set_mm_walk 检测共享 walk 已被占用时回退私有 kzalloc walk，或 aging 加 per-pgdat 互斥。
- 状态：共享成立【代码可证实】；>1 才损坏（默认不触发）。

#### PERF-S03/S04 [P1] 全局锁 EAS 重扫 / hardirq O(N) 遍历
- S03：fair.c:13043 `raw_spin_lock(&migration_lock)`（全系统单一）内 find_energy_efficient_cpu 全簇扫描+compute_energy，HZ=250 每 4ms，misfit 常态。L2 per-rq 节流（每窗口一次）或 per-cluster 锁。
- S04：walt.c:3514 walt_fill_ta_data `list_for_each_entry(grp->tasks)` 无早退（对比 _set_preferred_cluster:2777 有 break），hardirq 关中断每 8ms 遍历 top-app 全线程。L2 增量维护 coloc_demand 总和或早退+降频。
- 状态：【代码可证实】；S03 量级依赖 EAS 是否生效（待真机）。

#### PERF-I02/I03 [P1] BFQ 默认 / UFS 门控
- I02：elevator.c:1011 厂商 BFQ 覆盖（注释 :988 已过时）；bfq_slice_idle=8ms（bfq-iosched.c:182）+low_latency。BFQ_GROUP_IOSCHED=y 提供 io.weight（Android 后台限流可能依赖）。L1 确认 io.weight 依赖后经 sysfs/elevator= 切 mq-deadline；L2 调 slice_idle。
- I03：ufshcd.c:261 perf delay=50ms；空闲后首请求 ufshcd_hold(async)→-EAGAIN→BLK_STS_DEV_RESOURCE→blk_mq_delay_run_hw_queue 3ms（blk-mq.c:1141/1291）。L1 启动窗口提高 clkgate_delay_ms_perf 或临时 clkgate_enable=0（sysfs 已具备）。
- 状态：【代码可证实】；延迟取舍【推断】。

#### PERF-N01/N02 [P1] BBRv3 接线缺失 / ECN 语义（非目标场景，登记）
- N01：全树无 `tcp_ca_event(sk, CA_EVENT_TLP_RECOVERY)` 发射点，rs.is_acking_tlp_retrans_seq 恒 false → bbr_run_loss_probe_recovery 永不可达（tcp_bbr.c:553/1385）。L2 在 tcp_process_tlp_ack（tcp_input.c:3759）损失分支补发射+置标志。
- N02：compat.h:36 WANTS_CE_EVENTS→NEEDS_ECN 使 tcp_ecn_send_syn 对所有 bbr 连接主动协商 ECN（bbr_can_use_ecn 仅查 TCP_ECN_OK 无路由门控）；握手行为相对 cubic 改变（有 tcp_ecn_fallback 兜底）。L2 加显式开关/路由门控。
- 状态：【代码可证实】；吞吐影响【推断】。BBR 不在冷启动/滑动/内存压力路径，常驻代价仅每 sock +40B（slab 对象不变）。

### 5.3 中低优先提案（紧凑）

- **PERF-M03 [P2]** extra_free_kbytes=300000（page_alloc.c:344）抬高水位 + lru_gen_min_ttl 默认 0（vmscan.c:4043）→ 冷启动 page cache 易早驱逐。L1 设 `/sys/kernel/mm/lru_gen/min_ttl_ms=1000~2000` 保护工作集并复核 extra_free_kbytes。【推断】
- **PERF-M07 [P2]** PGSCAN_ANON/FILE（vmscan.c:4402）与 PGSTEAL_ANON/FILE（:4577）无条件全局计，而 PGSCAN/STEAL_KSWAPD/DIRECT（:4396/:4574）受 global_reclaim 门控 → memcg 回收污染全局 per-type 计数。A/B 证据中 pgscan_kswapd(27927)<pgsteal_kswapd(391078) 违反 scan≥steal 不变量与此相关。L2 将 per-type 全局计数同样加 global_reclaim 门控（改变计数语义，需告知监控）。【代码可证实口径不一致；A/B 归因推断】
- **PERF-S05 [P2]** WALT 下 util_est 维护纯开销（读者全旁路：task_util_est 返回 ravg.demand_scaled，cpu_util WALT 提前 return）。**已被 sched_feat(UTIL_EST) 门控（fair.c:3809/3842）→ L1 `echo NO_UTIL_EST > /sys/kernel/debug/sched_features` 即可，无需改码**。【代码可证实】
- **PERF-S06 [P2]** EAS 唤醒重复 schedtune_task_boost≥4/per_task_boost≥3（每次 rcu+task_css）；find_best_target:6980 prefer_idle 首值被 7008 覆盖。L2 入口取一次存 fbt_env 下传，删死赋值。【代码可证实】
- **PERF-S07 [P2]** select_task_rq_fair:7969 首 EAS 块无条件 return → :7982 死块，EAS_PREFER_IDLE 虚设；EAS 返回 -1 钉 prev_cpu 无 wake_affine 回落。L2 删死块+首块改 `if (new_cpu<0) goto sd_loop;`。【代码可证实】
- **PERF-S08 [P2]** em_pd_energy（energy_model.h:103）线性扫 OPP；表严格升序（energy_model.c:117）可二分。L2 二分（上游代码）。【代码可证实】
- **PERF-S09 [P2]** find_best_target:7083 循环内 per_task_boost(远端 rq->curr) 会写 boost_period/expires（fair.c:3195）未持远端 rq 锁 → 与远端重新武装竞态；循环内重读 sched_clock。L2 循环外取 now+只读判定。【代码可证实；后果推断】
- **PERF-S10 [P2]** stale_ns（cpufreq_schedutil.c:1266）启动快照 22.5ms，窗口缩到 8ms 后 :750 仍用旧值 → 降频迟滞。L2 窗口切换时同步刷新（用 walt_tunables_fixup 回调，避免动 sched_set_refresh_rate 原型）。【代码可证实】
- **PERF-I04 [P2]** zram 默认 lzo（zram_drv.c:44），LZ4/ZSTD 已编入。L1 userspace comp_algorithm=lz4 或改默认。写路径良好（slot 锁外压缩、same-page 免压缩）。【代码可证实；收益推断】
- **PERF-I05 [P2]** f2fs 缓冲读不回填 extent（data.c:1716/1740 仅 PRECACHE/DIO 更新），压力下 balance_fs_bg shrink extent（segment.c:522）→ 冷读逐 dnode NAT+node 读。L3 回移上游读路径 extent 插入。【代码可证实+推断】
- **PERF-I06 [P2]** binder_alloc_new_buf_locked 持 alloc->mutex（binder_alloc.c:183）内 alloc_page(GFP_KERNEL)（:256）→ 压力下 direct reclaim 持锁阻塞同 proc 全部事务。L3 缓冲池/预分配或 memalloc_noreclaim。【代码可证实路径；延迟推断】
- **PERF-N03 [P2]** lost:12 模数快照（tcp.h:881, TCPCB_LOST_MASK=0xFFF）单 ACK 区间 lost>4096 回绕 → 损失率低估（backport 自创，上游无 tx.lost）。高 BDP WiFi 边界可达。L2 扩位或注释标明上限。【截断数学可证实；触发推断】
- **PERF-M04 [P3]** lru_deactivate_file_fn（swap.c:570-571）先 add_page_to_lru_list 后 SetPageReclaim → lru_gen_add_page 读不到 PG_reclaim，脏文件页落最老代。L2 先 SetPageReclaim 再 add（需核对上游顺序，本次仅草案）。【代码可证实】
- **PERF-M05 [P3]** arch_has_hw_pte_young（pgtable.h:795）编译期 IS_ENABLED(HW_AFDBM)，TCR_HA 运行时门控；SM8250 成立，异核 SoC 可致 aging 失效。L2 改运行时 cap。【代码可证实，本 SoC 无碍】
- **PERF-M06 [P3]** LRU_GEN_STATS=y 每 memcg×node 增数组+2×4KB bloom filter，仅 mem_cgroup_free 释放 → Android zombie memcg 累积。L1 量产关 LRU_GEN_STATS；L2 离线释放 filter。【代码可证实】
- **PERF-S11 [P3]** context_switch（core.c:3551）lru_gen_use_mm（mm_types.h:587 nodes_setall）无 lru_gen_enabled() runtime 守卫 → MGLRU 运行时关闭后仍白写共享 mm cacheline。L2 调用点加 `if (lru_gen_enabled())`（开启态 static_branch_likely=NOP）。mm_types.h 不能反向 include mm_inline.h，宜在 core.c 判定。【代码可证实；弹跳量级推断】
- **PERF-S12 [P3]** fork 每任务 2×kcalloc(nr_cpu_ids,4,GFP_KERNEL|__GFP_NOFAIL)（walt.c:2151）+ rollover 每窗口双数组清零。L2 合并一次分配；L3 nr_cpu_ids≤8 内嵌定长。【代码可证实】
- **PERF-S13bc [P3]** walt.c:162 sched_load_granule 恒 20µs 不随窗口重算 → 8ms 窗口仅用 401/1000 桶，rollover 仍 memset 1000B×2/CPU；fair.c:180 sysctl_walt_low_latency_task_threshold=0 → walt_low_latency_task 恒假，binder low_latency 唯一消费者失效。L2 granule 随窗口重算；L1 提高阈值激活（需真机公平性验证）。【代码可证实】
- **PERF-I07 [P3]** f2fs NAT down_read+journal trylock 重试（node.c:539）与后台 GC down_write(gc_lock)（segment.c:511）竞争。L2 错峰/限并发。【推断】
- **PERF-I08 [P3]** VM_MAX_READAHEAD=512KB（mm.h:2587，已高于上游，无需改）；page_cache_sync_readahead 受 blk_cgroup_congested 门控（readahead.c:515）memcg 拥塞退化单页；UFS pm_qos 经 singlethread wq 异步（ufs-qcom.c:1647）。L2 关注拥塞门控/QoS 时序。【代码可证实+推断】
- **PERF-N05/N06/N07 [P3]** BBRv3 死标志 ECT_PERMANENT/TCP_ECN_LOW（tcp.h:386）；cong_ops tso_segs 中部插入致后续偏移+8（外模块理论错位，树内 14 CA 同编无碍）；tx.in_flight 字节→包近似（方向安全量级小）。L3 清理/追加至结构尾。【代码可证实】
- **PERF-N04 [P3 修正]** tcp_skb_cb 尺寸断言**已存在**（tcp.c:3926 `BUILD_BUG_ON(sizeof(struct tcp_skb_cb) > FIELD_SIZEOF(struct sk_buff, cb))`，pahole 实测 56==56）；仅 tcp.h:834/873 陈旧注释（"44 bytes"/"24 bytes"）。L3 更新注释。**无需新增断言**。【代码可证实】
- **PERF-S14 [建议]** uclamp 保持关闭：已桥接 schedtune（core.c:1269），开启会 ① RT 顶频语义分叉（schedutil_cpu_util:349）② rq 撑大+cacheline 空洞（sched.h:966）③ clamp 对 cpu_util_freq_walt 无效（接口在效果无）。per-task 频率下限优先用已生效的 per_task_boost+schedtune.boost+S01 pl。【代码可证实】

### 5.4 L1 配置/运行时建议（PERF-C01 等，零代码风险）

| 项 | 当前 | 建议 | 证据/机理 | 风险 |
|---|---|---|---|---|
| DEBUG_LIST | y | **n（释放版）** | list.h:60 每次 list_add 走 __list_add_valid 5 次 CHECK_DATA_CORRUPTION（list_debug.c:20），LRU/sched/binder 热路径全覆盖 | 失去链表损坏调试检测（释放版可接受） |
| SCHEDSTATS | y | n（可选） | fair.c:895/935 每 enqueue/update_curr 累加计数 | 失去 /proc/schedstat 观测 |
| SCHED_DEBUG | y | 保留 | 提供 /proc/sched_debug，调优工具可能依赖 | 关之失观测，权衡 |
| LRU_GEN_STATS | y | n（量产，可选） | M06 bloom/数组开销 | 失 MGLRU 观测（A/B 依赖） |
| UTIL_EST | feat=true | NO_UTIL_EST（WALT） | S05 纯开销，sched_feat 已门控 | 零功能影响（读者旁路） |
| schedutil/pl | 0 | 1（先验证 powerhal） | S01 唤醒升频 | 功耗上升 |
| lru_gen/min_ttl_ms | 0 | 1000~2000 | M03 工作集保护 | 回收滞后略增 |
| zram comp_algorithm | lzo | lz4 | I04 压缩吞吐 | userspace 可能已设 |
| walt_low_latency_threshold | 0 | 提高（验证） | S13 激活 binder low_latency | 公平性影响 |
| kswapd_threads | 1 | **保持 1** | M01 多 walker 竞态 | >1 触发竞态 |
| MSM_IDLE_STATS | y | n（可选） | cpuidle 每次 idle 统计 | 失 idle 观测 |

> 安全加固项（SLAB_FREELIST_HARDENED/RANDOM、HARDENED_USERCOPY、VMAP_STACK、STRICT_RWX）**不建议关闭**——性能让位于正确性/安全。

---

## 6. 测试代码与编译 / 静态检查结果

### 6.1 编译矩阵
| 组合 | 结果 | 日志 |
|---|---|---|
| 基线（无补丁，LRU_GEN=y/WALT=y/BBR=y） | exit 0，2 告警（qcacld 宏重定义） | build-baseline-check.log |
| 补丁集（LRU_GEN=y/WALT=y/BBR=y） | **exit 0，零新增告警**（dm-verity:68 为初始提交既有嵌套注释告警） | build-perf-patchset-current.log |
| 补丁集 + CONFIG_LRU_GEN=n | **exit 0**（mm/workingset.o legacy 路径编译通过；1366 告警为全树重建既有厂商告警，非补丁引入） | build-perf-matrix-lru_gen-off.log |
| WALT=n | sched_avg.c 不编译（Makefile:23 obj-$(CONFIG_SCHED_WALT)），P-C 自动排除 | 静态确认 |
| 干净源码 + y-config（恢复编译） | exit 0，工作树还原至工作前状态 | build-perf-clean-restore.log |

补丁可应用性：`git apply --check 00-all-perf-fixes.patch` 对干净源码 **PASS**（约束 6：可应用 + 可编译均验证）。最终工作树保持干净，补丁以文件交付，由用户选择性 `git apply`。

- 工具：llvm-21 clang + GNU ld，与记录构建 byte-for-byte 同 flag。
- 静态分析工具 sparse/smatch/coccinelle 主机未安装（command -v 空）；以 `W=1` 告警审查 + 人工逻辑推演替代（见 6.3）。

### 6.2 编译期断言（已有/核实）
- tcp.c:3926 `BUILD_BUG_ON(sizeof(struct tcp_skb_cb) > FIELD_SIZEOF(struct sk_buff, cb))`（56==56，pahole 实测）。
- tcp_bbr.c:2250 `BUILD_BUG_ON(sizeof(struct bbr) > ICSK_CA_PRIV_SIZE)`（144==144）。
- workingset.c:234 `BUILD_BUG_ON(LRU_GEN_WIDTH + LRU_REFS_WIDTH > BITS_PER_LONG - EVICTION_SHIFT)`。
- page-flags-layout.h `#error` 位域溢出守卫（NR_PAGEFLAGS≈24≪64）。

### 6.3 逻辑测试与推演（4.19 无 KUnit）
- **P-B 逻辑推演**：lru_gen_refault 输入 shadow token。case1 token 匹配 min_seq → 计 REFAULT+ACTIVATE（与改前同）；case2 token 不匹配 → goto unlock，改前计 REFAULT（错）、改后不计（对，对齐上游）；case3 memcg_id 不符/pgdat 不符 → 提前 return，两版均不计。结论：仅 case2 行为修正，无副作用。
- **P-C 逻辑推演**：sched_get_cpu_util 中 util 在 :209 赋值后于 :214 首次读取，:206 删除不影响数据流。
- **P-A 逻辑推演**：alloc_workqueue flag 回退；WQ_UNBOUND 路径 wq_select_unbound_cpu 摊载；WQ_MEM_RECLAIM rescuer 保留；max_active=num_online_cpus() 恢复原并发上限。
- 用户态模拟测试（可选，主机侧）：见 `patches/perf-20261117/test-workingset-count.c`（模拟 token 匹配/不匹配的计数分支，验证 P-B 语义）。
- kselftest 标注（供后续人工执行，本次不运行于设备）：`tools/testing/selftests/mm`（workingset/refault 相关）、`sched`、`futex`；4.19 无 KUnit，mm 自测项有限。

### 6.4 埋点设计（仅代码，写入验证计划）
- MGLRU：tracepoint `mm_vmscan_lru_gen_scan`/现有 mm_vmscan_*；判定阈值 pgscan_kswapd≥pgsteal_kswapd（M07 修复后应成立）。
- 调度：sched_switch/sched_wakeup + schedutil freq；判定 144Hz 帧内升频延迟<6.9ms（S01）。
- verity：dm-verity verify work 队列延迟（block/block_rq_complete + workqueue execute）；判定冷启动校验多核分布（I01）。

---

## 7. 第二遍审核记录（独立审核者视角）

| # | 审核意见 | 采纳/驳回 | 理由 |
|---|---|---|---|
| R1 | 子代理 N04 称"无 tcp_skb_cb 尺寸断言"是否属实？ | **驳回子代理结论** | 主审核实 tcp.c:3926 断言已存在；子代理仅查 tcp.h 漏 tcp.c。修正为 P3 注释清理，不新增断言（避免冗余）。 |
| R2 | S05 建议 L2 加 #ifdef CONFIG_SCHED_WALT 短路 util_est 是否过度设计？ | **驳回 L2，采纳 L1** | util_est_enqueue/dequeue 已被 sched_feat(UTIL_EST) 门控（fair.c:3809/3842），L1 NO_UTIL_EST 即达同等效果，加 ifdef 冗余且增风险。 |
| R3 | S02 直接把 cpufreq 循环移到解锁后是否安全？ | **驳回裸移** | 缩小锁保护范围：notif_pending 清除与 util 读取失 rq 锁保护，违反"不得缩小锁保护范围换性能"。改为 L3 快照方案草案，本次不落地。 |
| R4 | P-A 回退用户提交 d936a991331c 是否越权？ | **采纳但显式标注** | 证据充分（per-cpu 绑定+共享 highpri 池+max_active 语义均核实）；属可逆 git 操作；报告与摘要明确"修订用户提交，采纳与否由用户决定"。 |
| R5 | P-B 移动 REFAULT 计数是否影响 MGLRU 反馈环？ | **采纳，确认无副作用** | MGLRU 反馈用 refaulted[] atomic（:287）与 WORKINGSET_ACTIVATE/RESTORE，均不变；仅 vmstat REFAULT 位置对齐上游。 |
| R6 | P-C 删除 :206 是否可能 util 在 206~209 间被读？ | **采纳，确认安全** | 207 为 capacity 赋值，209 重赋 util，214 才首读；无中间读取。 |
| R7 | EAS 相关发现（S03/S06/S07/S08）量级是否可靠？ | **部分采纳，加前提** | EAS 是否生效取决于 DT dynamic-power-coefficient（vendor dts 悬空链接无法核实）+ qcom-cpufreq-hw.c:361 负 nr_states 绕过检查隐患。标注"量级依赖 EAS 生效，待真机"，不夸大。 |
| R8 | M01 多 kswapd 竞态是否默认触发？ | **采纳，降级表述** | 默认 kswapd_threads=1（vmscan.c:154）不触发；仅 sysctl>1 触发。标 P0(条件)，L1 保持 1 即安全。 |
| R9 | A/B 证据（ab/）能否作量化结论？ | **驳回量化，仅定性** | 单次顺序运行（B 在 A 后，swap 基线更高），且 M02 使 MGLRU-on/off refault 口径不同；仅作定性佐证，明确 caveat。 |
| R10 | M07 统计口径修复是否改变监控语义？ | **采纳，加告知** | per-type 全局计数加 global_reclaim 门控会改变 memcg 回收计数归属；本次仅草案，落地需告知监控工具。 |
| R11 | 补丁是否破坏导出符号/KMI？ | **采纳，确认无** | 三补丁均函数体内改动，无签名/EXPORT_SYMBOL 变更；非 GKI 无 KMI 约束；sched_get_cpu_util 非导出。 |
| R12 | 是否存在范围外重构/过度设计？ | **采纳克制原则** | 仅落地 3 项确定生效安全补丁；行为敏感项（S01/S02/S03/S07 等）出草案不盲改；BBR 非目标场景仅登记；uclamp 明确不开启。 |
| R13 | dm-verity:68 告警是否补丁引入？ | **驳回（非引入）** | git blame 属初始 bring-up 9b07f14fc025，行 68 嵌套注释，与改动行 1192 无关；补丁零新增告警。 |
| R14 | S11 加 lru_gen_enabled() 守卫收益是否值得落地？ | **驳回落地，留草案** | 开启态（默认）static_branch_likely=NOP 无收益，仅关闭态省 store；P3 边际收益，避免为微益增 include 依赖。 |

审核结论：3 项落地补丁正确性/并发/ABI/可编译性均通过；高价值行为敏感项以草案+真机验证计划交付，未盲改；子代理两处结论（N04/S05-L2）经核实修正。

---

## 8. 待真机验证清单（明确标注：不在本次执行范围）

> 以下均为**计划**，本次未执行任何设备命令。工具建议 perfetto/atrace/simpleperf（仅建议）。

| 项 | 指标定义 | 对照组 | 通过/回滚判据 |
|---|---|---|---|
| I01 verity | 冷启动 system/vendor 读校验延迟、kverityd 多核分布 | UNBOUND vs per-cpu HIGHPRI | 冷启动首屏延迟不劣化；多核摊载恢复→采纳 P-A |
| S01 pl | 144Hz 滑动帧内升频延迟、jank 率、功耗 | pl=0 vs pl=1（先查 powerhal） | 帧内升频<6.9ms 且功耗可接受→采纳 |
| S02/S04 | hardirq 关中断时长、帧抖动 | 改前/改后 | 最长关中断区下降→采纳快照方案 |
| M01 | kswapd_threads>1 时 VM_WARN/计数一致性 | threads=1 vs >1 | >1 无竞态→采纳守卫；否则保持 1 |
| M03 | 冷启动 page cache 命中、refault | min_ttl=0 vs 1000~2000 | 工作集保护改善→采纳 |
| M07 | pgscan_kswapd≥pgsteal_kswapd 不变量 | 改前/改后 | 不变量成立→采纳 |
| I02 | 冷启动 I/O 延迟、后台 io.weight 限流 | BFQ vs mq-deadline | 确认 io.weight 依赖后取舍 |
| I03 | 空闲后首请求延迟 | clkgate 50ms vs 启动窗口禁用 | 首请求延迟下降→采纳 |
| C01 | 释放版吞吐/延迟 | DEBUG_LIST=y vs n | 无回归→采纳 n |
| EAS 生效 | dmesg "no EM found"、sched_energy_aware、energy_model debugfs | — | 确认 EAS 是否生效，再评估 S03/S06/S07/S08 |
| BBR N01/N02 | TLP 恢复可达性、ECN 握手 | 改前/改后 | 非目标场景，独立网络回归 |

---

## 附录 A：既有静态证据（来源与时间点）

- `baseline-snapshot.txt`（2026-11-16 13:08，设备采集）：4.19.325-perf++ #20，Adreno650，144Hz，lsmod_count=0，dmesg_err_count=917。
- `ab/`（2026-11-16 18:36，设备采集，mempress 3072MB/14s，min_free_kbytes=2500000）：MGLRU on(A) vs off(B) 单次顺序运行。
  - kswapd stime：A 133 vs B 194 ticks；pgmajfault：A 4136 vs B 13638；pswpin：A 4115 vs B 12713；pswpout：A 345695 vs B 292422；pgrefill：A 798656 vs B 611105。
  - 口径异常：A pgscan_kswapd=27927 < pgsteal_kswapd=391078（违反 scan≥steal）；pgscan_anon+file=446832≫pgscan_kswapd → 指向 M07。
  - lru_gen_refault：A 5867 vs B 76849，但 M02 使两路径口径不同，**不可直接比较**。
  - caveat：单次、顺序、swap 基线不同；仅定性佐证，不作量化结论。
- `CHANGES-20261117.md`：MGLRU/BBRv3/verity/音频提交说明与构建 #8 记录。

## 附录 B：交付物清单
- 报告：`perf-report-20261117.md`（本文件）
- 补丁：`patches/perf-20261117/00-all-perf-fixes.patch`、`01-verity-wq-unbound-revert.patch`、`02-workingset-refault-count-order.patch`、`03-sched-avg-dead-load.patch`
- 编译脚本：`perf-build.sh`、`perf-matrix-lru-off.sh`
- 编译日志：`build-baseline-check.log`、`build-perf-patchset-current.log`、`build-perf-matrix-lru_gen-off.log`
- 逻辑测试：`patches/perf-20261117/test-workingset-count.c`
- config 备份：`config-perf-matrix.bak`