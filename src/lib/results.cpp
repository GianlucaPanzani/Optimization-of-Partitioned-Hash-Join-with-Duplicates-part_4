#include "results.hpp"

#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace {

std::string escape_csv_field(const std::string& field) {
    if (field.find_first_of(",\"\n\r") == std::string::npos) {
        return field;
    }

    std::string escaped = "\"";
    for (const char current : field) {
        if (current == '"') {
            escaped += "\"\"";
        } else {
            escaped.push_back(current);
        }
    }
    escaped.push_back('"');
    return escaped;
}

}  // namespace

double compute_throughput(std::uint64_t total_elements, double partition_time_seconds) {
    if (partition_time_seconds <= 0.0) {
        return 0.0;
    }
    return static_cast<double>(total_elements) / partition_time_seconds;
}

void append_to_csv(const std::string& csv_path, const ResultMap& results) {
    if (results.empty()) {
        throw std::invalid_argument("results map cannot be empty");
    }

    const std::filesystem::path output_path(csv_path);
    const std::filesystem::path parent = output_path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent);
    }

    const bool needs_header =
        !std::filesystem::exists(output_path) || std::filesystem::file_size(output_path) == 0;

    std::ofstream out(output_path, std::ios::app);
    if (!out) {
        throw std::runtime_error("Cannot open CSV file for writing: " + output_path.string());
    }

    if (needs_header) {
        bool first = true;
        for (const auto& entry : results) {
            if (!first) {
                out << ',';
            }
            out << escape_csv_field(entry.first);
            first = false;
        }
        out << '\n';
    }

    bool first = true;
    for (const auto& entry : results) {
        if (!first) {
            out << ',';
        }
        out << escape_csv_field(entry.second);
        first = false;
    }
    out << '\n';

    if (!out) {
        throw std::runtime_error("Error while writing CSV file: " + output_path.string());
    }
}
