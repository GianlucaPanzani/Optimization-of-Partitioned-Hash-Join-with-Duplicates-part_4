// hashjoin_omp_task_opt_wb.cpp
//
// Task-based OpenMP implementation for Module 3
// Partitioned Hash Join with Duplicates
//
// This file is intentionally kept close to hashjoin_seq.cpp, with only the
// modifications needed to express the partitioning and local-join phases with
// explicit OpenMP tasks.
//
// Run example:
//   ./hashjoin_omp_task -nr 5 -ns 8 -seed 13 -max-key 8 -p 4
//
// Output:
//   join_count
//   checksum1
//   checksum2

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <omp.h>

#include "lib/timing.hpp"
#include "lib/results.hpp"

// ------------------------------------------------------------
// Record definition
// ------------------------------------------------------------
struct Record {
    std::uint64_t key{};
};

// ------------------------------------------------------------
// Utility: command-line parsing
// ------------------------------------------------------------
static bool read_arg_u64(int argc, char** argv,
                         std::initializer_list<std::string_view> names,
                         std::uint64_t& out) {
    for (int i = 1; i + 1 < argc; ++i) {
        const std::string_view arg(argv[i]);
        for (const auto name : names) {
            if (arg == name) {
                out = std::strtoull(argv[i + 1], nullptr, 10);
                return true;
            }
        }
    }
    return false;
}

static bool read_arg_string(int argc, char** argv,
                            std::initializer_list<std::string_view> names,
                            std::string& out) {
    for (int i = 1; i + 1 < argc; ++i) {
        const std::string_view arg(argv[i]);
        for (const auto name : names) {
            if (arg == name) {
                out = argv[i + 1];
                return true;
            }
        }
    }
    return false;
}

static void usage(const char* prog) {
    std::cerr
        << "Usage:\n"
        << "  " << prog
        << " -nr NR -ns NS -seed SEED -max-key K -p P\n"
        << "       [--partition-threads T] [--join-threads T]\n"
        << "       [--partition-block-size B]\n"
        << "       [--partition-task-blocks B] [--join-task-partitions P] [--offset-task-partitions P]\n"
        << "       [--partition-chunk C] [--join-chunk C]\n\n"
        << "Parameters:\n"
        << "  -nr         Number of records in relation R\n"
        << "  -ns         Number of records in relation S\n"
        << "  -seed       Deterministic seed\n"
        << "  -max-key    Keys are generated in [0, max-key)\n"
        << "  -p          Number of partitions (power of two required in this reference code)\n"
        << "  --partition-threads      Number of threads for task-based partition phase\n"
        << "  --join-threads           Number of threads for task-based join phase\n"
        << "  --partition-block-size   Number of records per input block during partitioning\n"
        << "  --partition-task-blocks  Number of input blocks handled by each explicit partition task\n"
        << "  --join-task-partitions   Number of partitions handled by each explicit join task\n"
        << "  --offset-task-partitions Number of partitions handled by each explicit offset task\n"
        << "  --partition-chunk / --join-chunk are accepted for compatibility with the loop benchmark scripts.\n"
        << "  --dataset-type uniform|skewed_<record_pct>_<hot_partition_pct>\n"
        << "  --partition-task-grain / --join-task-grain / --offset-task-grain are accepted as legacy aliases.\n";
}

static bool is_power_of_two(std::uint32_t x) {
    return x != 0 && (x & (x - 1U)) == 0;
}

// ------------------------------------------------------------
// OpenMP task configuration
// ------------------------------------------------------------
struct OmpTaskConfig {
    int partition_threads = 1;
    int join_threads = 1;
    std::size_t partition_block_size = 65536;

    // Explicit task batch sizes.
    // partition_task_blocks is measured in input blocks.
    // join_task_partitions and offset_task_partitions are measured in partitions.
    int partition_task_blocks = 1;
    int join_task_partitions = 1;
    int offset_task_partitions = 1;

    // Kept only for compatibility with the same runner/grid format used by
    // hashjoin_omp_loop.cpp. They do not control task scheduling.
    std::string partition_schedule_name = "explicit-task";
    std::string join_schedule_name = "explicit-task";
    std::string dataset_type_name = "uniform";
    int partition_chunk = 0;
    int join_chunk = 0;
};

static int checked_positive_int(std::uint64_t value, const char* name) {
    if (value == 0 || value > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error(std::string("Invalid ") + name + ": must be in [1, INT_MAX]");
    }
    return static_cast<int>(value);
}

