/*
 * SPDX-FileCopyrightText: Copyright (c) 2025-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <utility>
#include <random>
#include <vector>

#include "nixl.h"
#include "mem_section.h"
#include "backend/backend_aux.h"

static constexpr size_t DESC_LEN   = 256;
#ifdef NDEBUG
static constexpr int ITERATIONS = 2048;
static constexpr int    WARMUP     = 64;
#else
static constexpr int    ITERATIONS = 64;
static constexpr int    WARMUP     = 8;
#endif

using Clock    = std::chrono::steady_clock;
using Duration = std::chrono::duration<double, std::micro>;
using Matrix   = std::vector<std::vector<double>>;

static std::vector<nixlSectionDesc>
generateDescs(size_t count, uintptr_t addr_offset, std::mt19937 &rng) {
    std::vector<nixlSectionDesc> descs(count);
    for (size_t i = 0; i < count; ++i) {
        descs[i].addr      = addr_offset +
                             static_cast<uintptr_t>(i * DESC_LEN * 2);
        descs[i].len       = DESC_LEN;
        descs[i].devId     = 0;
        descs[i].metadataP = nullptr;
    }
    std::shuffle(descs.begin(), descs.end(), rng);
    return descs;
}

enum class BatchPlacement { AFTER, INTERLEAVED };

static std::pair<std::vector<nixlSectionDesc>, std::vector<nixlSectionDesc>>
generateSplitDescs(size_t base_count,
                   size_t batch_count,
                   BatchPlacement placement,
                   std::mt19937 &rng) {
    if (placement == BatchPlacement::AFTER) {
        auto base = generateDescs(base_count, 0, rng);
        auto batch = generateDescs(batch_count, base_count * DESC_LEN * 2, rng);
        return {std::move(base), std::move(batch)};
    }

    size_t total = base_count + batch_count;
    std::vector<nixlSectionDesc> all(total);
    for (size_t i = 0; i < total; ++i) {
        all[i].addr = static_cast<uintptr_t>(i * DESC_LEN * 2);
        all[i].len = DESC_LEN;
        all[i].devId = 0;
        all[i].metadataP = nullptr;
    }
    std::shuffle(all.begin(), all.end(), rng);

    std::vector<nixlSectionDesc> base(all.begin(), all.begin() + base_count);
    std::vector<nixlSectionDesc> batch(all.begin() + base_count, all.end());
    return {std::move(base), std::move(batch)};
}

#ifdef HAVE_BULK_REMOVE
static nixl_reg_dlist_t
pickQueries(const nixlSecDescList &list, size_t count, std::mt19937 &rng) {
    size_t n = static_cast<size_t>(list.descCount());
    std::vector<size_t> indices(n);
    std::iota(indices.begin(), indices.end(), 0);
    std::shuffle(indices.begin(), indices.end(), rng);
    if (count > n) count = n;

    nixl_reg_dlist_t queries(DRAM_SEG);
    for (size_t i = 0; i < count; ++i) {
        const auto &d = list[indices[i]];
        queries.addDesc(nixlBlobDesc(d.addr, d.len, d.devId, ""));
    }
    return queries;
}
#endif

static double
median(std::vector<double> &v) {
    std::sort(v.begin(), v.end());
    size_t n = v.size();
    if (n % 2 == 0)
        return (v[n / 2 - 1] + v[n / 2]) / 2.0;
    return v[n / 2];
}

static nixlSecDescList
buildSortedList(const std::vector<nixlSectionDesc> &data) {
    nixlSecDescList list(DRAM_SEG);
    list.addDescs(std::vector<nixlSectionDesc>(data));
    return list;
}

static double
benchInsertPerElement(size_t base_size,
                      size_t batch_size,
                      BatchPlacement placement,
                      std::mt19937 &rng) {
    auto [base_data, batch_data] = generateSplitDescs(base_size, batch_size, placement, rng);
    auto base_list  = buildSortedList(base_data);

    for (int w = 0; w < WARMUP; ++w) {
        auto list = base_list;
        for (auto &d : batch_data) list.addDesc(d);
    }

    std::vector<double> times;
    times.reserve(ITERATIONS);
    for (int it = 0; it < ITERATIONS; ++it) {
        auto list = base_list;
        auto copy = batch_data;
        auto t0 = Clock::now();
        for (auto &d : copy) list.addDesc(d);
        auto t1 = Clock::now();
        times.push_back(Duration(t1 - t0).count());
    }

    return median(times) / static_cast<double>(batch_size);
}

static double
benchInsertBatch(size_t base_size, size_t batch_size, BatchPlacement placement, std::mt19937 &rng) {
    auto [base_data, batch_data] = generateSplitDescs(base_size, batch_size, placement, rng);
    auto base_list  = buildSortedList(base_data);

    for (int w = 0; w < WARMUP; ++w) {
        auto list = base_list;
        list.addDescs(std::vector<nixlSectionDesc>(batch_data));
    }

    std::vector<double> times;
    times.reserve(ITERATIONS);
    for (int it = 0; it < ITERATIONS; ++it) {
        auto list = base_list;
        auto copy = batch_data;
        auto t0 = Clock::now();
        list.addDescs(std::move(copy));
        auto t1 = Clock::now();
        times.push_back(Duration(t1 - t0).count());
    }

    return median(times) / static_cast<double>(batch_size);
}

#ifdef HAVE_BULK_REMOVE
static double
benchRemovePerElement(size_t list_size, size_t batch_size,
                      std::mt19937 &rng) {
    auto data = generateDescs(list_size, 0, rng);
    auto base_list = buildSortedList(data);
    auto queries = pickQueries(base_list, batch_size, rng);

    for (int w = 0; w < WARMUP; ++w) {
        auto list = base_list;
        for (int i = 0; i < queries.descCount(); ++i) {
            int idx = list.getIndex(queries[i]);
            if (idx >= 0) list.remDesc(idx);
        }
    }

    std::vector<double> times;
    times.reserve(ITERATIONS);
    for (int it = 0; it < ITERATIONS; ++it) {
        auto list = base_list;
        auto t0 = Clock::now();
        for (int i = 0; i < queries.descCount(); ++i) {
            int idx = list.getIndex(queries[i]);
            if (idx >= 0) list.remDesc(idx);
        }
        auto t1 = Clock::now();
        times.push_back(Duration(t1 - t0).count());
    }

    return median(times) / static_cast<double>(batch_size);
}

static double
benchRemoveBatch(size_t list_size, size_t batch_size, std::mt19937 &rng) {
    auto data = generateDescs(list_size, 0, rng);
    auto base_list = buildSortedList(data);
    auto queries = pickQueries(base_list, batch_size, rng);

    for (int w = 0; w < WARMUP; ++w) {
        auto list = base_list;
        std::vector<size_t> indices;
        for (int i = 0; i < queries.descCount(); ++i) {
            int idx = list.getIndex(queries[i]);
            if (idx >= 0) indices.push_back(static_cast<size_t>(idx));
        }
        list.bulkRemove(std::move(indices));
    }

    std::vector<double> times;
    times.reserve(ITERATIONS);
    for (int it = 0; it < ITERATIONS; ++it) {
        auto list = base_list;
        auto t0 = Clock::now();
        std::vector<size_t> indices;
        for (int i = 0; i < queries.descCount(); ++i) {
            int idx = list.getIndex(queries[i]);
            if (idx >= 0) indices.push_back(static_cast<size_t>(idx));
        }
        list.bulkRemove(std::move(indices));
        auto t1 = Clock::now();
        times.push_back(Duration(t1 - t0).count());
    }

    return median(times) / static_cast<double>(batch_size);
}
#endif

static void
printTable(const char *title,
           const std::vector<size_t> &row_sizes,
           const std::vector<size_t> &col_sizes,
           const Matrix &data) {
    std::printf("--- %s (per-desc us) ---\n", title);
    std::printf("%-8s", "L\\B");
    for (size_t c : col_sizes)
        std::printf("%10zu", c);
    std::printf("\n");

    for (size_t r = 0; r < row_sizes.size(); ++r) {
        std::printf("%-8zu", row_sizes[r]);
        for (size_t c = 0; c < col_sizes.size(); ++c) {
            if (data[r][c] < 0)
                std::printf("%10s", "-");
            else
                std::printf("%10.3f", data[r][c]);
        }
        std::printf("\n");
    }
    std::printf("\n");
}

static void
printSpeedupTable(const char *title,
                  const std::vector<size_t> &row_sizes,
                  const std::vector<size_t> &col_sizes,
                  const Matrix &per_elem,
                  const Matrix &batch) {
    std::printf("--- %s (speedup: per-element / batch) ---\n", title);
    std::printf("%-8s", "L\\B");
    for (size_t c : col_sizes)
        std::printf("%10zu", c);
    std::printf("\n");

    for (size_t r = 0; r < row_sizes.size(); ++r) {
        std::printf("%-8zu", row_sizes[r]);
        for (size_t c = 0; c < col_sizes.size(); ++c) {
            if (per_elem[r][c] < 0 || batch[r][c] < 0 || batch[r][c] == 0) {
                std::printf("%10s", "-");
            } else {
                double ratio = per_elem[r][c] / batch[r][c];
                char arrow = (ratio > 1.005) ? '^' : ' ';
                std::printf("%8.2fx%c", ratio, arrow);
            }
        }
        std::printf("\n");
    }
    std::printf("\n");
}

int main() {
    std::mt19937 rng(42);

    const std::vector<size_t> insert_list_sizes = {
        0, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096};
    const std::vector<size_t> remove_list_sizes = {8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096};
    const std::vector<size_t> batch_sizes       = {1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024};

    std::printf("=== nixlSecDescList Microbenchmark (matrix) ===\n");
    std::printf("Iterations: %d (+ %d warmup)\n\n", ITERATIONS, WARMUP);

    size_t iR = insert_list_sizes.size();
    size_t iC = batch_sizes.size();

    struct PlacementInfo {
        BatchPlacement placement;
        const char *label;
    };

    const PlacementInfo placements[] = {
        {BatchPlacement::AFTER, "append"},
        {BatchPlacement::INTERLEAVED, "interleaved"},
    };

    for (const auto &pi : placements) {
        Matrix ins_per_elem(iR, std::vector<double>(iC));
        Matrix ins_batch(iR, std::vector<double>(iC));

        for (size_t r = 0; r < iR; ++r) {
            for (size_t c = 0; c < iC; ++c) {
                size_t L = insert_list_sizes[r];
                size_t B = batch_sizes[c];
                std::printf("  bench insert [%s]  L=%-6zu B=%-6zu ...\r", pi.label, L, B);
                std::fflush(stdout);
                ins_per_elem[r][c] = benchInsertPerElement(L, B, pi.placement, rng);
                ins_batch[r][c] = benchInsertBatch(L, B, pi.placement, rng);
            }
        }
        std::printf("%-60s\n", "");

        char title[128];
        std::snprintf(title, sizeof(title), "Insertion per-element (%s)", pi.label);
        printTable(title, insert_list_sizes, batch_sizes, ins_per_elem);

        std::snprintf(title, sizeof(title), "Insertion batch (%s)", pi.label);
        printTable(title, insert_list_sizes, batch_sizes, ins_batch);

        std::snprintf(title, sizeof(title), "Insertion (%s)", pi.label);
        printSpeedupTable(title, insert_list_sizes, batch_sizes, ins_per_elem, ins_batch);
    }

#ifdef HAVE_BULK_REMOVE
    size_t rR = remove_list_sizes.size();
    size_t rC = batch_sizes.size();
    Matrix rem_per_elem(rR, std::vector<double>(rC, -1.0));
    Matrix rem_batch(rR, std::vector<double>(rC, -1.0));

    for (size_t r = 0; r < rR; ++r) {
        for (size_t c = 0; c < rC; ++c) {
            size_t L = remove_list_sizes[r];
            size_t B = batch_sizes[c];
            if (B > L) continue;
            std::printf("  bench remove  L=%-6zu B=%-6zu ...\r", L, B);
            std::fflush(stdout);
            rem_per_elem[r][c] = benchRemovePerElement(L, B, rng);
            rem_batch[r][c]    = benchRemoveBatch(L, B, rng);
        }
    }
    std::printf("%-60s\n", "");

    printTable("Removal per-element", remove_list_sizes, batch_sizes,
               rem_per_elem);
    printTable("Removal batch", remove_list_sizes, batch_sizes, rem_batch);
    printSpeedupTable("Removal", remove_list_sizes, batch_sizes,
                      rem_per_elem, rem_batch);
#endif

    return 0;
}
