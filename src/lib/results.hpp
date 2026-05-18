#ifndef RESULTS_HPP
#define RESULTS_HPP

#include <cstdint>
#include <map>
#include <string>

using ResultMap = std::map<std::string, std::string>;

double compute_throughput(std::uint64_t total_elements, double partition_time_seconds);
void append_to_csv(const std::string& csv_path, const ResultMap& results);

#endif
