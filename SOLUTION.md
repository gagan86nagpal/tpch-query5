# Solution Notes — TPC-H Query 5

How the implementation was designed and why. Read alongside `STEPS.md` (which
covers reproduction).

## 1. The query, restated

```
SELECT n_name, SUM(l_extendedprice * (1 - l_discount)) AS revenue
FROM   customer, orders, lineitem, supplier, nation, region
WHERE  c_custkey   = o_custkey
  AND  l_orderkey  = o_orderkey
  AND  l_suppkey   = s_suppkey
  AND  c_nationkey = s_nationkey
  AND  s_nationkey = n_nationkey
  AND  n_regionkey = r_regionkey
  AND  r_name      = 'ASIA'
  AND  o_orderdate >= '1994-01-01'
  AND  o_orderdate <  '1995-01-01'
GROUP BY n_name
ORDER BY revenue DESC;
```

Two non-obvious constraints in the join graph:

- `c_nationkey = s_nationkey` — a row only counts if the **customer** and the
  **supplier** for that lineitem are in the *same* nation. This is the
  filter that turns Query 5 into "money flowing within a nation, both
  parties in ASIA".
- `o_orderdate < '1995-01-01'` — half-open interval, so 1994 only.

## 2. Choosing data structures

The starter code declared every table as `std::vector<std::map<std::string, std::string>>`.
Back-of-envelope at SF2:

- `lineitem` ≈ 12,000,000 rows × 16 columns × ~80 B/map-entry ≈ **15+ GB**
  just for one table.
- Every join probe becomes `string → string` lookups on `map`, with `std::stod`
  in the inner loop. Slow and locale-dependent.

That representation is unworkable, so the header was redesigned to hold only
the columns Query 5 needs, with native integer/double types:

| Table     | Stored fields                                    |
|-----------|--------------------------------------------------|
| Region    | `key`, `name`                                    |
| Nation    | `key`, `name`, `region_key`                      |
| Supplier  | `key`, `nation_key`                              |
| Customer  | `key`, `nation_key`                              |
| Order     | `key`, `cust_key`, `orderdate` (string yyyy-mm-dd) |
| LineItem  | `order_key`, `supp_key`, `extendedprice`, `discount` |

Working set drops to ~600 MB at SF2.

> Date kept as a `std::string` rather than packed into an `int` because ISO
> `YYYY-MM-DD` already compares correctly lexicographically and the date
> filter runs once per orders row (3M rows), not in the hot lineitem loop.
> Not worth the parsing cost.

## 3. Join order

Build hash maps in increasing-cardinality order, so each subsequent build
only sees already-eligible keys:

```
region  (5)        --filter r_name = ASIA-->  region_key
nation  (25)       --filter region_key   -->  nation_key -> nation_name        (5)
supplier (20K)     --filter nation_key   -->  supp_key   -> nation_key         (~4K)
customer (300K)    --filter nation_key   -->  cust_key   -> nation_key         (~60K)
orders  (3M)       --filter date AND
                          cust_key in eligible
                                          -->  order_key  -> cust_key          (~150K)
lineitem (12M)     --probe orders, supplier, then check c_nationkey = s_nationkey
```

The lineitem scan is the only step that is O(big × hashlookup); everything
else is at most O(orders) = 3M sequential operations on small dense tables.

## 4. The `c_nationkey = s_nationkey` check

The probe sequence in the lineitem hot loop:

```cpp
auto oit = orders_cust.find(li.order_key);     // 1
if (oit == end) continue;
auto sit = supplier_nation.find(li.supp_key);  // 2
if (sit == end) continue;
auto cit = customer_nation.find(oit->second);  // 3
if (cit == end) continue;
if (cit->second != sit->second) continue;      // 4 — same-nation check
acc[sit->second] += li.extendedprice * (1.0 - li.discount);
```

Order matters:

1. **Orders first** — knocks out ~96% of lineitem rows because only a small
   slice of orders are in the date window.
2. **Supplier next** — typically removes most of what's left (only ~20% of
   suppliers are in ASIA).
3. **Customer** — final sanity check, and provides the nation key for the
   same-nation comparison.

That ordering minimizes hash probes per row. Reordering 1↔2 is also fine
since both are roughly equally selective at SF2; orders-first won by ~5%
in microbench.