static int checked_nonnegative_int(std::uint64_t value, const char* name) {
    if (value > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error(std::string("Invalid ") + name + ": too large");
    }
    return static_cast<int>(value);
}

// ------------------------------------------------------------
// Deterministic pseudo-random generation
// ------------------------------------------------------------
static inline std::uint64_t splitmix64_mix(std::uint64_t x) {
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    x = x ^ (x >> 31);
    return x;
}
static inline std::uint64_t splitmix64(std::uint64_t x) {
    return splitmix64_mix(x + 0x9e3779b97f4a7c15ULL);
}
static inline std::uint64_t splitmix64_next(std::uint64_t& state) {
    state += 0x9e3779b97f4a7c15ULL;
    return splitmix64_mix(state);
}



// ------------------------------------------------------------
// Dataset distribution configuration
// ------------------------------------------------------------
// Supported format:
//   uniform
//   skewed_<record_percentage>_<hot_partition_percentage>
// Example:
//   skewed_80_5 means: 80% of records are generated from keys mapping to
//   5% of the partitions. This creates partition-level workload imbalance.
struct DatasetConfig {
    std::string type = "uniform";
    bool skewed = false;
    std::uint32_t skew_record_percent = 0;
    std::uint32_t hot_partition_percent = 100;
};

static bool parse_dataset_type(const std::string& value, DatasetConfig& cfg) {
    cfg.type = value;
    cfg.skewed = false;
    cfg.skew_record_percent = 0;
    cfg.hot_partition_percent = 100;

    if (value == "uniform") {
        return true;
    }

    const std::string prefix = "skewed_";
    if (value.rfind(prefix, 0) != 0) {
        return false;
    }

    const std::string rest = value.substr(prefix.size());
    const std::size_t sep = rest.find('_');
    if (sep == std::string::npos) {
        return false;
    }

    try {
        const unsigned long rec = std::stoul(rest.substr(0, sep));
        const unsigned long hot = std::stoul(rest.substr(sep + 1));
        if (rec > 100 || hot == 0 || hot > 100) {
            return false;
        }
        cfg.skewed = true;
        cfg.skew_record_percent = static_cast<std::uint32_t>(rec);
        cfg.hot_partition_percent = static_cast<std::uint32_t>(hot);
        return true;
    } catch (...) {
        return false;
    }
}

static inline std::uint32_t compute_partition_id(std::uint64_t key, std::uint32_t P);
static std::vector<std::uint64_t> build_skew_key_pool(std::uint32_t P,
                                                       std::uint64_t max_key,
                                                       std::uint32_t hot_partition_percent);

static std::vector<Record> generate_relation(std::size_t n,
                                             std::uint64_t seed,
                                             std::uint64_t max_key,
                                             std::uint32_t P,
                                             const DatasetConfig& dataset_cfg) {
    std::vector<Record> out(n);
    std::uint64_t state = seed;

    std::vector<std::uint64_t> skew_key_pool;
    if (dataset_cfg.skewed && dataset_cfg.skew_record_percent > 0) {
        skew_key_pool = build_skew_key_pool(P, max_key, dataset_cfg.hot_partition_percent);
        if (skew_key_pool.empty()) {
            throw std::runtime_error("Unable to generate skewed dataset: no keys map to hot partitions. Increase max-key or hot partition percentage.");
        }
    }

    for (std::size_t i = 0; i < n; ++i) {
        const std::uint64_t r = splitmix64_next(state);
        if (!skew_key_pool.empty() && (r % 100ULL) < dataset_cfg.skew_record_percent) {
            const std::uint64_t idx = splitmix64_next(state) % static_cast<std::uint64_t>(skew_key_pool.size());
            out[i].key = skew_key_pool[static_cast<std::size_t>(idx)];
        } else {
            out[i].key = (max_key == 0) ? 0ULL : (r % max_key);
        }
    }
    return out;
}

// ------------------------------------------------------------
// Intentionally simple partition mapping
// ------------------------------------------------------------
static inline std::uint32_t compute_partition_id(std::uint64_t key, std::uint32_t P) {
    const std::uint32_t mask = P - 1U;
    key ^= key >> 33;
    key ^= key >> 17;
    key ^= key >> 9;
    return static_cast<std::uint32_t>(key) & mask;
}

