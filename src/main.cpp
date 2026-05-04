#include "query5.hpp"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

static void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog
              << " --r_name <REGION>"
                 " --start_date <YYYY-MM-DD>"
                 " --end_date <YYYY-MM-DD>"
                 " --threads <N>"
                 " --table_path <DIR>"
                 " --result_path <FILE>" << std::endl;
}

int main(int argc, char* argv[]) {
    std::string r_name, start_date, end_date, table_path, result_path;
    int num_threads = 0;

    if (!parseArgs(argc, argv, r_name, start_date, end_date,
                   num_threads, table_path, result_path)) {
        std::cerr << "Failed to parse command line arguments." << std::endl;
        print_usage(argv[0]);
        return 1;
    }

    std::cout << "TPC-H Query 5 (multithreaded)\n";
    std::cout << "  region:      " << r_name      << "\n";
    std::cout << "  date range:  [" << start_date << ", " << end_date << ")\n";
    std::cout << "  threads:     " << num_threads << "\n";
    std::cout << "  table_path:  " << table_path  << "\n";
    std::cout << "  result_path: " << result_path << "\n\n";

    TPCHData data;
    auto t_read_begin = std::chrono::steady_clock::now();
    if (!readTPCHData(table_path, data)) {
        std::cerr << "Failed to read TPCH data." << std::endl;
        return 1;
    }
    auto t_read_end = std::chrono::steady_clock::now();
    auto read_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                       t_read_end - t_read_begin).count();

    std::cout << "Data load: " << read_ms << " ms\n";
    std::cout << "  region:   " << data.region.size()   << " rows\n";
    std::cout << "  nation:   " << data.nation.size()   << " rows\n";
    std::cout << "  supplier: " << data.supplier.size() << " rows\n";
    std::cout << "  customer: " << data.customer.size() << " rows\n";
    std::cout << "  orders:   " << data.orders.size()   << " rows\n";
    std::cout << "  lineitem: " << data.lineitem.size() << " rows\n\n";

    std::map<std::string, double> results;
    auto t_query_begin = std::chrono::steady_clock::now();
    if (!executeQuery5(r_name, start_date, end_date, num_threads, data, results)) {
        std::cerr << "Failed to execute TPCH Query 5." << std::endl;
        return 1;
    }
    auto t_query_end = std::chrono::steady_clock::now();
    auto query_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        t_query_end - t_query_begin).count();

    std::cout << "Query execution: " << query_ms
              << " ms (" << num_threads << " thread(s))\n\n";

    std::vector<std::pair<std::string, double>> sorted(results.begin(), results.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const std::pair<std::string, double>& a,
                 const std::pair<std::string, double>& b) {
                  return a.second > b.second;
              });

    std::cout << "Results (n_name, revenue) sorted desc:\n";
    std::cout << std::fixed << std::setprecision(4);
    std::cout << "+-------------+-------------------+\n";
    std::cout << "| n_name      |           revenue |\n";
    std::cout << "+-------------+-------------------+\n";
    for (const auto& kv : sorted) {
        std::cout << "| " << std::left  << std::setw(12) << kv.first
                  << "| " << std::right << std::setw(17) << kv.second
                  << " |\n";
    }
    std::cout << "+-------------+-------------------+\n\n";

    if (!outputResults(result_path, results)) {
        std::cerr << "Failed to output results." << std::endl;
        return 1;
    }

    std::cout << "TPCH Query 5 implementation completed." << std::endl;
    return 0;
}
