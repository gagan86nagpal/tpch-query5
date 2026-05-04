#include "query5.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <utility>

namespace {

// ---------- pipe-delimited field parsing helpers ----------
// All TPC-H .tbl lines look like: f1|f2|f3|...|fN|
// (note the trailing pipe). We never need a heap allocation while
// scanning past columns we don't care about.

inline void skip_field(const char* s, size_t& pos) {
    while (s[pos] != '|' && s[pos] != '\0') ++pos;
    if (s[pos] == '|') ++pos;
}

inline int32_t parse_int_field(const char* s, size_t& pos) {
    int32_t v = 0;
    bool neg = false;
    if (s[pos] == '-') { neg = true; ++pos; }
    while (s[pos] >= '0' && s[pos] <= '9') {
        v = v * 10 + (s[pos] - '0');
        ++pos;
    }
    if (s[pos] == '|') ++pos;
    return neg ? -v : v;
}

inline double parse_double_field(const char* s, size_t& pos) {
    char* endptr = nullptr;
    double v = std::strtod(s + pos, &endptr);
    pos = static_cast<size_t>(endptr - s);
    if (s[pos] == '|') ++pos;
    return v;
}

inline std::string parse_str_field(const char* s, size_t& pos) {
    size_t start = pos;
    while (s[pos] != '|' && s[pos] != '\0') ++pos;
    std::string out(s + start, pos - start);
    if (s[pos] == '|') ++pos;
    return out;
}

// ---------- per-table loaders (only needed columns) ----------

bool read_region(const std::string& path, std::vector<Region>& out) {
    std::ifstream in(path);
    if (!in) { std::cerr << "Cannot open " << path << std::endl; return false; }
    std::string line;
    while (std::getline(in, line)) {
        size_t pos = 0;
        Region r;
        r.key  = parse_int_field(line.c_str(), pos);
        r.name = parse_str_field(line.c_str(), pos);
        out.push_back(std::move(r));
    }
    return true;
}

bool read_nation(const std::string& path, std::vector<Nation>& out) {
    std::ifstream in(path);
    if (!in) { std::cerr << "Cannot open " << path << std::endl; return false; }
    std::string line;
    while (std::getline(in, line)) {
        size_t pos = 0;
        Nation n;
        n.key        = parse_int_field(line.c_str(), pos);
        n.name       = parse_str_field(line.c_str(), pos);
        n.region_key = parse_int_field(line.c_str(), pos);
        out.push_back(std::move(n));
    }
    return true;
}

bool read_supplier(const std::string& path, std::vector<Supplier>& out) {
    std::ifstream in(path);
    if (!in) { std::cerr << "Cannot open " << path << std::endl; return false; }
    out.reserve(20000);
    std::string line;
    while (std::getline(in, line)) {
        size_t pos = 0;
        Supplier s;
        s.key = parse_int_field(line.c_str(), pos);
        skip_field(line.c_str(), pos); // s_name
        skip_field(line.c_str(), pos); // s_address
        s.nation_key = parse_int_field(line.c_str(), pos);
        out.push_back(s);
    }
    return true;
}

bool read_customer(const std::string& path, std::vector<Customer>& out) {
    std::ifstream in(path);
    if (!in) { std::cerr << "Cannot open " << path << std::endl; return false; }
    out.reserve(300000);
    std::string line;
    while (std::getline(in, line)) {
        size_t pos = 0;
        Customer c;
        c.key = parse_int_field(line.c_str(), pos);
        skip_field(line.c_str(), pos); // c_name
        skip_field(line.c_str(), pos); // c_address
        c.nation_key = parse_int_field(line.c_str(), pos);
        out.push_back(c);
    }
    return true;
}

bool read_orders(const std::string& path, std::vector<Order>& out) {
    std::ifstream in(path);
    if (!in) { std::cerr << "Cannot open " << path << std::endl; return false; }
    out.reserve(3000000);
    std::string line;
    while (std::getline(in, line)) {
        size_t pos = 0;
        Order o;
        o.key      = parse_int_field(line.c_str(), pos);
        o.cust_key = parse_int_field(line.c_str(), pos);
        skip_field(line.c_str(), pos); // o_orderstatus
        skip_field(line.c_str(), pos); // o_totalprice
        o.orderdate = parse_str_field(line.c_str(), pos);
        out.push_back(std::move(o));
    }
    return true;
}

bool read_lineitem(const std::string& path, std::vector<LineItem>& out) {
    std::ifstream in(path);
    if (!in) { std::cerr << "Cannot open " << path << std::endl; return false; }
    out.reserve(12000000);
    std::string line;
    line.reserve(256);
    while (std::getline(in, line)) {
        size_t pos = 0;
        LineItem li;
        li.order_key = parse_int_field(line.c_str(), pos);
        skip_field(line.c_str(), pos); // l_partkey
        li.supp_key  = parse_int_field(line.c_str(), pos);
        skip_field(line.c_str(), pos); // l_linenumber
        skip_field(line.c_str(), pos); // l_quantity
        li.extendedprice = parse_double_field(line.c_str(), pos);
        li.discount      = parse_double_field(line.c_str(), pos);
        out.push_back(li);
    }
    return true;
}

} // namespace