static std::vector<std::uint64_t> build_skew_key_pool(std::uint32_t P,
                                                       std::uint64_t max_key,
                                                       std::uint32_t hot_partition_percent) {
    const std::uint32_t hot_partitions = std::max<std::uint32_t>(
        1U,
        static_cast<std::uint32_t>((static_cast<std::uint64_t>(P) * hot_partition_percent + 99ULL) / 100ULL)
    );

    std::vector<std::uint64_t> pool;
    const std::uint64_t key_limit = (max_key == 0) ? 1ULL : max_key;
    pool.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(key_limit, 1ULL << 20)));

    for (std::uint64_t key = 0; key < key_limit; ++key) {
        if (compute_partition_id(key, P) < hot_partitions) {
            pool.push_back(key);
        }
    }
    return pool;
}


// ------------------------------------------------------------
// Blocked histogram for task-based OpenMP partitioning
// ------------------------------------------------------------
struct HistogramResult {
    std::vector<std::size_t> hist;
    std::vector<std::size_t> block_hist; // flattened: block_hist[block * P + pid]
    std::size_t num_blocks = 0;
};

static inline std::size_t task_batch_size(int configured_value) {
    return static_cast<std::size_t>(configured_value);
}

static HistogramResult compute_histogram(const std::vector<Record>& data,
                                         std::uint32_t P,
                                         const OmpTaskConfig& cfg) {
    const std::size_t n = data.size();
    const std::size_t block_size = cfg.partition_block_size;
    const std::size_t num_blocks = (n + block_size - 1) / block_size;
    const std::size_t P_size = static_cast<std::size_t>(P);
    const std::size_t partition_task_blocks = task_batch_size(cfg.partition_task_blocks);
    const std::size_t offset_task_partitions = task_batch_size(cfg.offset_task_partitions);

    HistogramResult out;
    out.hist.assign(P_size, 0);
    out.block_hist.assign(num_blocks * P_size, 0);
    out.num_blocks = num_blocks;

    #pragma omp parallel num_threads(cfg.partition_threads) default(none) shared(data, out) firstprivate(P, P_size, block_size, num_blocks, partition_task_blocks, offset_task_partitions)
    {
        #pragma omp single
        {
            for (std::size_t task_begin = 0; task_begin < num_blocks; task_begin += partition_task_blocks) {
                const std::size_t task_end = std::min(task_begin + partition_task_blocks, num_blocks);

                #pragma omp task default(none) shared(data, out) firstprivate(P, P_size, block_size, task_begin, task_end)
                {
                    for (std::size_t b = task_begin; b < task_end; ++b) {
                        const std::size_t begin = b * block_size;
                        const std::size_t end = std::min(begin + block_size, data.size());
                        std::size_t* local = out.block_hist.data() + b * P_size;

                        for (std::size_t i = begin; i < end; ++i) {
                            const std::uint32_t pid = compute_partition_id(data[i].key, P);
                            ++local[pid];
                        }
                    }
                }
            }

            #pragma omp taskwait

            for (std::size_t task_begin = 0; task_begin < P_size; task_begin += offset_task_partitions) {
                const std::size_t task_end = std::min(task_begin + offset_task_partitions, P_size);

                #pragma omp task default(none) shared(out) firstprivate(P_size, num_blocks, task_begin, task_end)
                {
                    for (std::size_t pid = task_begin; pid < task_end; ++pid) {
                        std::size_t sum = 0;
                        for (std::size_t b = 0; b < num_blocks; ++b) {
                            sum += out.block_hist[b * P_size + pid];
                        }
                        out.hist[pid] = sum;
                    }
                }
            }

            #pragma omp taskwait
        }
    }

    return out;
}

// ------------------------------------------------------------
// Prefix sum (exclusive scan)
// ------------------------------------------------------------
static std::vector<std::size_t> exclusive_prefix_sum(const std::vector<std::size_t>& hist) {
    std::vector<std::size_t> begin(hist.size(), 0);

    std::size_t running = 0;
    for (std::size_t p = 0; p < hist.size(); ++p) {
        begin[p] = running;
        running += hist[p];
    }
    return begin;
}

