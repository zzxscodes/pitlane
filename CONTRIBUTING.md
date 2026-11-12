# Contributing

Keep changes composition-first, fixed-capacity, and fail-closed. Do not add heap, `std::map` / `std::string` / `std::vector`, or mutexes on helper hot paths. The caller owns ticks, clocks, and send decisions.

## Docs

Update `README.md` with `README_CN.md`, and `doc/API.md` with `doc/API_CN.md`, in the same change. Regenerate the site:

```bash
python3 doc/gen_site.py
```

## Build / test

```bash
cmake -S . -B build -DPITLANE_NATIVE=OFF -DPITLANE_LTO=OFF
cmake --build build -j
cd build && ctest --output-on-failure
```

## Style

Match existing headers. Run `clang-format` on touched files (`.clang-format` is in the tree). Prefer C++17.

## Security

See `SECURITY.md`.