// ---------- public API ----------

bool parseArgs(int argc, char* argv[],
               std::string& r_name,
               std::string& start_date,
               std::string& end_date,
               int& num_threads,
               std::string& table_path,
               std::string& result_path) {
    bool got_threads = false;
    num_threads = 0;
    for (int i = 1; i + 1 < argc; ) {
        std::string flag = argv[i];
        std::string val  = argv[i + 1];
        if      (flag == "--r_name")      r_name      = val;
        else if (flag == "--start_date")  start_date  = val;
        else if (flag == "--end_date")    end_date    = val;
        else if (flag == "--threads")   { num_threads = std::atoi(val.c_str()); got_threads = true; }
        else if (flag == "--table_path")  table_path  = val;
        else if (flag == "--result_path") result_path = val;
        else { ++i; continue; }
        i += 2;
    }
    if (r_name.empty() || start_date.empty() || end_date.empty()
        || table_path.empty() || result_path.empty()
        || !got_threads || num_threads < 1) {
        return false;
    }
    return true;
}

bool readTPCHData(const std::string& table_path, TPCHData& data) {
    auto join_path = [&](const char* fname) {
        std::string p = table_path;
        if (!p.empty() && p.back() != '/') p.push_back('/');
        p += fname;
        return p;
    };
    if (!read_region  (join_path("region.tbl"),   data.region))   return false;
    if (!read_nation  (join_path("nation.tbl"),   data.nation))   return false;
    if (!read_supplier(join_path("supplier.tbl"), data.supplier)) return false;
    if (!read_customer(join_path("customer.tbl"), data.customer)) return false;
    if (!read_orders  (join_path("orders.tbl"),   data.orders))   return false;
    if (!read_lineitem(join_path("lineitem.tbl"), data.lineitem)) return false;
    return true;
}