// ------------------------------------------------------------
// Scatter into a partitioned array
// ------------------------------------------------------------
static std::vector<Record> scatter_partitioned(const std::vector<Record>& data,
                                               std::uint32_t P,
                                               const std::vector<std::size_t>& begin,
                                               const HistogramResult& hist_result,
                                               const OmpTaskConfig& cfg) {
    std::vector<Record> out(data.size());

    const std::size_t num_blocks = hist_result.num_blocks;
    const std::size_t block_size = cfg.partition_block_size;
    const std::size_t P_size = static_cast<std::size_t>(P);
    const std::size_t partition_task_blocks = task_batch_size(cfg.partition_task_blocks);
    const std::size_t offset_task_partitions = task_batch_size(cfg.offset_task_partitions);

    std::vector<std::size_t> block_offsets(num_blocks * P_size, 0);

    #pragma omp parallel num_threads(cfg.partition_threads) default(none) shared(data, out, block_offsets, hist_result, begin) firstprivate(P, P_size, block_size, num_blocks, partition_task_blocks, offset_task_partitions)
    {
        #pragma omp single
        {
            for (std::size_t task_begin = 0; task_begin < P_size; task_begin += offset_task_partitions) {
                const std::size_t task_end = std::min(task_begin + offset_task_partitions, P_size);

                #pragma omp task default(none) shared(block_offsets, hist_result, begin) firstprivate(P_size, num_blocks, task_begin, task_end)
                {
                    for (std::size_t pid = task_begin; pid < task_end; ++pid) {
                        std::size_t running = begin[pid];
                        for (std::size_t b = 0; b < num_blocks; ++b) {
                            block_offsets[b * P_size + pid] = running;
                            running += hist_result.block_hist[b * P_size + pid];
                        }
                    }
                }
            }

            #pragma omp taskwait

            for (std::size_t task_begin = 0; task_begin < num_blocks; task_begin += partition_task_blocks) {
                const std::size_t task_end = std::min(task_begin + partition_task_blocks, num_blocks);

                #pragma omp task default(none) shared(data, out, block_offsets) firstprivate(P, P_size, block_size, task_begin, task_end)
                {
                    for (std::size_t b = task_begin; b < task_end; ++b) {
                        const std::size_t in_begin = b * block_size;
                        const std::size_t in_end = std::min(in_begin + block_size, data.size());

                        std::vector<std::size_t> next(P_size);
                        for (std::size_t pid = 0; pid < P_size; ++pid) {
                            next[pid] = block_offsets[b * P_size + pid];
                        }

                        for (std::size_t i = in_begin; i < in_end; ++i) {
                            const std::uint32_t pid = compute_partition_id(data[i].key, P);
                            out[next[pid]++] = data[i];
                        }
                    }
                }
            }

            #pragma omp taskwait
        }
    }

    return out;
}

// ------------------------------------------------------------
// Partitioned relation metadata
// ------------------------------------------------------------
struct PartitionedRelation {
    std::vector<Record> data;
    std::vector<std::size_t> begin;
    std::vector<std::size_t> end;
};

struct JoinWorkItem {
    std::uint32_t pid = 0;
    std::size_t s_begin = 0;
    std::size_t s_end = 0;
};

static std::vector<JoinWorkItem> build_join_work_items(const PartitionedRelation& Rpart,
                                                       const PartitionedRelation& Spart,
                                                       std::uint32_t P,
                                                       int join_threads) {
    std::vector<std::uint32_t> order(P);
    std::vector<std::size_t> partition_work(P, 0);
    std::size_t total_join_work = 0;

    for (std::uint32_t pid = 0; pid < P; ++pid) {
        order[pid] = pid;
        partition_work[pid] = (Rpart.end[pid] - Rpart.begin[pid]) +
                              (Spart.end[pid] - Spart.begin[pid]);
        total_join_work += partition_work[pid];
    }

    std::sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) {
        return partition_work[a] > partition_work[b];
    });

    // Target amount of work per join thread, measured as records to scan.
    const std::size_t thread_count = std::max<std::size_t>(1, static_cast<std::size_t>(join_threads));
    const std::size_t work_per_thread = std::max<std::size_t>(
        1, (total_join_work + thread_count - 1) / thread_count
    );

    std::vector<JoinWorkItem> work_items;
    for (const std::uint32_t pid : order) {
        const std::size_t r_size = Rpart.end[pid] - Rpart.begin[pid];
        const std::size_t s_size = Spart.end[pid] - Spart.begin[pid];
        if (r_size == 0 || s_size == 0) {
            continue;
        }

        // Number of join-thread slots assigned to this partition. Large
        // partitions are split into multiple S subranges; small partitions keep
        // one slot. The clamp avoids empty subranges and over-assignment.
        std::size_t threads_for_partition =
            (partition_work[pid] + work_per_thread - 1) / work_per_thread;
        threads_for_partition = std::max<std::size_t>(1, threads_for_partition);
        threads_for_partition = std::min(threads_for_partition, thread_count);
        threads_for_partition = std::min(threads_for_partition, s_size);

        for (std::size_t part = 0; part < threads_for_partition; ++part) {
            const std::size_t sub_begin = Spart.begin[pid] + (part * s_size) / threads_for_partition;
            const std::size_t sub_end = Spart.begin[pid] + ((part + 1) * s_size) / threads_for_partition;
            work_items.push_back(JoinWorkItem{pid, sub_begin, sub_end});
        }
    }

    return work_items;
}


