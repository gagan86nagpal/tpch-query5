# TPC-H Query 5 — End-to-End Setup & Benchmark Guide

This document walks through everything needed to take this repo from a fresh
clone to a fully working multithreaded benchmark of TPC-H Query 5 at
**Scale Factor 2 (SF2)**.

The reference run on a Linux x86_64 box with `g++ 12.2.0` produced
**5 ASIA nations**, single-thread query time ≈ **162 ms**, 4-thread ≈ **108 ms**.

---

## 0. Prerequisites

| Tool                 | Tested version | How to check          |
|----------------------|----------------|-----------------------|
| `g++` (C++11+)       | 12.2.0         | `g++ --version`       |
| `gcc` (for dbgen)    | 12.2.0         | `gcc --version`       |
| CMake                | ≥ 3.10         | `cmake --version`     |
| GNU Make             | 4.3            | `make --version`      |
| `git`                | 2.39+          | `git --version`       |
| pthreads             | system         | already on Linux/macOS |

On Debian/Ubuntu, install the lot with:

```bash
sudo apt update
sudo apt install -y build-essential cmake git
```

You also need ~10 GB free disk for the SF2 data files (only `lineitem.tbl`
takes 1.5 GB) and a few hundred MB more for build artifacts.

---

## 1. Clone this repo

```bash
git clone https://github.com/<your-fork>/tpch-query5.git
cd tpch-query5
```

---

## 2. Build & run the TPC-H data generator (`tpch-dbgen`) at SF2

The reference dbgen tool is hosted at https://github.com/electrum/tpch-dbgen.

```bash
cd ..
git clone --depth 1 https://github.com/electrum/tpch-dbgen.git
cd tpch-dbgen
cp makefile.suite makefile

# Configure the build (one of the few mandatory edits):
sed -i \
  -e 's/^CC      =/CC      = gcc/' \
  -e 's/^DATABASE=/DATABASE= ORACLE/' \
  -e 's/^MACHINE =/MACHINE = LINUX/' \
  -e 's/^WORKLOAD =/WORKLOAD = TPCH/' \
  makefile

make
```

This produces two binaries: `dbgen` (data generator) and `qgen` (query
template tool — not needed here).

Now generate SF2 data into a clean directory. `dbgen` looks for `dists.dss`
in its current directory, so either `cd` into the dbgen tree or pass `-b`:

```bash
mkdir -p ~/tpch-data-sf2
cd ~/tpch-data-sf2
~/tpch-dbgen/dbgen -s 2 -b ~/tpch-dbgen/dists.dss -f
ls -lh *.tbl
```

Expected output:

| Table         | SF2 row count   | Size   |
|---------------|-----------------|--------|
| `region.tbl`  | 5               | 389 B  |
| `nation.tbl`  | 25              | 2.2 KB |
| `supplier.tbl`| 20,000          | 2.7 MB |
| `customer.tbl`| 300,000         | 47 MB  |
| `part.tbl`    | 400,000         | 47 MB  |
| `partsupp.tbl`| 1,600,000       | 229 MB |
| `orders.tbl`  | 3,000,000       | 330 MB |
| `lineitem.tbl`| 11,997,996      | 1.5 GB |

Query 5 only reads the first six (`part` and `partsupp` are unused).

> Each `.tbl` line is **pipe-delimited** with a trailing `|` before the
> newline, e.g.
> `1|310379|15395|1|17|23619.12|0.04|0.02|N|O|1996-03-13|...|`.

---

## 3. Build the project

From the repo root:

```bash
mkdir -p build
cd build
cmake ..        # defaults to Release with -O3 (see CMakeLists.txt)
make -j
```

You should get a `build/tpch_query5` binary (~50 KB).

If you want a debug build instead:

```bash
cmake -DCMAKE_BUILD_TYPE=Debug ..
make -j
```

> **Important:** always benchmark with the Release build. A Debug build is
> ~10× slower and gives misleading timings.

---

## 4. Run the benchmark

### Single-threaded

```bash
./tpch_query5 \
  --r_name ASIA \
  --start_date 1994-01-01 \
  --end_date 1995-01-01 \
  --threads 1 \
  --table_path ~/tpch-data-sf2 \
  --result_path ../results/result_t1.txt
```

### 4-threaded

```bash
./tpch_query5 \
  --r_name ASIA \
  --start_date 1994-01-01 \
  --end_date 1995-01-01 \
  --threads 4 \
  --table_path ~/tpch-data-sf2 \
  --result_path ../results/result_t4.txt
```

The result file uses the same `n_name|revenue` pipe format as the input data,
sorted by revenue descending.

### Expected output (SF2, ASIA, 1994)

```
n_name|revenue
INDONESIA|115979499.6518
CHINA|109568736.2163
INDIA|106258458.1656
JAPAN|104738341.0311
VIETNAM|98052109.1293
```

