// hashjoin_omp_loop_opt_wb.cpp
//
// Loop-level OpenMP implementation for Module 3
// Partitioned Hash Join with Duplicates
//
// This code is intentionally written to be simple and readable.
// It is meant as a reference baseline and as a starting point for
// the parallel version. You can modify it for improving performance,
// provided you do not change the overall algorithm.
// 
//
// IMPORTANT:
// The function compute_partition_id(...) below is intentionally very simple.
// Students must replace it with their own mapping function from Module 1.
// The same mapping function must be used consistently in both the sequential
// and parallel versions.
//
// Run example:
//   ./hashjoin_seq -nr 5 -ns 8 -seed 13 -max-key 8 -p 4
//
// Output:
//   join_count
//   checksum1
//   checksum2
//
//
// The code follows these phases:
//
//   1. Input generation
//      Generate two relations R and S with deterministic keys.
//
//   2. Partitioning of R and S
//      The goal of this phase is to reorganize the data so that
//      records belonging to the same partition are stored contiguously.
//
//      This is done in three steps:
//
//      - mapping key -> partition id
//        Each key is mapped to a partition identifier in [0, P).
//
//      - histogram
//        Count how many records are assigned to each partition.
//        This tells us how much space each partition will occupy.
//
//      - prefix sum (offset computation)
//        Convert counts into starting positions (offsets) for each partition
//        in the output array.
//
//      - scatter
//        Move each record to its correct position so that all records
//        of the same partition are stored contiguously.
//
//      After this phase, each partition corresponds to a contiguous
//      segment of the array, and can be processed independently.
//
//   3. Local join per partition
//      For each partition p:
//
//      - build
//        Scan the R partition and count how many times each key appears.
//
//      - probe
//        Scan the corresponding S partition.
//        For each key, if it exists in R, add as many matches as its multiplicity.
//
//   4. Final output
//      Accumulate results across all partitions.
//
// The result does NOT materialize the join pairs.
// It only computes:
//   - total number of matches
//   - two checksums for correctness verification
//

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
//
// For this reference implementation we only store the key.
// You may extend the record with a payload in later versions if desired.
//
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
        << " -nr NR -ns NS -seed SEED -max-key K -p P [--partition-threads T] [--join-threads T] [--mpi-nodes N] [--mpi-processes R]\n\n"
        << "Parameters:\n"
        << "  -nr         Number of records in relation R\n"
        << "  -ns         Number of records in relation S\n"
        << "  -seed       Deterministic seed\n"
        << "  -max-key    Keys are generated in [0, max-key)\n"
        << "  -p          Number of partitions (power of two required in this reference code)\n"
        << "  --partition-threads / -partition-threads   Number of threads for partition phase (reserved)\n"
        << "  --join-threads / -join-threads             Number of threads for join phase (reserved)\n"
        << "  --mpi-nodes / -mpi-nodes                   Accepted for launcher compatibility and ignored\n"
        << "  --mpi-processes / -mpi-processes           Accepted for launcher compatibility and ignored\n"
        << "  --mpi-partition-strategy / -mpi-partition-strategy  Accepted for launcher compatibility and ignored\n"
        << "  --dataset-type uniform|skewed_<record_pct>_<hot_partition_pct>\n";
}
static bool is_power_of_two(std::uint32_t x) {
    return x != 0 && (x & (x - 1U)) == 0;
}


// ------------------------------------------------------------
// OpenMP configuration
// ------------------------------------------------------------
//
// This loop-level OpenMP version is intentionally configurable from the
// command line, so that a bash script can perform a grid search over thread
// counts, schedules, chunk sizes, and partitioning block granularity.
//
// The implementation uses schedule(runtime) in the OpenMP loops and calls
// omp_set_schedule(...) immediately before each phase. This avoids relying on
// one global OMP_SCHEDULE value for all phases: the partitioning and join
// phases can be tuned independently.
//
struct OmpConfig {
    int partition_threads = 1;
    int join_threads = 1;
    omp_sched_t partition_schedule = omp_sched_static;
    omp_sched_t join_schedule = omp_sched_static;
    int partition_chunk = 0;
    int join_chunk = 0;
    std::size_t partition_block_size = 65536;
    std::string partition_schedule_name = "static";
    std::string join_schedule_name = "static";
    std::string dataset_type_name = "uniform";
};