// ------------------------------------------------------------
// Full partitioning pipeline for one relation
// ------------------------------------------------------------
static PartitionedRelation partition_relation(const std::vector<Record>& rel,
                                              std::uint32_t P,
                                              const OmpTaskConfig& cfg) {
    const auto hist_result = compute_histogram(rel, P, cfg);
    const auto begin = exclusive_prefix_sum(hist_result.hist);
    auto data = scatter_partitioned(rel, P, begin, hist_result, cfg);

    std::vector<std::size_t> end(P, 0);
    for (std::uint32_t pid = 0; pid < P; ++pid) {
        end[pid] = begin[pid] + hist_result.hist[pid];
    }

    return PartitionedRelation{
        .data = std::move(data),
        .begin = begin,
        .end = end
    };
}

// ------------------------------------------------------------
// Join result
// ------------------------------------------------------------
struct JoinResult {
    std::uint64_t join_count = 0;
    std::uint64_t checksum1 = 0;
    std::uint64_t checksum2 = 0;
    double part_time_sec = 0.0;
    double join_time_sec = 0.0;
};


// ------------------------------------------------------------
// Count table
// ------------------------------------------------------------
class FlatCountTable {
public:
    explicit FlatCountTable(std::size_t expected_items) {
        const std::size_t min_capacity = expected_items * 2;
        std::size_t x = std::max<std::size_t>(8, min_capacity);
        std::size_t v = 1;
        while (v < x) {
            v <<= 1;
        }
        capacity_ = v;
        mask_ = capacity_ - 1;
        keys_.assign(capacity_, 0);
        counts_.assign(capacity_, 0);
        occupied_.assign(capacity_, 0);
    }

    void increment(std::uint64_t key) {
        std::size_t idx = static_cast<std::size_t>(splitmix64_mix(key)) & mask_;
        while (occupied_[idx] != 0) {
            if (keys_[idx] == key) {
                ++counts_[idx];
                return;
            }
            idx = (idx + 1) & mask_;
        }

        occupied_[idx] = 1;
        keys_[idx] = key;
        counts_[idx] = 1;
    }

    std::uint32_t find_count(std::uint64_t key) const {
        std::size_t idx = static_cast<std::size_t>(splitmix64_mix(key)) & mask_;
        while (occupied_[idx] != 0) {
            if (keys_[idx] == key) {
                return counts_[idx];
            }
            idx = (idx + 1) & mask_;
        }
        return 0;
    }

private:
    std::size_t capacity_ = 0;
    std::size_t mask_ = 0;
    std::vector<std::uint64_t> keys_;
    std::vector<std::uint32_t> counts_;
    std::vector<std::uint8_t> occupied_;
};

// ------------------------------------------------------------
// Local join on one partition
// ------------------------------------------------------------
static JoinResult join_one_partition_subrange(const PartitionedRelation& Rpart,
                                              const PartitionedRelation& Spart,
                                              std::uint32_t pid,
                                              std::size_t s_begin,
                                              std::size_t s_end) {
    JoinResult result{};

    const std::size_t r_begin = Rpart.begin[pid];
    const std::size_t r_end = Rpart.end[pid];

    // Nothing to do if the build side or this assigned probe subrange is empty.
    if (r_begin == r_end || s_begin == s_end) {
        return result;
    }

    // Each work item builds a private table and writes a private result. The
    // S subranges are therefore independent and need no mutex in the hot path.
    FlatCountTable countR(r_end - r_begin);

    for (std::size_t i = r_begin; i < r_end; ++i) {
        countR.increment(Rpart.data[i].key);
    }

    // Probe only the subrange assigned to this logical join thread.
    for (std::size_t i = s_begin; i < s_end; ++i) {
        const std::uint64_t key = Spart.data[i].key;
        const std::uint32_t multiplicity = countR.find_count(key);
        if (multiplicity != 0U) {

            result.join_count += multiplicity;
            result.checksum1 += splitmix64(key) * multiplicity;
            result.checksum2 += splitmix64(key ^ 0x9e3779b97f4a7c15ULL) * multiplicity;
        }
    }

    return result;
}