These five values must be identical regardless of thread count. To prove
correctness:

```bash
diff results/result_t1.txt results/result_t4.txt && echo "results match"
```

---

## 5. Reference timings (SF2, Linux x86_64, g++ 12.2 -O3)

Three runs averaged on a 96-core Xeon-class box:

| Threads | Query exec time | Speedup vs 1T |
|--------:|----------------:|--------------:|
| 1       | ~162 ms         | 1.00×         |
| 2       | ~128 ms         | 1.27×         |
| 4       | ~108 ms         | 1.50×         |
| 8       |  ~97 ms         | 1.67×         |

> Why doesn't speedup scale linearly? Only the lineitem scan
> (~12M rows) is parallelized. Building hash maps for region / nation /
> supplier / customer / orders runs single-threaded, accounting for ~70 ms.
> By Amdahl's law, that serial portion caps achievable speedup at ~2.3×.

Data-load time (`readTPCHData`) is reported separately and is dominated by
parsing 1.5 GB of `lineitem.tbl` from disk — it sits around 3.2-3.5 s
regardless of `--threads`, because the loader itself is serial.

---

## 6. Project layout

```
tpch-query5/
├── CMakeLists.txt        # Release build with -O3 by default
├── include/
│   └── query5.hpp        # Struct-typed row definitions + public API
├── src/
│   ├── main.cpp          # CLI front-end + timing
│   └── query5.cpp        # Loaders, parser, parallel join
├── results/              # Output written here at runtime
└── STEPS.md              # This file
```

## 7. How the query is executed

The C++ implementation follows a textbook hash-join order:

1. **Region filter**: linear scan of the 5-row region table to resolve the
   `r_name` argument to an `r_regionkey`.
2. **Nation filter**: keep only nations whose `n_regionkey` matches step 1
   (5 rows for ASIA). Build `nation_key → nation_name`.
3. **Supplier filter**: keep only suppliers in those nations. Build
   `supp_key → nation_key`.
4. **Customer filter**: keep only customers in those nations. Build
   `cust_key → nation_key`.
5. **Order filter**: keep only orders within `[start_date, end_date)`
   AND whose `cust_key` is in the eligible-customer map. Build
   `order_key → cust_key`. ISO `YYYY-MM-DD` dates compare correctly as
   strings.
6. **Lineitem scan (parallel)**: split the ~12M-row lineitem vector into
   `num_threads` equal chunks and scan in parallel. For each row, check the
   four hash maps and the `c_nationkey = s_nationkey` constraint, then
   accumulate `extendedprice × (1 - discount)` into a thread-local map keyed
   by nation. No shared writes during the hot loop.
7. **Reduce**: merge the `num_threads` thread-local maps into a single
   `nation_key → revenue` map, translate keys to nation names, sort by
   revenue descending, and write the result.

The dominant memory choice is to load only the columns Query 5 needs
(`Region.{key,name}`, `Nation.{key,name,region_key}`, `Supplier.{key,nation_key}`,
`Customer.{key,nation_key}`, `Order.{key,cust_key,orderdate}`,
`LineItem.{order_key,supp_key,extendedprice,discount}`). That keeps total
RAM under ~600 MB at SF2 instead of the >18 GB a `vector<map<string,string>>`
representation would need.

---

## 8. Submission checklist

| Item                    | Where to find it                                    |
|-------------------------|-----------------------------------------------------|
| GitHub repo link        | your fork                                           |
| Final result (SF2)      | `results/result_t1.txt` (5 ASIA nations)            |
| Single-thread runtime   | `./tpch_query5 ... --threads 1` — "Query execution" |
| 4-thread runtime        | `./tpch_query5 ... --threads 4` — "Query execution" |
| Screenshot              | terminal screenshot showing both runs above         |

---

## 9. Troubleshooting

| Symptom                                   | Likely cause / fix                                                                       |
|-------------------------------------------|------------------------------------------------------------------------------------------|
| `Cannot open .../region.tbl`              | `--table_path` is wrong; must point at the directory containing the `.tbl` files.        |
| `Region not found: ASIA`                  | `region.tbl` is empty or corrupt; regenerate with `dbgen -s 2`.                          |
| `dbgen: error while loading dists.dss`    | `dbgen` needs `dists.dss` in its CWD; either `cd` into the dbgen dir or pass `-b path`.  |
| 4-thread run is slower than 1-thread      | You built in Debug mode. Re-run `cmake -DCMAKE_BUILD_TYPE=Release ..` then `make`.       |
| Different revenues for `--threads 1` vs `4` | Should not happen. If it does, please open an issue with both result files attached.    |
| Compiler errors on macOS                  | Use `clang++` ≥ 10 or `brew install gcc`; the code is portable C++11.                    |