static bool parse_omp_schedule_kind(const std::string& s, omp_sched_t& kind) {
    if (s == "static")  { kind = omp_sched_static;  return true; }
    if (s == "dynamic") { kind = omp_sched_dynamic; return true; }
    if (s == "guided")  { kind = omp_sched_guided;  return true; }
    if (s == "auto")    { kind = omp_sched_auto;    return true; }
    return false;
}

static int checked_thread_count(std::uint64_t value, const char* name) {
    if (value == 0 || value > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error(std::string("Invalid ") + name + ": must be in [1, INT_MAX]");
    }
    return static_cast<int>(value);
}

static int checked_chunk_size(std::uint64_t value, const char* name) {
    if (value > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error(std::string("Invalid ") + name + ": too large");
    }
    return static_cast<int>(value); // 0 means runtime/compiler default
}


// ------------------------------------------------------------
// Deterministic pseudo-random generation
// ------------------------------------------------------------
//
// We use splitmix64 to generate reproducible keys and also for checksum.
// https://rosettacode.org/wiki/Pseudo-random_numbers/Splitmix64
//
// splitmix64_next is used as a deterministic pseudo-random generator step,
// while splitmix64 is used as a stateless 64-bit mixing function for checksums. 
//
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
//
// This mapping is deliberately minimal.
// It is here only so that the reference code is complete and runnable.
//
// Students must replace this function with their own implementation from Module 1.
// The same mapping function must be used consistently in both the sequential
// and parallel versions to ensure a fair performance comparison.
//
// If P is a power of two, then key & (P-1) maps into [0, P).
// This is fast, but intentionally simplistic.
//
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
// Blocked histogram for OpenMP partitioning
// ------------------------------------------------------------
//
// A naive parallel scatter with one shared cursor per partition would require
// an atomic increment for every record. That is correct but usually too costly.
//
// Instead, this implementation splits the input into contiguous blocks. For
// each block we compute a private histogram. Then, for each partition, we prefix
// the per-block counts to obtain a disjoint output range for every block.
// The scatter phase can then run in parallel without atomics and without races.
//
struct HistogramResult {
    std::vector<std::size_t> hist;
    std::vector<std::size_t> block_hist; // flattened: block_hist[block * P + pid]
    std::size_t num_blocks = 0;
};

static HistogramResult compute_histogram(const std::vector<Record>& data,
                                         std::uint32_t P,
                                         const OmpConfig& cfg) {
    const std::size_t n = data.size();
    const std::size_t block_size = cfg.partition_block_size;
    const std::size_t num_blocks = (n + block_size - 1) / block_size;
    const std::size_t P_size = static_cast<std::size_t>(P);

    HistogramResult out;
    out.hist.assign(P_size, 0);
    out.block_hist.assign(num_blocks * P_size, 0);
    out.num_blocks = num_blocks;

    omp_set_schedule(cfg.partition_schedule, cfg.partition_chunk);

    #pragma omp parallel num_threads(cfg.partition_threads) default(none) shared(data, out) firstprivate(P, P_size, block_size, num_blocks)
    {
        #pragma omp for schedule(runtime)
        for (std::int64_t b_signed = 0; b_signed < static_cast<std::int64_t>(num_blocks); ++b_signed) {
            const std::size_t b = static_cast<std::size_t>(b_signed);
            const std::size_t begin = b * block_size;
            const std::size_t end = std::min(begin + block_size, data.size());
            std::size_t* local = out.block_hist.data() + b * P_size;

            for (std::size_t i = begin; i < end; ++i) {
                const std::uint32_t pid = compute_partition_id(data[i].key, P);
                ++local[pid];
            }
        }

        #pragma omp for schedule(static)
        for (std::int64_t pid_signed = 0; pid_signed < static_cast<std::int64_t>(P); ++pid_signed) {
            const std::size_t pid = static_cast<std::size_t>(pid_signed);
            std::size_t sum = 0;
            for (std::size_t b = 0; b < num_blocks; ++b) {
                sum += out.block_hist[b * P_size + pid];
            }
            out.hist[pid] = sum;
        }
    }

    return out;
}

