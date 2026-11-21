#pragma once

// 火焰数据：把带层级的作用域计时累积成 folded stack（root;a;b <ns>），
// 可直接喂给 flamegraph.pl 或任何支持 folded 格式的查看器。
//
// 用法：
//   const int t = flame.enter("decode", now_ns);
//   ... 工作 ...
//   flame.leave(t, later_ns);
//
// 定长、无堆；标签按（父节点 + 标签）唯一化，最多 64 个节点、8 层。
// 调用方提供纳秒时间，采样开销只有两次读取。

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace pitlane {

class FlameBook {
public:
    static constexpr int kNodes = 64;
    static constexpr int kDepth = 8;
    static constexpr int kLabel = 24;

    void reset() noexcept {
        for (int i = 0; i < kNodes; ++i) nodes_[i] = Node{};
        for (int i = 0; i < kDepth; ++i) stack_[i] = Frame{};
        n_ = 0;
        depth_ = 0;
    }

    // 进入作用域，返回 leave 需要的令牌；无空间时返回 -1（计时被跳过）。
    int enter(const char* label, uint64_t now_ns) noexcept {
        if (!label || !label[0] || depth_ >= kDepth) return -1;
        const int parent = depth_ > 0 ? stack_[depth_ - 1].node : -1;
        int node = find_node(parent, label);
        if (node < 0) {
            if (n_ >= kNodes) return -1;
            node = n_++;
            nodes_[node].parent = parent;
            std::snprintf(nodes_[node].label, kLabel, "%s", label);
        }
        const int tok = depth_;
        stack_[tok].node = node;
        stack_[tok].enter_ns = now_ns;
        ++depth_;
        return tok;
    }

    // 退出作用域。令牌必须来自同一层 enter；乱序调用被忽略。
    void leave(int token, uint64_t now_ns) noexcept {
        if (token < 0 || token >= depth_) return;
        // 丢弃 token 之上的未闭合帧（异常路径），避免栈错位。
        depth_ = token + 1;
        const Frame& f = stack_[token];
        if (now_ns < f.enter_ns) {
            depth_ = token;
            return;
        }
        const uint64_t dt = now_ns - f.enter_ns;
        Node& nd = nodes_[f.node];
        nd.total_ns += dt;
        ++nd.calls;
        if (nd.parent >= 0) nodes_[nd.parent].child_ns += dt;
        depth_ = token;
    }

    int nodes() const noexcept { return n_; }
    int depth() const noexcept { return depth_; }

    // 输出 folded stack，每行 "<path> <self_ns>"。返回写入字节数。
    int folded(char* out, int cap) const noexcept {
        if (!out || cap <= 0) return 0;
        int used = 0;
        for (int i = 0; i < n_; ++i) {
            const uint64_t self = nodes_[i].total_ns > nodes_[i].child_ns ? nodes_[i].total_ns - nodes_[i].child_ns : 0;
            if (self == 0) continue;
            const int w = write_path(out, used, cap, i);
            if (w < 0) break;
            used = w;
            used += std::snprintf(out + used, static_cast<std::size_t>(cap - used), " %llu\n",
                                  static_cast<unsigned long long>(self));
            if (used >= cap) {
                used = cap;
                break;
            }
        }
        return used;
    }

private:
    struct Node {
        char label[kLabel]{};
        int32_t parent{-1};
        uint64_t total_ns{0};
        uint64_t child_ns{0};
        uint64_t calls{0};
    };
    struct Frame {
        int32_t node{-1};
        uint64_t enter_ns{0};
    };

    int find_node(int parent, const char* label) const noexcept {
        for (int i = 0; i < n_; ++i) {
            if (nodes_[i].parent == parent && std::strcmp(nodes_[i].label, label) == 0) return i;
        }
        return -1;
    }

    int write_path(char* out, int used, int cap, int node) const noexcept {
        int chain[kDepth + 1]{};
        int len = 0;
        for (int i = node; i >= 0 && len <= kDepth; i = nodes_[i].parent) chain[len++] = i;
        for (int j = len - 1; j >= 0; --j) {
            const int w = std::snprintf(out + used, static_cast<std::size_t>(cap - used), "%s%s", j == len - 1 ? "" : ";",
                                        nodes_[chain[j]].label);
            if (w < 0 || used + w >= cap) return -1;
            used += w;
        }
        return used;
    }

    Node nodes_[kNodes]{};
    Frame stack_[kDepth]{};
    int n_{0};
    int depth_{0};
};

} // namespace pitlane
