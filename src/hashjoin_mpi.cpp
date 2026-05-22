// hashjoin_mpi.cpp
//
// MPI implementation for Module 4
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
#include <array>
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

#include <mpi.h>

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
        << " -nr NR -ns NS -seed SEED -max-key K -p P [--mpi-nodes N] [--mpi-processes R] [--mpi-partition-strategy block|cyclic]\n\n"
        << "Parameters:\n"
        << "  -nr         Number of records in relation R\n"
        << "  -ns         Number of records in relation S\n"
        << "  -seed       Deterministic seed\n"
        << "  -max-key    Keys are generated in [0, max-key)\n"
        << "  -p          Number of partitions (power of two required in this reference code)\n"
        << "  --dataset-type uniform|skewed_<record_pct>_<hot_partition_pct>\n"
        << "  --mpi-nodes / -mpi-nodes                   Requested MPI nodes allocated by SLURM (1..8)\n  --mpi-processes / -mpi-processes           Requested total MPI ranks used by the algorithm\n"
        << "  --mpi-partition-strategy / -mpi-partition-strategy  block|cyclic partition ownership\n"
        << "  --partition-threads / -partition-threads   Accepted for launcher compatibility and ignored\n"
        << "  --join-threads / -join-threads             Accepted for launcher compatibility and ignored\n"
        << "  --output-csv / -output-csv                 Output CSV path (default: results/<executable>.csv)\n";
}
static bool is_power_of_two(std::uint32_t x) {
    return x != 0 && (x & (x - 1U)) == 0;
}


// ------------------------------------------------------------
// MPI configuration
// ------------------------------------------------------------
struct MpiConfig {
    int requested_nodes = 1;
    int requested_processes = 1;
    int world_rank = 0;
    int world_size = 1;
    int active_ranks = 1;
    std::string partition_strategy = "block";
};

static int checked_mpi_nodes(std::uint64_t value) {
    if (value == 0 || value > 8) {
        throw std::runtime_error("Invalid MPI node count: must be in [1, 8]");
    }
    return static_cast<int>(value);
}

static int checked_mpi_processes(std::uint64_t value) {
    if (value == 0 || value > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("Invalid MPI process count: must be positive and fit in int");
    }
    return static_cast<int>(value);
}

static bool parse_mpi_partition_strategy(const std::string& value) {
    return value == "block" || value == "cyclic";
}

static int partition_owner(std::uint32_t pid, std::uint32_t P, const MpiConfig& cfg) {
    const std::uint32_t active = static_cast<std::uint32_t>(cfg.active_ranks);
    if (cfg.partition_strategy == "cyclic") {
        return static_cast<int>(pid % active);
    }

    // Inverse of the block interval assignment used in owns_partition().
    const std::uint32_t owner = static_cast<std::uint32_t>(
        ((static_cast<std::uint64_t>(pid) + 1ULL) * active - 1ULL) / P
    );
    return static_cast<int>(std::min<std::uint32_t>(owner, active - 1U));
}

static bool owns_partition(std::uint32_t pid, std::uint32_t P, const MpiConfig& cfg) {
    if (cfg.world_rank >= cfg.active_ranks) {
        return false;
    }
    return partition_owner(pid, P, cfg) == cfg.world_rank;
}