// ------------------------------------------------------------
// Full task-based partitioned hash join
// ------------------------------------------------------------
static JoinResult partitioned_hash_join(const std::vector<Record>& R,
                                        const std::vector<Record>& S,
                                        std::uint32_t p,
                                        const OmpTaskConfig& cfg) {
    JoinResult result{};

    double t0 = get_time();
    const PartitionedRelation Rpart = partition_relation(R, p, cfg);
    const PartitionedRelation Spart = partition_relation(S, p, cfg);
    double t1 = get_time();
    result.part_time_sec = t1 - t0;

    const std::vector<JoinWorkItem> work_items = build_join_work_items(Rpart, Spart, p, cfg.join_threads);

    t0 = get_time();
    std::vector<JoinResult> partial(work_items.size());
    const std::size_t item_count = work_items.size();
    const std::size_t join_task_items = task_batch_size(cfg.join_task_partitions);

    #pragma omp parallel num_threads(cfg.join_threads) default(none) shared(partial, Rpart, Spart, work_items) firstprivate(item_count, join_task_items)
    {
        #pragma omp single
        {
            for (std::size_t task_begin = 0; task_begin < item_count; task_begin += join_task_items) {
                const std::size_t task_end = std::min(task_begin + join_task_items, item_count);

                #pragma omp task default(none) shared(partial, Rpart, Spart, work_items) firstprivate(task_begin, task_end)
                {
                    for (std::size_t idx = task_begin; idx < task_end; ++idx) {
                        const JoinWorkItem& item = work_items[idx];
                        partial[idx] = join_one_partition_subrange(Rpart, Spart, item.pid, item.s_begin, item.s_end);
                    }
                }
            }

            #pragma omp taskwait
        }
    }

    for (const JoinResult& local : partial) {
        result.join_count += local.join_count;
        result.checksum1 += local.checksum1;
        result.checksum2 += local.checksum2;
    }

    t1 = get_time();
    result.join_time_sec = t1 - t0;
    return result;
}

// ------------------------------------------------------------
// Verifier for very small inputs
// ------------------------------------------------------------
static JoinResult naive_join_verifier(const std::vector<Record>& R,
                                      const std::vector<Record>& S) {
    JoinResult result{};

    for (const auto& r : R) {
        for (const auto& s : S) {
            if (r.key == s.key) {
                result.join_count += 1;
                result.checksum1 += splitmix64(r.key);
                result.checksum2 += splitmix64(r.key ^ 0x9e3779b97f4a7c15ULL);
            }
        }
    }
    return result;
}