bool executeQuery5(const std::string& r_name,
                   const std::string& start_date,
                   const std::string& end_date,
                   int num_threads,
                   const TPCHData& data,
                   std::map<std::string, double>& results) {
    if (num_threads < 1) num_threads = 1;

    // 1. Resolve target region key.
    int32_t region_key = -1;
    for (const auto& r : data.region) {
        if (r.name == r_name) { region_key = r.key; break; }
    }
    if (region_key < 0) {
        std::cerr << "Region not found: " << r_name << std::endl;
        return false;
    }

    // 2. Nations belonging to the target region: nation_key -> nation_name.
    std::unordered_map<int32_t, std::string> nation_in_region;
    nation_in_region.reserve(8);
    for (const auto& n : data.nation) {
        if (n.region_key == region_key) {
            nation_in_region.emplace(n.key, n.name);
        }
    }

    // 3. Suppliers in those nations: supp_key -> nation_key.
    std::unordered_map<int32_t, int32_t> supplier_nation;
    supplier_nation.reserve(data.supplier.size() / 2 + 1);
    for (const auto& s : data.supplier) {
        if (nation_in_region.count(s.nation_key)) {
            supplier_nation.emplace(s.key, s.nation_key);
        }
    }

    // 4. Customers in those nations: cust_key -> nation_key.
    std::unordered_map<int32_t, int32_t> customer_nation;
    customer_nation.reserve(data.customer.size() / 2 + 1);
    for (const auto& c : data.customer) {
        if (nation_in_region.count(c.nation_key)) {
            customer_nation.emplace(c.key, c.nation_key);
        }
    }

    // 5. Orders in date range whose customer is eligible: order_key -> cust_key.
    //    Parallelized: each worker emits a local vector of (key, cust_key)
    //    pairs, then we merge them into a single hash map. ISO yyyy-mm-dd
    //    compares correctly as a string lexicographically.
    const size_t no = data.orders.size();
    std::vector<std::vector<std::pair<int32_t, int32_t>>> orders_partial(num_threads);

    auto orders_worker = [&](int tid, size_t begin, size_t end) {
        auto& local = orders_partial[tid];
        local.reserve((end - begin) / 8 + 1);
        for (size_t i = begin; i < end; ++i) {
            const auto& o = data.orders[i];
            if (o.orderdate >= start_date && o.orderdate < end_date) {
                auto it = customer_nation.find(o.cust_key);
                if (it != customer_nation.end()) {
                    local.emplace_back(o.key, o.cust_key);
                }
            }
        }
    };

    {
        size_t ochunk = (no + static_cast<size_t>(num_threads) - 1) / static_cast<size_t>(num_threads);
        std::vector<std::thread> oworkers;
        oworkers.reserve(num_threads);
        for (int t = 0; t < num_threads; ++t) {
            size_t b = std::min(static_cast<size_t>(t) * ochunk, no);
            size_t e = std::min(b + ochunk, no);
            if (b == e && t != 0) break;
            oworkers.emplace_back(orders_worker, t, b, e);
        }
        for (auto& th : oworkers) th.join();
    }

    size_t total_orders = 0;
    for (const auto& v : orders_partial) total_orders += v.size();

    std::unordered_map<int32_t, int32_t> orders_cust;
    orders_cust.reserve(total_orders + 1);
    for (auto& v : orders_partial) {
        for (const auto& p : v) orders_cust.emplace(p.first, p.second);
        v.clear();
        v.shrink_to_fit();
    }

    // 6. Parallel scan over lineitem. Each worker accumulates locally to
    //    avoid contention; results are merged afterwards.
    const size_t n = data.lineitem.size();
    std::vector<std::unordered_map<int32_t, double>> partial(num_threads);

    auto worker = [&](int tid, size_t begin, size_t end) {
        auto& acc = partial[tid];
        acc.reserve(nation_in_region.size() * 2);
        for (size_t i = begin; i < end; ++i) {
            const auto& li = data.lineitem[i];
            auto oit = orders_cust.find(li.order_key);
            if (oit == orders_cust.end()) continue;
            auto sit = supplier_nation.find(li.supp_key);
            if (sit == supplier_nation.end()) continue;
            auto cit = customer_nation.find(oit->second);
            if (cit == customer_nation.end()) continue;
            // c_nationkey = s_nationkey constraint (same nation).
            if (cit->second != sit->second) continue;
            acc[sit->second] += li.extendedprice * (1.0 - li.discount);
        }
    };

    size_t chunk = (n + static_cast<size_t>(num_threads) - 1) / static_cast<size_t>(num_threads);
    std::vector<std::thread> workers;
    workers.reserve(num_threads);
    for (int t = 0; t < num_threads; ++t) {
        size_t b = std::min(static_cast<size_t>(t) * chunk, n);
        size_t e = std::min(b + chunk, n);
        if (b == e && t != 0) break; // nothing left for this worker
        workers.emplace_back(worker, t, b, e);
    }
    for (auto& th : workers) th.join();

    // 7. Merge thread-local accumulators and translate keys to nation names.
    std::unordered_map<int32_t, double> totals;
    totals.reserve(nation_in_region.size());
    for (const auto& m : partial) {
        for (const auto& kv : m) totals[kv.first] += kv.second;
    }

    results.clear();
    for (const auto& kv : totals) {
        auto it = nation_in_region.find(kv.first);
        if (it != nation_in_region.end()) {
            results[it->second] = kv.second;
        }
    }
    return true;
}

bool outputResults(const std::string& result_path,
                   const std::map<std::string, double>& results) {
    std::ofstream out(result_path);
    if (!out) {
        std::cerr << "Cannot open output file: " << result_path << std::endl;
        return false;
    }

    std::vector<std::pair<std::string, double>> sorted(results.begin(), results.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const std::pair<std::string, double>& a,
                 const std::pair<std::string, double>& b) {
                  return a.second > b.second;
              });

    out << std::fixed << std::setprecision(4);
    out << "n_name|revenue\n";
    for (const auto& kv : sorted) {
        out << kv.first << "|" << kv.second << "\n";
    }
    return true;
}
