#ifndef QUERY5_HPP
#define QUERY5_HPP

#include <cstdint>
#include <map>
#include <string>
#include <vector>

// Per-table row structs containing only the columns Query 5 needs.
// Reading just the needed fields keeps memory usage feasible at SF2
// (lineitem has ~12M rows; storing every column as a map<string,string>
// would consume tens of GB and dominate runtime).

struct Region {
    int32_t key;
    std::string name;
};

struct Nation {
    int32_t key;
    std::string name;
    int32_t region_key;
};

struct Supplier {
    int32_t key;
    int32_t nation_key;
};

struct Customer {
    int32_t key;
    int32_t nation_key;
};

struct Order {
    int32_t key;
    int32_t cust_key;
    std::string orderdate;
};

struct LineItem {
    int32_t order_key;
    int32_t supp_key;
    double extendedprice;
    double discount;
};

struct TPCHData {
    std::vector<Region> region;
    std::vector<Nation> nation;
    std::vector<Supplier> supplier;
    std::vector<Customer> customer;
    std::vector<Order> orders;
    std::vector<LineItem> lineitem;
};

bool parseArgs(int argc, char* argv[],
               std::string& r_name,
               std::string& start_date,
               std::string& end_date,
               int& num_threads,
               std::string& table_path,
               std::string& result_path);

bool readTPCHData(const std::string& table_path, TPCHData& data);

bool executeQuery5(const std::string& r_name,
                   const std::string& start_date,
                   const std::string& end_date,
                   int num_threads,
                   const TPCHData& data,
                   std::map<std::string, double>& results);

bool outputResults(const std::string& result_path,
                   const std::map<std::string, double>& results);

#endif // QUERY5_HPP
