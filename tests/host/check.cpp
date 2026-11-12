#include "pitlane/pitlane.hpp"

#include <cstdio>
#include <cstring>

static int g_fails = 0;

static void expect(bool cond, const char* what) {
    if (cond) return;
    std::fprintf(stderr, "fail: %s\n", what);
    ++g_fails;
}

static void check_affinity() {
    const int n = pitlane::cpu_count();
    expect(n > 0, "cpu count");
    const int self = pitlane::current_cpu();
    expect(self >= 0 && self < n, "current cpu");
    expect(pitlane::pin_thread(self), "pin thread");
    const int one = self;
    expect(pitlane::pin_process_mask(&one, 1), "pin process mask");
    int isolated[64]{};
    const int ni = pitlane::isolated_cpus(isolated, 64);
    expect(ni >= 0, "isolated read");
}

static void check_priority() {
    const int nice = pitlane::get_nice();
    expect(nice != 0x7fffffff, "nice");
    (void)pitlane::set_fifo(1);
    (void)pitlane::set_fifo_thread(1);
    (void)pitlane::set_other();
    expect(pitlane::get_policy() >= 0, "policy");
}

static void check_irq_hugepage() {
    int irqs[8]{};
    const int n = pitlane::irq_list(irqs, 8);
    expect(n >= 0, "irq list");
    if (n > 0) {
        char list[64]{};
        expect(pitlane::irq_affinity_list_get(irqs[0], list, 64) >= 0, "irq get");
    }
    long pages = -1;
    expect(pitlane::hugepage_count_get(pages) && pages >= 0, "hugepage count");
    long kb = 0;
    expect(pitlane::hugepage_size_kB(kb) && kb > 0, "hugepage size");
    char cmd[256]{};
    expect(pitlane::cmdline_get(cmd, 256) > 0, "cmdline");
    (void)pitlane::thp_disable();
    (void)pitlane::thp_is_never();
}

static void check_memlock_numa() {
    (void)pitlane::mem_raise_limit();
    (void)pitlane::mem_lock_all();
    (void)pitlane::mem_unlock_all();
    alignas(64) unsigned char page[4096]{};
    (void)pitlane::mem_lock_range(page, sizeof page);
    (void)pitlane::mem_unlock_range(page, sizeof page);
    expect(pitlane::numa_node_count() >= 1, "numa nodes");
    const int cpu = pitlane::current_cpu();
    if (cpu >= 0) {
        const int node = pitlane::numa_node_of_cpu(cpu);
        expect(node >= -1, "numa node of cpu");
        expect(pitlane::numa_local_node() == node || pitlane::numa_local_node() >= -1, "local node");
    }
}

int main() {
    check_affinity();
    check_priority();
    check_irq_hugepage();
    check_memlock_numa();
    if (g_fails) {
        std::fprintf(stderr, "%d host check(s) failed\n", g_fails);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
