# Host quiet-machine checklist

pitlane's `host/` helpers only change the **calling** process or thread. They do not edit GRUB, stop `irqbalance`, or create cpusets. Use this checklist with the OS before relying on `conf_apply_host`.

1. **Isolate cores** — `isolcpus` / `nohz_full` / `rcu_nocbs` for trading CPUs; keep IRQs off those cores.
2. **IRQ** — pin NIC / storage IRQs away from trading cores (`irq_affinity_list_set`). Disable or constrain `irqbalance`.
3. **Huge pages** — reserve explicit hugepages (`hugepage_count_ensure`); keep THP off (`thp_disable`). pitlane mappings use hugetlbfs / `MAP_HUGETLB`, not THP.
4. **Lock memory** — raise `RLIMIT_MEMLOCK`, then `mem_lock_all` or per-map `MapOpt::lock`.
5. **NUMA** — bind process and mappings to one node (`numa` helpers / `MapOpt::numa_node`).
6. **RT** — `SCHED_FIFO`/`RR` only with CAP_SYS_NICE; pair with affinity.
7. **Frequency** — performance governor; avoid AVX-512-only paths (pitlane caps auto-vector width at 256 bits under `PITLANE_NATIVE`).

See also notes §1–4, §7, §32. Chinese: `HOST_TUNING_CN.md`.
