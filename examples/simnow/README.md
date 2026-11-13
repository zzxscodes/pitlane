# pitlane_simnow

Self-contained SimNow 7×24 example: MD → strategy → real IOC → TD accept, with local/gate/roundtrip T2T.

## Layout

```
examples/simnow/
  main.cpp                 # closed-loop host
  ctp/                     # CTP gateway on MdEngine and TdClient
  conf/paper.conf          # center loop, no broker
  conf/simnow_7x24.conf.example
  conf/simnow_7x24.conf    # local secrets (gitignored)
  run/                     # flow_*, desk.bin, wire.log (gitignored contents)
  README.md
```

Account, engines, and `WireLog` are pitlane framework types. `ctp/` is this example's gateway: it subclasses the library and talks to CTP. The library does not ship that code. `mode = paper` runs the trading center (`Center` + `TapeMd` + `PaperVenue`) with no broker. `mode = live` uses the same desk with this gateway. Progress is written to `run/wire.log`.

```bash
../../build/pitlane_simnow conf/paper.conf
```

## Build

Needs CTP `_se` headers/libs (`PITLANE_CTP_API_ROOT`). From repo root:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DPITLANE_BUILD_CTP=ON
cmake --build build -j --target pitlane_simnow
```

## Run

```bash
cd examples/simnow
cp -n conf/simnow_7x24.conf.example conf/simnow_7x24.conf   # edit password
../../build/pitlane_simnow conf/simnow_7x24.conf
```

Optional: `SIMNOW_PASSWORD`, `SIMNOW_INVESTOR`, `SIMNOW_WORK_DIR`.

Runtime files only under `./run` (or `work_dir` in conf).
