# 主机静默配置清单

pitlane 的 `host/` 只改**当前**进程或线程，不改 GRUB、不停 `irqbalance`、不建 cpuset。上线前先按本清单配好操作系统，再视需要调用 `conf_apply_host`。

1. **核隔离** — 交易核用 `isolcpus` / `nohz_full` / `rcu_nocbs`；IRQ 离开这些核。
2. **中断** — 网卡/存储 IRQ 绑到非交易核（`irq_affinity_list_set`）；关掉或限制 `irqbalance`。
3. **大页** — 预留显式大页（`hugepage_count_ensure`）；关掉 THP（`thp_disable`）。库映射走 hugetlbfs / `MAP_HUGETLB`，不用 THP。
4. **锁页** — 抬高 `RLIMIT_MEMLOCK`，再 `mem_lock_all` 或按映射 `MapOpt::lock`。
5. **NUMA** — 进程与映射绑同一节点。
6. **实时** — `SCHED_FIFO`/`RR` 需权限；与亲和性一起用。
7. **频率** — performance 调速；避免只靠 AVX-512 的路径（`PITLANE_NATIVE` 下自动向量宽上限 256 位）。

详见笔记 §1–4、§7、§32。英文：`HOST_TUNING.md`。