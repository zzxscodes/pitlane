#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pitlane {

// Broker-neutral session and runtime paths. A conf file supplies fronts and secrets.
// Startup only: the hot path does not reload this.
struct Account {
    std::string broker_id;
    std::string investor_id;
    std::string password;
    std::string new_password; // set only to rotate an expired counter password, then clear it
    std::string app_id;
    std::string auth_code;
    std::string md_front;
    std::string td_front;
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
    int paper{0}; // 0 live counter, 1 paper center (paper matcher role)
};

bool load_account_conf(const char* path, Account& acct, StratCfg& strat, std::string& err);

// PITLANE_PASSWORD / PITLANE_INVESTOR / PITLANE_WORK_DIR, else the SIMNOW_* aliases.
void apply_account_env(Account& acct);

// Fill empty path fields from work_dir. Flow dirs gain a trailing slash.
void resolve_runtime_paths(Account& acct);

} // namespace pitlane
