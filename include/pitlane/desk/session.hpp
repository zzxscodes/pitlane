#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pitlane {

// Broker-neutral session and runtime paths. Venue credentials, fronts, and
// venue-private keys belong to the adapter that consumes them; the core only
// sees instruments and filesystem locations. Startup only: the hot path does
// not reload this.
struct Account {
    std::vector<std::string> instruments;

    std::string work_dir{"./run"};
    std::string flow_md_dir;
    std::string flow_td_dir;
    std::string desk_path;
    std::string log_path;
    int host_cpu{-1};
};

struct StratCfg {
    int64_t buy_px_x10000{999999999};
    int64_t max_spread_x10000{200};
    int64_t order_qty{1};
    int64_t min_order_gap_us{3000000};
    int max_live_orders{2};
    int t2t_samples{50};
    int connect_timeout_s{30};
    int paper{0}; // 0 live counter, 1 paper center
    // Opening equity for the flow's performance curve, in the same units as
    // PnL marks (1e-4 price x quantity). The curve and its drawdown ratio are
    // measured against this, not against the PnL swing itself.
    int64_t capital{1000000};
};

bool load_account_conf(const char* path, Account& acct, StratCfg& strat, std::string& err);

// PITLANE_WORK_DIR overrides the conf's work_dir. Adapter-owned credentials
// are read by the adapter (see pitlane-trading/adapters/<name>/config.hpp).
void apply_account_env(Account& acct);

// Fill empty path fields from work_dir. Flow dirs gain a trailing slash.
void resolve_runtime_paths(Account& acct);

} // namespace pitlane
