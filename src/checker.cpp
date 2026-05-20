#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;



static bool read_file_binary(const fs::path& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

struct ResultOutput {
    std::string executable;
    std::string dataset_type;
    std::string join_count;
    std::string checksum1;
    std::string checksum2;
    bool has_verified_field = false;
    bool verified = false;
};

static std::map<std::string, std::string> parse_key_values(const std::string& content) {
    std::map<std::string, std::string> values;
    std::istringstream in(content);
    std::string line;

    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        const auto separator = line.find('=');
        if (separator == std::string::npos) {
            continue;
        }

        values[line.substr(0, separator)] = line.substr(separator + 1);
    }

    return values;
}

static std::optional<ResultOutput> result_from_content(const std::string& content) {
    const auto values = parse_key_values(content);

    const auto executable = values.find("executable");
    const auto dataset_type = values.find("dataset_type");
    const auto join_count = values.find("join_count");
    const auto checksum1 = values.find("checksum1");
    const auto checksum2 = values.find("checksum2");
    const auto verified = values.find("verified");

    if (executable == values.end() || !(dataset_type != values.end()) ||
        join_count == values.end() || checksum1 == values.end() || checksum2 == values.end()) {
        return std::nullopt;
    }

    return ResultOutput{
        executable->second,
        (dataset_type != values.end()) ? dataset_type->second : "",
        join_count->second,
        checksum1->second,
        checksum2->second,
        verified != values.end(),
        verified != values.end() && (verified->second == "true" || verified->second == "1"),
    };
}

struct OutputFile {
    fs::path path;
    ResultOutput result;
};

static bool is_slurm_output_file(const fs::path& path) {
    const std::string filename = path.filename().string();
    return filename.rfind("slurm", 0) == 0;
}



int main(int argc, char** argv) {
    const fs::path out_dir = (argc > 1) ? fs::path(argv[1]) : fs::path("out");

    // Cases of path errors
    if (!fs::exists(out_dir)) {
        std::cerr << "[checker] directory does not exist: " << out_dir << '\n';
        return 2;
    }
    if (!fs::is_directory(out_dir)) {
        std::cerr << "[checker] path is not a directory: " << out_dir << '\n';
        return 2;
    }

    // Get the files inside the directory
    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(out_dir)) {
        if (entry.is_regular_file() && is_slurm_output_file(entry.path())) {
            files.push_back(entry.path());
        }
    }
    if (files.empty()) {
        std::cerr << "[checker] no slurm output files found in: " << out_dir << '\n';
        return 2;
    }

    std::sort(files.begin(), files.end());

    std::map<std::string, std::vector<OutputFile>> files_by_dataset_type;
    for (const auto& file : files) {
        std::string content;
        if (!read_file_binary(file, content)) {
            std::cerr << "[checker] failed to read file: " << file << '\n';
            return 2;
        }

        const auto result = result_from_content(content);
        if (!result.has_value() || result->executable.empty() || result->dataset_type.empty()) {
            std::cerr << "[checker] missing executable/dataset_type/checksum output in: " << file.filename().string() << '\n';
            return 2;
        }
        if (result->has_verified_field && !result->verified) {
            std::cerr << "[checker] naive verification failed in: " << file.filename().string() << '\n';
            return 1;
        }

        const std::string group_key = result->executable + " | " + result->dataset_type;
        files_by_dataset_type[group_key].push_back(OutputFile{file, *result});
    }

    bool all_equal = true;

    for (const auto& [group_key, dataset_files] : files_by_dataset_type) {
        std::vector<OutputFile> unchecked_files;
        std::size_t verified_count = 0;
        for (const auto& file : dataset_files) {
            if (file.result.verified) {
                ++verified_count;
            } else {
                unchecked_files.push_back(file);
            }
        }

        if (unchecked_files.empty()) {
            const auto& reference = dataset_files.front();
            std::cout << "[checker] OK --> executable=" << reference.result.executable
                      << " dataset_type=" << reference.result.dataset_type
                      << " all " << dataset_files.size()
                      << " output files were already naive-verified\n";
            continue;
        }

        const auto& reference = unchecked_files.front();
        bool dataset_equal = true;

        for (std::size_t i = 1; i < unchecked_files.size(); ++i) {
            const auto& current = unchecked_files[i];
            if (current.result.join_count != reference.result.join_count ||
                current.result.checksum1 != reference.result.checksum1 ||
                current.result.checksum2 != reference.result.checksum2) {
                dataset_equal = false;
                all_equal = false;
                std::cout << "[checker] mismatch: executable=" << current.result.executable
                          << " dataset_type=" << current.result.dataset_type << ' '
                          << current.path.filename().string() << " differs from "
                          << reference.path.filename().string()
                          << " reference=(" << reference.result.join_count << ", "
                          << reference.result.checksum1 << ", " << reference.result.checksum2 << ")"
                          << " current=(" << current.result.join_count << ", "
                          << current.result.checksum1 << ", " << current.result.checksum2 << ")\n";
            }
        }

        if (!dataset_equal) {
            std::cout << "[checker] NO --> executable=" << reference.result.executable
                      << " dataset_type=" << reference.result.dataset_type
                      << " output files do NOT have identical checksums\n";
        } else {
            std::cout << "[checker] OK --> executable=" << reference.result.executable
                      << " dataset_type=" << reference.result.dataset_type
                      << " all " << unchecked_files.size()
                      << " unchecked output files have identical checksums";
            if (verified_count > 0) {
                std::cout << " and " << verified_count << " output files were already naive-verified";
            }
            std::cout << "\n";
        }
    }

    if (!all_equal) {
        std::cout << "[checker] NO --> at least one executable/dataset_type group has non-identical checksums\n";
    }
    return 0;
}