static std::pair<std::size_t, std::size_t> local_range_for_rank(std::size_t n, const MpiConfig& cfg) {
    if (cfg.world_rank >= cfg.active_ranks) {
        return {0, 0};
    }
    const std::size_t active = static_cast<std::size_t>(cfg.active_ranks);
    const std::size_t rank = static_cast<std::size_t>(cfg.world_rank);
    const std::size_t begin = (n * rank) / active;
    const std::size_t end = (n * (rank + 1U)) / active;
    return {begin, end};
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

static std::vector<Record> generate_relation_slice(std::size_t global_n,
                                                   std::size_t begin,
                                                   std::size_t end,
                                                   std::uint64_t seed,
                                                   std::uint64_t max_key,
                                                   std::uint32_t P,
                                                   const DatasetConfig& dataset_cfg) {
    if (begin > end || end > global_n) {
        throw std::runtime_error("Invalid local generation range");
    }

    std::vector<Record> out;
    out.reserve(end - begin);
    std::uint64_t state = seed;

    std::vector<std::uint64_t> skew_key_pool;
    if (dataset_cfg.skewed && dataset_cfg.skew_record_percent > 0) {
        skew_key_pool = build_skew_key_pool(P, max_key, dataset_cfg.hot_partition_percent);
        if (skew_key_pool.empty()) {
            throw std::runtime_error("Unable to generate skewed dataset: no keys map to hot partitions. Increase max-key or hot partition percentage.");
        }
    }

    // Reproduce the same global sequence generated by generate_relation(),
    // but keep only [begin, end). This is outside the timed region.
    for (std::size_t i = 0; i < end; ++i) {
        Record rec{};
        const std::uint64_t r = splitmix64_next(state);
        if (!skew_key_pool.empty() && (r % 100ULL) < dataset_cfg.skew_record_percent) {
            const std::uint64_t idx = splitmix64_next(state) % static_cast<std::uint64_t>(skew_key_pool.size());
            rec.key = skew_key_pool[static_cast<std::size_t>(idx)];
        } else {
            rec.key = (max_key == 0) ? 0ULL : (r % max_key);
        }

        if (i >= begin) {
            out.push_back(rec);
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
// Histogram
// ------------------------------------------------------------
static std::vector<std::size_t> compute_histogram(const std::vector<Record>& data, std::uint32_t P) {
    std::vector<std::size_t> hist(P, 0);

    for (const auto& record : data) {
        const std::uint32_t pid = compute_partition_id(record.key, P);
        ++hist[pid];
    }
    return hist;
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
static std::vector<Record> scatter_partitioned(const std::vector<Record>& data,
                                               std::uint32_t P,
                                               const std::vector<std::size_t>& begin) {
    std::vector<Record> out(data.size());
    std::vector<std::size_t> next = begin;

    for (const auto& record : data) {
        const std::uint32_t pid = compute_partition_id(record.key, P);
        out[next[pid]++] = record;
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
static PartitionedRelation partition_relation(const std::vector<Record>& rel, std::uint32_t P) {
    const auto hist = compute_histogram(rel, P);
    const auto begin = exclusive_prefix_sum(hist);
    auto data = scatter_partitioned(rel, P, begin);

    std::vector<std::size_t> end(P, 0);
    for (std::uint32_t pid = 0; pid < P; ++pid) {
        end[pid] = begin[pid] + hist[pid];
    }

    return PartitionedRelation{
        .data = std::move(data),
        .begin = begin,
        .end = end
    };
}

static std::vector<int> exclusive_prefix_sum_int(const std::vector<int>& counts) {
    std::vector<int> displs(counts.size(), 0);
    int running = 0;
    for (std::size_t i = 0; i < counts.size(); ++i) {
        displs[i] = running;
        if (counts[i] > std::numeric_limits<int>::max() - running) {
            throw std::runtime_error("MPI displacement overflow");
        }
        running += counts[i];
    }
    return displs;
}

static std::vector<Record> redistribute_relation_alltoallv(const std::vector<Record>& local_rel,
                                                           std::uint32_t P,
                                                           const MpiConfig& cfg,
                                                           MPI_Datatype record_type,
                                                           MPI_Comm comm) {
    std::vector<int> send_counts(static_cast<std::size_t>(cfg.world_size), 0);

    for (const auto& record : local_rel) {
        const std::uint32_t pid = compute_partition_id(record.key, P);
        const int dest = partition_owner(pid, P, cfg);
        ++send_counts[static_cast<std::size_t>(dest)];
    }

    const std::vector<int> send_displs = exclusive_prefix_sum_int(send_counts);
    const int total_send = send_counts.empty() ? 0 : send_displs.back() + send_counts.back();
    std::vector<Record> send_buffer(static_cast<std::size_t>(total_send));
    std::vector<int> next = send_displs;

    for (const auto& record : local_rel) {
        const std::uint32_t pid = compute_partition_id(record.key, P);
        const int dest = partition_owner(pid, P, cfg);
        send_buffer[static_cast<std::size_t>(next[static_cast<std::size_t>(dest)]++)] = record;
    }

    std::vector<int> recv_counts(static_cast<std::size_t>(cfg.world_size), 0);
    MPI_Alltoall(send_counts.data(), 1, MPI_INT,
                 recv_counts.data(), 1, MPI_INT,
                 comm);

    const std::vector<int> recv_displs = exclusive_prefix_sum_int(recv_counts);
    const int total_recv = recv_counts.empty() ? 0 : recv_displs.back() + recv_counts.back();
    std::vector<Record> recv_buffer(static_cast<std::size_t>(total_recv));

    MPI_Alltoallv(send_buffer.data(), send_counts.data(), send_displs.data(), record_type,
                  recv_buffer.data(), recv_counts.data(), recv_displs.data(), record_type,
                  comm);

    return recv_buffer;
}

// ------------------------------------------------------------
// Join result
// ------------------------------------------------------------
struct JoinResult {
    std::uint64_t join_count = 0;
    std::uint64_t checksum1 = 0;
    std::uint64_t checksum2 = 0;
    double part_time_sec = 0.0; // local partitioning after redistribution
    double join_time_sec = 0.0; // local join phase
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
static JoinResult join_one_partition(const PartitionedRelation& Rpart,
                                     const PartitionedRelation& Spart,
                                     std::uint32_t pid) {
    JoinResult result{};

    const std::size_t r_begin = Rpart.begin[pid];
    const std::size_t r_end = Rpart.end[pid];
    const std::size_t s_begin = Spart.begin[pid];
    const std::size_t s_end = Spart.end[pid];

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
static JoinResult partitioned_hash_join(const std::vector<Record>& local_R,
                                        const std::vector<Record>& local_S,
                                        std::uint32_t p,
                                        const MpiConfig& cfg,
                                        MPI_Datatype record_type,
                                        MPI_Comm comm) {
    JoinResult result{};

    const std::vector<Record> redistributed_R = redistribute_relation_alltoallv(local_R, p, cfg, record_type, comm);
    const std::vector<Record> redistributed_S = redistribute_relation_alltoallv(local_S, p, cfg, record_type, comm);

    double t0 = get_time();
    const PartitionedRelation Rpart = partition_relation(redistributed_R, p);
    const PartitionedRelation Spart = partition_relation(redistributed_S, p);
    double t1 = get_time();
    result.part_time_sec = t1 - t0;

    t0 = get_time();
    for (std::uint32_t pid = 0; pid < p; ++pid) {
        if (!owns_partition(pid, p, cfg)) {
            continue;
        }
        const JoinResult local = join_one_partition(Rpart, Spart, pid);
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

static JoinResult reduce_join_result(const JoinResult& local, MPI_Comm comm) {
    std::array<unsigned long long, 3> local_counts = {
        static_cast<unsigned long long>(local.join_count),
        static_cast<unsigned long long>(local.checksum1),
        static_cast<unsigned long long>(local.checksum2)
    };
    std::array<unsigned long long, 3> global_counts = {0ULL, 0ULL, 0ULL};

    MPI_Reduce(local_counts.data(), global_counts.data(), 3, MPI_UNSIGNED_LONG_LONG, MPI_SUM, 0, comm);

    JoinResult global{};
    global.join_count = static_cast<std::uint64_t>(global_counts[0]);
    global.checksum1 = static_cast<std::uint64_t>(global_counts[1]);
    global.checksum2 = static_cast<std::uint64_t>(global_counts[2]);
    MPI_Reduce(&local.part_time_sec, &global.part_time_sec, 1, MPI_DOUBLE, MPI_MAX, 0, comm);
    MPI_Reduce(&local.join_time_sec, &global.join_time_sec, 1, MPI_DOUBLE, MPI_MAX, 0, comm);
    return global;
}

// ------------------------------------------------------------
// Main
// ------------------------------------------------------------
int main(int argc, char** argv) {
    MPI_Init(&argc, &argv);

    int world_rank = 0;
    int world_size = 1;
    MPI_Comm_rank(MPI_COMM_WORLD, &world_rank);
    MPI_Comm_size(MPI_COMM_WORLD, &world_size);

    std::uint64_t nr = 0, ns = 0, seed = 0, max_key = 0, p = 0;
    std::uint64_t ignored_part_threads = 1;
    std::uint64_t ignored_join_threads = 1;
    std::uint64_t ignored_partition_chunk = 0;
    std::uint64_t ignored_join_chunk = 0;
    std::uint64_t ignored_partition_block_size = 65536;
    std::uint64_t mpi_nodes_u64 = static_cast<std::uint64_t>(world_size);
    std::uint64_t mpi_processes_u64 = static_cast<std::uint64_t>(world_size);
    std::string partition_schedule_name = "static";
    std::string join_schedule_name = "static";
    std::string dataset_type_name = "uniform";
    std::string mpi_partition_strategy = "block";
    std::string output_csv_path;

    if (!read_arg_u64(argc, argv, {"-nr"}, nr) ||
        !read_arg_u64(argc, argv, {"-ns"}, ns) ||
        !read_arg_u64(argc, argv, {"-seed"}, seed) ||
        !read_arg_u64(argc, argv, {"-max-key"}, max_key) ||
        !read_arg_u64(argc, argv, {"-p"}, p)) {
        if (world_rank == 0) {
            usage(argv[0]);
        }
        MPI_Finalize();
        return 1;
    }
    read_arg_u64(argc, argv, {"--partition-threads", "-partition-threads"}, ignored_part_threads);
    read_arg_u64(argc, argv, {"--join-threads", "-join-threads"}, ignored_join_threads);
    read_arg_u64(argc, argv, {"--partition-chunk", "-partition-chunk"}, ignored_partition_chunk);
    read_arg_u64(argc, argv, {"--join-chunk", "-join-chunk"}, ignored_join_chunk);
    read_arg_u64(argc, argv, {"--partition-block-size", "-partition-block-size"}, ignored_partition_block_size);
    read_arg_u64(argc, argv, {"--mpi-nodes", "-mpi-nodes"}, mpi_nodes_u64);
    read_arg_u64(argc, argv, {"--mpi-processes", "-mpi-processes"}, mpi_processes_u64);
    read_arg_string(argc, argv, {"--partition-schedule", "-partition-schedule"}, partition_schedule_name);
    read_arg_string(argc, argv, {"--join-schedule", "-join-schedule"}, join_schedule_name);
    read_arg_string(argc, argv, {"--dataset-type", "-dataset-type", "--dataset", "-dataset"}, dataset_type_name);
    read_arg_string(argc, argv, {"--mpi-partition-strategy", "-mpi-partition-strategy"}, mpi_partition_strategy);
    read_arg_string(argc, argv, {"--output-csv", "-output-csv", "--csv", "-csv"}, output_csv_path);

    (void)ignored_part_threads;
    (void)ignored_join_threads;
    (void)ignored_partition_chunk;
    (void)ignored_join_chunk;
    (void)ignored_partition_block_size;
    (void)partition_schedule_name;
    (void)join_schedule_name;

    if (p > std::numeric_limits<std::uint32_t>::max()) {
        if (world_rank == 0) {
            std::cerr << "Error: P too large.\n";
        }
        MPI_Finalize();
        return 1;
    }

    MpiConfig cfg{};
    try {
        cfg.requested_nodes = checked_mpi_nodes(mpi_nodes_u64);
        cfg.requested_processes = checked_mpi_processes(mpi_processes_u64);
    } catch (const std::exception& e) {
        if (world_rank == 0) {
            std::cerr << "Error: " << e.what() << "\n";
        }
        MPI_Finalize();
        return 1;
    }
    if (cfg.requested_processes > world_size) {
        if (world_rank == 0) {
            std::cerr << "Error: --mpi-processes cannot be larger than the number of launched MPI ranks. "
                      << "requested=" << cfg.requested_processes << ", launched=" << world_size << "\n";
        }
        MPI_Finalize();
        return 1;
    }
    if (!parse_mpi_partition_strategy(mpi_partition_strategy)) {
        if (world_rank == 0) {
            std::cerr << "Error: invalid MPI partition strategy '" << mpi_partition_strategy
                      << "'. Use block or cyclic.\n";
        }
        MPI_Finalize();
        return 1;
    }
    cfg.world_rank = world_rank;
    cfg.world_size = world_size;
    cfg.active_ranks = std::min(cfg.requested_processes, world_size);
    cfg.partition_strategy = mpi_partition_strategy;

    DatasetConfig dataset_cfg{};
    if (!parse_dataset_type(dataset_type_name, dataset_cfg)) {
        if (world_rank == 0) {
            std::cerr << "Error: invalid dataset type '" << dataset_type_name
                      << "'. Use uniform or skewed_<record_percentage>_<hot_partition_percentage>, e.g. skewed_80_5.\n";
        }
        MPI_Finalize();
        return 1;
    }

    const std::uint32_t P = static_cast<std::uint32_t>(p);

	// Power-of-two constraint on P
    if (!is_power_of_two(P)) {
        if (world_rank == 0) {
            std::cerr << "Error: in this reference implementation, P must be a power of two.\n";
        }
        MPI_Finalize();
        return 1;
    }

    const std::size_t NR = static_cast<std::size_t>(nr);
    const std::size_t NS = static_cast<std::size_t>(ns);

    // Each active rank generates a disjoint slice of the same global R/S sequences
    const auto [r_begin, r_end] = local_range_for_rank(NR, cfg);
    const auto [s_begin, s_end] = local_range_for_rank(NS, cfg);
    const auto R = generate_relation_slice(NR, r_begin, r_end, seed, max_key, P, dataset_cfg);
    const auto S = generate_relation_slice(NS, s_begin, s_end, seed ^ 0xdeadebdecdeedef1ULL, max_key, P, dataset_cfg);

    MPI_Datatype record_type;
    MPI_Type_contiguous(static_cast<int>(sizeof(Record)), MPI_BYTE, &record_type);
    MPI_Type_commit(&record_type);

    // Time only the join pipeline, not input generation
    MPI_Barrier(MPI_COMM_WORLD);
    double t0 = get_time();
    const JoinResult local_result = partitioned_hash_join(R, S, P, cfg, record_type, MPI_COMM_WORLD);
    const JoinResult result = reduce_join_result(local_result, MPI_COMM_WORLD);
    MPI_Barrier(MPI_COMM_WORLD);
    double t1 = get_time();
    const double tot_time_sec = t1 - t0;
    bool verified = false;

    if (world_rank != 0) {
        MPI_Type_free(&record_type);
        MPI_Finalize();
        return 0;
    }
    
    // Resulted output
    std::cout << "executable=" << std::filesystem::path(argv[0]).stem().string() << "\n";
    std::cout << "dataset_type=" << dataset_cfg.type << "\n";
    std::cout << "mpi_nodes=" << cfg.requested_nodes << "\n";
    std::cout << "mpi_processes=" << cfg.requested_processes << "\n";
    std::cout << "join_count=" << result.join_count << "\n";
    std::cout << "checksum1=" << result.checksum1 << "\n";
    std::cout << "checksum2=" << result.checksum2 << "\n";

    //Tiny debug check, only for very small datasets
    if (NR <= 500 && NS <= 500) {
        const auto R_full = generate_relation(NR, seed, max_key, P, dataset_cfg);
        const auto S_full = generate_relation(NS, seed ^ 0xdeadebdecdeedef1ULL, max_key, P, dataset_cfg);
        const JoinResult naive = naive_join_verifier(R_full, S_full);
        verified = naive.join_count == result.join_count &&
                   naive.checksum1 == result.checksum1 &&
                   naive.checksum2 == result.checksum2;
        std::cout << "naive_join_count=" << naive.join_count << "\n";
        std::cout << "naive_checksum1=" << naive.checksum1 << "\n";
        std::cout << "naive_checksum2=" << naive.checksum2 << "\n";
        std::cout << "verified=" << (verified ? "true" : "false") << "\n";
        if (!verified) {
            std::cerr << "Error: naive verifier mismatch.\n";
            MPI_Type_free(&record_type);
            MPI_Finalize();
            return 1;
        }
    }

    // Append results to csv file
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
        {"mpi_nodes", std::to_string(cfg.requested_nodes)},
        {"mpi_processes", std::to_string(cfg.requested_processes)},
        {"mpi_active_ranks", std::to_string(cfg.active_ranks)},
        {"mpi_world_size", std::to_string(cfg.world_size)},
        {"mpi_partition_strategy", cfg.partition_strategy},
        {"max_key", std::to_string(max_key)},
        {"nr", std::to_string(NR)},
        {"ns", std::to_string(NS)},
        {"time_sec", std::to_string(tot_time_sec)}
    };
    const std::string filepath = output_csv_path.empty()
        ? "results/" + std::filesystem::path(argv[0]).stem().string() + ".csv"
        : output_csv_path;
    append_to_csv(filepath, results_map);

    MPI_Type_free(&record_type);
    MPI_Finalize();
    return 0;
}
