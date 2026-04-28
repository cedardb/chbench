# chbench

A CH-benCHmark driver: TPC-C OLTP load mixed with the 22 CH analytical queries, against CedarDB or PostgreSQL.

## Build

```
make
```

Requires a C++20 compiler and libpq headers (Debian/Ubuntu: `apt install libpq-dev`).

## Populate the database

The driver does not load data. Use a separate TPC-C loader to populate the target database with a chosen scale factor (= warehouse count) before running. The `sql/functions_<system>.sql` stored procedures are installed automatically on first run.

## Run

```
./chbench --conn "host=/tmp port=5432 dbname=chbench" --scale 8 --olap-threads 2
```

Required:
- `--conn <str>` — libpq connection string
- `--scale <n>` — scale factor; equals the warehouse count and the number of TPC-C client threads

Common options:
- `--olap-threads <n>` — analytical clients (default 0)
- `--warmup <sec>` / `--measure <sec>` — defaults 5 / 30
- `--target-rate <n>` — global TPC-C tx/s cap (0 = uncapped)
- `--pipeline-depth <n>` — TPC-C statements per libpq pipeline batch (default 1)
- `--target-system cedar|postgres` — selects `functions_<s>.sql` (default `cedar`)
- `--csv <path>` — per-statement latency CSV output (default `chbench-stats.csv`)
- `--script-dir <path>` — SQL script directory (default `<binary-dir>/sql`)

## Output

Live throughput is printed each second during warmup and measurement. After measurement, a per-transaction / per-query latency table is printed and appended to the CSV file. OLAP queries that fail (e.g. server out of memory) are logged to stderr and the thread continues with the next query; TPC-C errors abort the run.
