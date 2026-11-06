#include "pitlane/pitlane.hpp"

#include <cstdio>
#include <unistd.h>

// A desk file plus one paper name. Prices are integer ticks. Times are microseconds.

int main() {
    const char* path = "pitlane_demo.bin";
    ::unlink(path);
    pitlane::MapInfo map{};
    if (pitlane::map_create(path, pitlane::desk_bytes(), &map) != 0) return 1;
    pitlane::Desk* desk = pitlane::desk_view(&map, 1);
    if (!desk) return 1;

    pitlane::IdMint mint(1, 1);
    pitlane::Intent in{};
    in.tag = mint.next(4, 10 * 3600);
    in.name = 1;
    in.acct = 2;
    in.px = 100;
    in.qty = 5;
    in.stamp_us = 10 * 3600 * 1000000ll;
    in.side = static_cast<uint8_t>(pitlane::Side::Buy);
    in.act = static_cast<uint8_t>(pitlane::Act::Enter);
    in.style = static_cast<uint8_t>(pitlane::Style::Day);
    desk->intents.publish(in, 0x1);

    pitlane::VenueCfg cfg{};
    cfg.lot = 1;
    pitlane::PaperVenue venue(cfg);
    const pitlane::Rung asks[] = {{100, 5}};
    venue.set_levels(1, nullptr, 0, asks, 1);
    pitlane::Report rep[4]{};
    const pitlane::VenueOut out = venue.admit(in, in.stamp_us, rep, 4);
    std::printf("tag %llu reports %d phase %u qty %lld\n",
                static_cast<unsigned long long>(in.tag), out.reports, rep[0].phase,
                static_cast<long long>(rep[0].qty));
    pitlane::map_release(&map);
    ::unlink(path);
    return out.why == pitlane::Why::None ? 0 : 1;
}