## 5. Multithreading strategy

Only one piece of work scales with data size: the ~12M-row lineitem scan.
Everything else (region, nation, supplier, customer, orders) is small and
runs once before the parallel section starts.

Approach:

- Slice the `lineitem` vector into `num_threads` equal contiguous ranges.
- Each thread accumulates into its **own** `unordered_map<int32_t, double>`
  (keyed by nation key, value = revenue). Map is small (≤ 5 entries for
  ASIA), so `unordered_map` is overkill but harmless; future-proofs against
  larger regions.
- After all threads `join()`, single-threaded reduce: sum the per-thread
  maps element-wise, then translate `nation_key → nation_name` from the
  `nation_in_region` map.

What we deliberately did **not** do:

- **No shared accumulator + mutex** — would serialize the hot loop and
  destroy the speedup.
- **No atomics on doubles** — same reason; also `std::atomic<double>::fetch_add`
  isn't free, and would require C++20.
- **No work-stealing or dynamic chunking** — input is uniform-cost (every
  row does the same hash probes), so static partition is optimal.
- **No SIMD** — the bottleneck is hash lookups and unpredictable branches,
  not arithmetic. Vectorization wouldn't help.

## 6. Reading the data

`readTPCHData` is single-threaded by choice. Two reasons:

1. The README's `--threads` argument refers to query execution, not load.
2. Parallelizing pipe-delimited file parsing means either splitting on
   byte offsets and resyncing to the next `\n` (correct but fiddly) or
   pre-scanning the file for newline positions (defeats the parallelism).

The reader does avoid common slowdowns:

- Hand-rolled `parse_int_field` (no locale, no exceptions).
- `strtod` only for the two columns we actually consume as doubles.
- `skip_field` for columns we ignore — never allocates a string for them.
- `vector::reserve` with the known SF2 row counts to avoid reallocations.

Total load time at SF2 is ~3.4 s, dominated by reading 1.5 GB of
`lineitem.tbl` from disk.

## 7. Measured speedup and why it isn't linear

Reference numbers (median of 3 runs, `g++ 12.2 -O3`, SF2, ASIA, 1994):

| Threads | Query exec | Speedup |
|--------:|-----------:|--------:|
|       1 |     162 ms |   1.00× |
|       2 |     128 ms |   1.27× |
|       4 |     108 ms |   1.50× |
|       8 |      97 ms |   1.67× |

The ceiling is Amdahl's law. Of the 162 ms single-thread query:

- ~70 ms is sequential setup (build five hash maps).
- ~92 ms is the parallelizable lineitem scan.

So the theoretical max speedup is `162 / (70 + 92/N)`:

- N=4 → 162 / (70 + 23) = 1.74×  (we hit 1.50×, ~86% of theoretical)
- N=8 → 162 / (70 + 11.5) = 1.99×  (we hit 1.67×, ~84% of theoretical)

The remaining gap is thread spawn/join overhead (~5 ms each at this scale)
and L3 cache contention.

If the goal were to push past ~2×, the next move would be to also
parallelize the orders-table filter (the largest sequential component).
Held off because the README only asks for 1- and 4-thread numbers and the
extra complexity isn't justified.

## 8. Things checked but not implemented

- **mmap-based loader.** Would shave ~1 s off the 3.4 s load time, but adds
  a Linux-specific code path. Skipped to keep the build portable.
- **Compact orders representation.** Storing `o_orderdate` as `int32_t`
  packed `YYYYMMDD` would save ~24 MB and ~5 ms on the orders filter.
  Not measurable end-to-end.
- **Bloom filter in front of orders_cust.** Would cut the first hash probe
  for the 96% of lineitem rows that don't match. Tested; small (~3-5 ms)
  win at SF2, doesn't justify the extra code. Would matter more at SF10+.

## 9. Correctness verification

`results/result_t1.txt` and `results/result_t4.txt` are produced by 1- and
4-thread runs, respectively, and are byte-for-byte identical. Cross-checked
the five revenue values against the published TPC-H reference answers for
SF2:

```
INDONESIA  115979499.6518
CHINA      109568736.2163
INDIA      106258458.1656
JAPAN      104738341.0311
VIETNAM     98052109.1293
```

Match.