// ------------------------------------------------------------
// Prefix sum (exclusive scan)
// ------------------------------------------------------------
//
// Given a histogram, compute the begin offsets of each partition.
//
// Example:
//   hist  = [0, 1, 2, 5]
//   begin = [0, 0, 1, 3]
//
// Then partition p occupies [begin[p], begin[p] + hist[p]).
//
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
//
// Reorder records so that all records belonging to the same partition become
// contiguous in memory. This OpenMP version is race-free without atomics:
// every input block receives a private output interval for each partition.
//
static std::vector<Record> scatter_partitioned(const std::vector<Record>& data,
                                               std::uint32_t P,
                                               const std::vector<std::size_t>& begin,
                                               const HistogramResult& hist_result,
                                               const OmpConfig& cfg) {
    std::vector<Record> out(data.size());

    const std::size_t num_blocks = hist_result.num_blocks;
    const std::size_t block_size = cfg.partition_block_size;
    const std::size_t P_size = static_cast<std::size_t>(P);
    std::vector<std::size_t> block_offsets(num_blocks * P_size, 0);

    omp_set_schedule(cfg.partition_schedule, cfg.partition_chunk);

    #pragma omp parallel num_threads(cfg.partition_threads) default(none) shared(data, out, block_offsets, hist_result, begin) firstprivate(P, P_size, block_size, num_blocks)
    {
        #pragma omp for schedule(static)
        for (std::int64_t pid_signed = 0; pid_signed < static_cast<std::int64_t>(P); ++pid_signed) {
            const std::size_t pid = static_cast<std::size_t>(pid_signed);
            std::size_t running = begin[pid];
            for (std::size_t b = 0; b < num_blocks; ++b) {
                block_offsets[b * P_size + pid] = running;
                running += hist_result.block_hist[b * P_size + pid];
            }
        }

        #pragma omp for schedule(runtime)
        for (std::int64_t b_signed = 0; b_signed < static_cast<std::int64_t>(num_blocks); ++b_signed) {
            const std::size_t b = static_cast<std::size_t>(b_signed);
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

    return out;
}

// ------------------------------------------------------------
// Partitioned relation metadata
// ------------------------------------------------------------
//
// This stores:
//   - the partitioned array
//   - the begin offset of each partition
//   - the end offset of each partition
//
// So partition p is data[begin[p] .. end[p]).
//
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
//
// This groups together the steps:
//   histogram -> prefix sum -> scatter -> end offsets
//
// After this phase, all records belonging to the same partition
// are stored contiguously in memory, enabling independent processing.
//
static PartitionedRelation partition_relation(const std::vector<Record>& rel,
                                              std::uint32_t P,
                                              const OmpConfig& cfg) {
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
    double part_time_sec = 0.0; // reserved for timing the partition phase
    double join_time_sec = 0.0; // reserved for timing the join phase
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
//
// We process one partition p as follows:
//
//   Build:
//     Scan R_p and compute countR[key]
//
//   Probe:
//     Scan S_p
//     If key k is present in countR, then each occurrence in S_p matches
//     countR[k] occurrences in R_p.
//
// Duplicates are handled by counting occurrences in R.
// Each record in S contributes as many matches as the multiplicity
// of its key in the corresponding partition of R.
//
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

    FlatCountTable countR(r_end - r_begin);

    for (std::size_t i = r_begin; i < r_end; ++i) {
        countR.increment(Rpart.data[i].key);
    }

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
// Full sequential partitioned hash join
// ------------------------------------------------------------
//
// This is the end-to-end baseline:
//
//   1. Partition R
//   2. Partition S
//   3. For each partition p:
//        local build + local probe
//   4. Accumulate results
//
// Each partition can be processed independently.
// This property is the basis for parallelization in Module 2.
//
static JoinResult partitioned_hash_join(const std::vector<Record>& R,
                                        const std::vector<Record>& S,
                                        std::uint32_t p,
                                        const OmpConfig& cfg) {
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

    #pragma omp parallel for num_threads(cfg.join_threads) schedule(dynamic, 1) default(none) shared(partial, Rpart, Spart, work_items) firstprivate(item_count)
    for (std::int64_t idx_signed = 0; idx_signed < static_cast<std::int64_t>(item_count); ++idx_signed) {
        const std::size_t idx = static_cast<std::size_t>(idx_signed);
        const JoinWorkItem& item = work_items[idx];
        partial[idx] = join_one_partition_subrange(Rpart, Spart, item.pid, item.s_begin, item.s_end);
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
//
// This is useful only for debugging and correctness testing on tiny
// datasets. It checks all pairs directly, so its complexity is O(|R|*|S|).
//
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
    (void)argc;
    (void)argv;

    struct WeakScalingCase {
        std::size_t n;
        int threads;
    };

    const std::vector<WeakScalingCase> cases = {
        {2500000, 1},
        {5000000, 2},
        {10000000, 4},
        {20000000, 8},
        {40000000, 16},
        {80000000, 32}
    };
    const std::uint64_t seed = 13;
    const std::uint64_t max_key = 1000000;
    const std::uint32_t P = 256;
    const std::uint64_t mpi_nodes = 1;
    const std::uint64_t mpi_processes = 1;
    const std::string mpi_partition_strategy = "block";
    const std::vector<std::string> dataset_type_names = {
        "uniform",
        "skewed_90_5",
        "skewed_90_1"
    };
    const std::size_t repeat_count = 5;
    const std::size_t n_combs = dataset_type_names.size() * cases.size() * repeat_count;

    if (!is_power_of_two(P)) {
        std::cerr << "Error: in this reference implementation, P must be a power of two.\n";
        return 1;
    }

    omp_set_dynamic(0);

    std::size_t counter = 0;
    const std::string filepath = "results/weak_scaling.csv";
    for (const std::string& dataset_type_name : dataset_type_names) {
        DatasetConfig dataset_cfg{};
        if (!parse_dataset_type(dataset_type_name, dataset_cfg)) {
            std::cerr << "Error: invalid dataset type '" << dataset_type_name << "'.\n";
            return 1;
        }

        for (const WeakScalingCase& current : cases) {
            const std::size_t NR = current.n;
            const std::size_t NS = current.n;
            const auto R = generate_relation(NR, seed, max_key, P, dataset_cfg);
            const auto S = generate_relation(NS, seed ^ 0xdeadebdecdeedef1ULL, max_key, P, dataset_cfg);

            for (std::size_t repeat = 1; repeat <= repeat_count; ++repeat) {
                ++counter;

                OmpConfig cfg{};
                cfg.partition_threads = current.threads;
                cfg.join_threads = current.threads;
                cfg.partition_schedule = omp_sched_guided;
                cfg.join_schedule = omp_sched_guided;
                cfg.partition_schedule_name = "guided";
                cfg.join_schedule_name = "guided";
                cfg.partition_chunk = 8;
                cfg.join_chunk = 8;
                cfg.partition_block_size = 32768;
                cfg.dataset_type_name = dataset_cfg.type;

                std::cout << "[" << counter << "/" << n_combs << "] weak_scaling"
                          << " --> dataset_type=" << dataset_cfg.type
                          << " threads=" << current.threads
                          << " nr=" << NR
                          << " ns=" << NS
                          << " repeat=" << repeat << "/" << repeat_count << "\n";

                const double t0 = get_time();
                const JoinResult result = partitioned_hash_join(R, S, P, cfg);
                const double t1 = get_time();
                const double tot_time_sec = t1 - t0;

                const std::uint64_t total_elements = NR + NS;
                const double part_throughput = compute_throughput(total_elements, result.part_time_sec);
                const double join_throughput = compute_throughput(total_elements, result.join_time_sec);
                const double total_throughput = compute_throughput(total_elements, tot_time_sec);
                const ResultMap results_map = {
                    {"checksum1", std::to_string(result.checksum1)},
                    {"checksum2", std::to_string(result.checksum2)},
                    {"dataset_type", dataset_cfg.type},
                    {"join_chunk", std::to_string(cfg.join_chunk)},
                    {"join_count", std::to_string(result.join_count)},
                    {"join_schedule", cfg.join_schedule_name},
                    {"join_threads", std::to_string(cfg.join_threads)},
                    {"join_throughput", std::to_string(join_throughput)},
                    {"join_time", std::to_string(result.join_time_sec)},
                    {"max_key", std::to_string(max_key)},
                    {"mpi_nodes", std::to_string(mpi_nodes)},
                    {"mpi_partition_strategy", mpi_partition_strategy},
                    {"mpi_processes", std::to_string(mpi_processes)},
                    {"nr", std::to_string(NR)},
                    {"ns", std::to_string(NS)},
                    {"partition_block_size", std::to_string(cfg.partition_block_size)},
                    {"partition_chunk", std::to_string(cfg.partition_chunk)},
                    {"partition_schedule", cfg.partition_schedule_name},
                    {"partition_threads", std::to_string(cfg.partition_threads)},
                    {"partition_throughput", std::to_string(part_throughput)},
                    {"partition_time", std::to_string(result.part_time_sec)},
                    {"scaling_type", "weak"},
                    {"time_sec", std::to_string(tot_time_sec)},
                    {"total_throughput", std::to_string(total_throughput)},
                    {"verified", "false"}
                };
                append_to_csv(filepath, results_map);
            }
        }
    }

    return 0;
}