// ------------------------------------------------------------
// Main
// ------------------------------------------------------------
int main(int argc, char** argv) {
    std::uint64_t nr = 0, ns = 0, seed = 0, max_key = 0, p = 0;
    std::uint64_t part_threads_u64 = static_cast<std::uint64_t>(omp_get_max_threads());
    std::uint64_t join_threads_u64 = static_cast<std::uint64_t>(omp_get_max_threads());
    std::uint64_t partition_block_size_u64 = 65536;

    std::uint64_t partition_chunk_u64 = 0;
    std::uint64_t join_chunk_u64 = 0;
    std::uint64_t partition_task_blocks_u64 = 1;
    std::uint64_t join_task_partitions_u64 = 1;
    std::uint64_t offset_task_partitions_u64 = 1;
    std::uint64_t partition_task_grain_u64 = 0; // legacy alias
    std::uint64_t join_task_grain_u64 = 0;      // legacy alias
    std::uint64_t offset_task_grain_u64 = 0;    // legacy alias

    std::string partition_schedule_name = "explicit-task";
    std::string join_schedule_name = "explicit-task";
    std::string dataset_type_name = "uniform";

    if (!read_arg_u64(argc, argv, {"-nr"}, nr) ||
        !read_arg_u64(argc, argv, {"-ns"}, ns) ||
        !read_arg_u64(argc, argv, {"-seed"}, seed) ||
        !read_arg_u64(argc, argv, {"-max-key"}, max_key) ||
        !read_arg_u64(argc, argv, {"-p"}, p)) {
        usage(argv[0]);
        return 1;
    }

    read_arg_u64(argc, argv, {"--partition-threads", "-partition-threads"}, part_threads_u64);
    read_arg_u64(argc, argv, {"--join-threads", "-join-threads"}, join_threads_u64);
    read_arg_u64(argc, argv, {"--partition-block-size", "-partition-block-size"}, partition_block_size_u64);

    // Compatibility with the loop benchmark runner.
    read_arg_u64(argc, argv, {"--partition-chunk", "-partition-chunk"}, partition_chunk_u64);
    read_arg_u64(argc, argv, {"--join-chunk", "-join-chunk"}, join_chunk_u64);
    read_arg_string(argc, argv, {"--partition-schedule", "-partition-schedule"}, partition_schedule_name);
    read_arg_string(argc, argv, {"--join-schedule", "-join-schedule"}, join_schedule_name);
    read_arg_string(argc, argv, {"--dataset-type", "-dataset-type", "--dataset", "-dataset"}, dataset_type_name);

    // Task-specific parameters. Legacy task-grain arguments are accepted so the
    // existing bash grid can be reused without changing the runner.
    const bool has_partition_task_blocks = read_arg_u64(argc, argv, {"--partition-task-blocks", "-partition-task-blocks"}, partition_task_blocks_u64);
    const bool has_join_task_partitions = read_arg_u64(argc, argv, {"--join-task-partitions", "-join-task-partitions"}, join_task_partitions_u64);
    const bool has_offset_task_partitions = read_arg_u64(argc, argv, {"--offset-task-partitions", "-offset-task-partitions"}, offset_task_partitions_u64);
    const bool has_partition_task_grain = read_arg_u64(argc, argv, {"--partition-task-grain", "-partition-task-grain"}, partition_task_grain_u64);
    const bool has_join_task_grain = read_arg_u64(argc, argv, {"--join-task-grain", "-join-task-grain"}, join_task_grain_u64);
    const bool has_offset_task_grain = read_arg_u64(argc, argv, {"--offset-task-grain", "-offset-task-grain"}, offset_task_grain_u64);

    if (!has_partition_task_blocks && has_partition_task_grain) {
        partition_task_blocks_u64 = partition_task_grain_u64;
    } else if (!has_partition_task_blocks && partition_chunk_u64 != 0) {
        partition_task_blocks_u64 = partition_chunk_u64;
    }
    if (!has_join_task_partitions && has_join_task_grain) {
        join_task_partitions_u64 = join_task_grain_u64;
    } else if (!has_join_task_partitions && join_chunk_u64 != 0) {
        join_task_partitions_u64 = join_chunk_u64;
    }
    if (!has_offset_task_partitions && has_offset_task_grain) {
        offset_task_partitions_u64 = offset_task_grain_u64;
    }

    if (p > std::numeric_limits<std::uint32_t>::max()) {
        std::cerr << "Error: P too large.\n";
        return 1;
    }
    if (partition_block_size_u64 == 0) {
        std::cerr << "Error: partition block size must be greater than zero.\n";
        return 1;
    }
    if (partition_task_blocks_u64 == 0) {
        std::cerr << "Error: partition task block count must be greater than zero.\n";
        return 1;
    }
    if (join_task_partitions_u64 == 0) {
        std::cerr << "Error: join task partition count must be greater than zero.\n";
        return 1;
    }
    if (offset_task_partitions_u64 == 0) {
        std::cerr << "Error: offset task partition count must be greater than zero.\n";
        return 1;
    }

    OmpTaskConfig cfg{};
    try {
        cfg.partition_threads = checked_positive_int(part_threads_u64, "partition thread count");
        cfg.join_threads = checked_positive_int(join_threads_u64, "join thread count");
        cfg.partition_chunk = checked_nonnegative_int(partition_chunk_u64, "partition chunk size");
        cfg.join_chunk = checked_nonnegative_int(join_chunk_u64, "join chunk size");
        cfg.partition_task_blocks = checked_positive_int(partition_task_blocks_u64, "partition task block count");
        cfg.join_task_partitions = checked_positive_int(join_task_partitions_u64, "join task partition count");
        cfg.offset_task_partitions = checked_positive_int(offset_task_partitions_u64, "offset task partition count");
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    cfg.partition_block_size = static_cast<std::size_t>(partition_block_size_u64);
    cfg.partition_schedule_name = partition_schedule_name;
    cfg.join_schedule_name = join_schedule_name;



    DatasetConfig dataset_cfg{};
    if (!parse_dataset_type(dataset_type_name, dataset_cfg)) {
        std::cerr << "Error: invalid dataset type '" << dataset_type_name
                  << "'. Use uniform or skewed_<record_percentage>_<hot_partition_percentage>, e.g. skewed_80_5.\n";
        return 1;
    }

    // Keep benchmark runs controlled. Affinity can still be set externally with
    // OMP_PROC_BIND and OMP_PLACES.
    omp_set_dynamic(0);

    const std::uint32_t P = static_cast<std::uint32_t>(p);

    if (!is_power_of_two(P)) {
        std::cerr << "Error: in this reference implementation, P must be a power of two.\n";
        return 1;
    }

    const std::size_t NR = static_cast<std::size_t>(nr);
    const std::size_t NS = static_cast<std::size_t>(ns);

    const auto R = generate_relation(NR, seed, max_key, P, dataset_cfg);
    const auto S = generate_relation(NS, seed ^ 0xdeadebdecdeedef1ULL, max_key, P, dataset_cfg);

    double t0 = get_time();
    const JoinResult result = partitioned_hash_join(R, S, P, cfg);
    double t1 = get_time();
    const double tot_time_sec = t1 - t0;
    bool verified = false;

    std::cout << "executable=" << std::filesystem::path(argv[0]).stem().string() << "\n";
    std::cout << "dataset-type=" << dataset_cfg.type << "\n";
    std::cout << "join_count=" << result.join_count << "\n";
    std::cout << "checksum1=" << result.checksum1 << "\n";
    std::cout << "checksum2=" << result.checksum2 << "\n";

    if (NR <= 500 && NS <= 500) {
        const JoinResult naive = naive_join_verifier(R, S);
        verified = naive.join_count == result.join_count &&
                   naive.checksum1 == result.checksum1 &&
                   naive.checksum2 == result.checksum2;
        std::cout << "naive_join_count=" << naive.join_count << "\n";
        std::cout << "naive_checksum1=" << naive.checksum1 << "\n";
        std::cout << "naive_checksum2=" << naive.checksum2 << "\n";
        std::cout << "verified=" << (verified ? "true" : "false") << "\n";
        if (!verified) {
            std::cerr << "Error: naive verifier mismatch.\n";
            return 1;
        }
    }

    const std::uint64_t total_elements = NR + NS;
    const double part_throughput = compute_throughput(total_elements, result.part_time_sec);
    const double join_throughput = compute_throughput(total_elements, result.join_time_sec);
    const double total_throughput = compute_throughput(total_elements, tot_time_sec);

    const ResultMap results_map = {
        {"checksum1", std::to_string(result.checksum1)},
        {"checksum2", std::to_string(result.checksum2)},
        {"join_count", std::to_string(result.join_count)},
        {"verified", verified ? "true" : "false"},
        {"dataset_type", dataset_cfg.type},
        {"join_throughput", std::to_string(join_throughput)},
        {"total_throughput", std::to_string(total_throughput)},
        {"partition_time", std::to_string(result.part_time_sec)},
        {"partition_throughput", std::to_string(part_throughput)},
        {"join_time", std::to_string(result.join_time_sec)},
        {"partition_threads", std::to_string(cfg.partition_threads)},
        {"join_threads", std::to_string(cfg.join_threads)},
        {"partition_schedule", cfg.partition_schedule_name},
        {"join_schedule", cfg.join_schedule_name},
        {"partition_chunk", std::to_string(cfg.partition_chunk)},
        {"join_chunk", std::to_string(cfg.join_chunk)},
        {"partition_task_blocks", std::to_string(cfg.partition_task_blocks)},
        {"join_task_partitions", std::to_string(cfg.join_task_partitions)},
        {"offset_task_partitions", std::to_string(cfg.offset_task_partitions)},
        {"partition_task_grain", std::to_string(cfg.partition_task_blocks)},
        {"join_task_grain", std::to_string(cfg.join_task_partitions)},
        {"offset_task_grain", std::to_string(cfg.offset_task_partitions)},
        {"partition_block_size", std::to_string(cfg.partition_block_size)},
        {"max_key", std::to_string(max_key)},
        {"nr", std::to_string(NR)},
        {"ns", std::to_string(NS)},
        {"time_sec", std::to_string(tot_time_sec)}
    };

    const std::string filepath = "results/" + std::filesystem::path(argv[0]).stem().string() + ".csv";
    append_to_csv(filepath, results_map);

    return 0;
}
