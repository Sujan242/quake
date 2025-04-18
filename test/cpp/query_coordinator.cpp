// query_coordinator_test.cpp

#include <gtest/gtest.h>
#include <torch/torch.h>
#include <memory>
#include <vector>
#include <stdexcept>
#include <cmath>
#include <algorithm>

#include "query_coordinator.h"
#include "partition_manager.h"
#include "quake_index.h"
#include "faiss/IndexFlat.h"
#include "faiss/IndexIVFFlat.h"

// Test fixture
class QueryCoordinatorTest : public ::testing::Test {
protected:
    int64_t dimension_ = 8;
    int64_t total_vectors_ = 40;
    int64_t nlist_ = 4;
    int64_t k_ = 5;
    int64_t num_queries_ = 5;
    std::shared_ptr<QuakeIndex> index_;
    torch::Tensor queries_;
    std::shared_ptr<PartitionManager> partition_manager_;
    MetricType metric_ = faiss::METRIC_L2;

    static torch::Tensor generate_random_data(int64_t num_vectors, int64_t dim) {
        return torch::randn({num_vectors, dim}, torch::kFloat32);
    }

    static torch::Tensor generate_sequential_ids(int64_t count, int64_t start = 0) {
        return torch::arange(start, start + count, torch::kInt64);
    }

    static std::shared_ptr<arrow::Table> generate_data_frame(int64_t num_vectors, torch::Tensor ids) {
        arrow::MemoryPool* pool = arrow::default_memory_pool();

        // Builders for the "price" and "id" columns
        arrow::DoubleBuilder price_builder(pool);
        arrow::Int64Builder id_builder(pool);

        // Append values to the builders
        for (int64_t i = 0; i < num_vectors; i++) {
            price_builder.Append(i); // Price column
            id_builder.Append(ids[i].item<int64_t>());          // ID column from the input tensor
        }

        // Finalize the arrays
        std::shared_ptr<arrow::Array> price_array;
        std::shared_ptr<arrow::Array> id_array;
        price_builder.Finish(&price_array);
        id_builder.Finish(&id_array);

        // Define the schema with two fields: "price" and "id"
        std::vector<std::shared_ptr<arrow::Field>> schema_vector = {
            arrow::field("id", arrow::int64()),
            arrow::field("price", arrow::float64()),
        };
        auto schema = std::make_shared<arrow::Schema>(schema_vector);

        // Create and return the table with both columns
        return arrow::Table::Make(schema, {id_array, price_array});
    }


    void SetUp() override {

        // Create dummy vectors and IDs
        torch::Tensor vectors = torch::randn({total_vectors_, dimension_}, torch::kFloat32);
        torch::Tensor ids = torch::arange(0, total_vectors_, torch::kInt64);
        // Build the QuakeIndex
        index_ = std::make_shared<QuakeIndex>();
        auto build_params = std::make_shared<IndexBuildParams>();
        build_params->nlist = nlist_;
        build_params->metric = "l2";
        index_->build(vectors, ids, build_params);
        partition_manager_ = index_->partition_manager_;
        
        // Create random queries
        queries_ = torch::randn({num_queries_, dimension_}, torch::kCPU);
    }
};

TEST_F(QueryCoordinatorTest, NullParentBatchedScanTest) {
    // Parent is null => QueryCoordinator scans all partitions (flat index scenario)
    auto coordinator = std::make_shared<QueryCoordinator>(
        nullptr /* parent */,
        index_->partition_manager_,
        nullptr,
        faiss::METRIC_L2);

    auto search_params = std::make_shared<SearchParams>();
    search_params->k = k_;
    // Because parent is null, the code sets batched_scan = true for us.

    auto result = coordinator->search(queries_, search_params);

    ASSERT_TRUE(result != nullptr);
    ASSERT_EQ(result->ids.sizes(), (std::vector<int64_t>{queries_.size(0), search_params->k}));
    ASSERT_EQ(result->distances.sizes(), (std::vector<int64_t>{queries_.size(0), search_params->k}));

    // Ensure no -1s remain if data_size >= k
    for (int64_t i = 0; i < num_queries_; i++) {
        for (int64_t j = 0; j < k_; j++) {
            ASSERT_NE(result->ids[i][j].item<int64_t>(), -1);
        }
    }
}

TEST_F(QueryCoordinatorTest, NullParentSerialScanTest) {
    // Force "serial_scan" by disabling batched_scan
    auto coordinator = std::make_shared<QueryCoordinator>(
        nullptr,
        index_->partition_manager_,
        nullptr,
        faiss::METRIC_L2);

    // We'll set batched_scan = false artificially to see if the serial path works
    auto search_params = std::make_shared<SearchParams>();
    search_params->k = 3;
    search_params->batched_scan = false; // Force serial scan

    auto result = coordinator->search(queries_, search_params);
    ASSERT_TRUE(result != nullptr);
    ASSERT_EQ(result->ids.size(0), queries_.size(0));
    ASSERT_EQ(result->ids.size(1), search_params->k);
}

TEST_F(QueryCoordinatorTest, NonNullParentTest) {
    auto coordinator = std::make_shared<QueryCoordinator>(
        index_->parent_,
        index_->partition_manager_,
        nullptr,
        faiss::METRIC_L2);

    auto search_params = std::make_shared<SearchParams>();
    search_params->k = 2; // final K
    search_params->nprobe = 1; // parent's search does a top-1

    auto result = coordinator->search(queries_, search_params);
    ASSERT_TRUE(result != nullptr);

    // The parent's result says "scan partition #0 only", so the coordinator
    // should have results from partition #0 only, for all queries.
    ASSERT_EQ(result->ids.size(0), queries_.size(0));
    ASSERT_EQ(result->ids.size(1), 2); // k=2
}

TEST_F(QueryCoordinatorTest, EmptyQueryTest) {
    // If queries is empty, we expect an empty result
    auto coordinator = std::make_shared<QueryCoordinator>(
        nullptr,
        index_->partition_manager_,
        nullptr,
        faiss::METRIC_L2);

    auto empty_queries = torch::empty({0, dimension_}, torch::kCPU);

    auto search_params = std::make_shared<SearchParams>();
    search_params->k = 2;

    auto result = coordinator->search(empty_queries, search_params);
    ASSERT_TRUE(result != nullptr);
    ASSERT_EQ(result->ids.numel(), 0);
    ASSERT_EQ(result->distances.numel(), 0);
}

TEST_F(QueryCoordinatorTest, NullPartitionManagerThrows) {
    // If PartitionManager is null, we expect the coordinator to throw
    auto coordinator = std::make_shared<QueryCoordinator>(
        nullptr,
        nullptr /* partition_manager_ = null */,
        nullptr,
        faiss::METRIC_L2);

    auto search_params = std::make_shared<SearchParams>();
    search_params->k = 2;

    // The code in serial_scan or batched_serial_scan throws if partition_manager_ is null
    EXPECT_THROW({
        coordinator->search(queries_, search_params);
    }, std::runtime_error);
}

TEST_F(QueryCoordinatorTest, WorkerInitializationTest) {
    auto coordinator = std::make_shared<QueryCoordinator>(
        index_->parent_,
        index_->partition_manager_,
        nullptr,
        faiss::METRIC_L2
    );

    // Check that the workers are not initialized
    ASSERT_FALSE(coordinator->workers_initialized_);

    coordinator = std::make_shared<QueryCoordinator>(
        index_->parent_,
        index_->partition_manager_,
        nullptr,
        faiss::METRIC_L2,
        4 /* num_workers */
    );

    ASSERT_TRUE(coordinator->workers_initialized_);
}

TEST_F(QueryCoordinatorTest, PreFilteringTest) {
    auto index = std::make_shared<QuakeIndex>();
    auto build_params = std::make_shared<IndexBuildParams>();
    build_params->nlist = 1;
    build_params->metric = "l2";
    int64_t num_vectors = 10;
    auto data_vectors = generate_random_data(num_vectors, dimension_);
    auto data_ids = generate_sequential_ids(num_vectors, 0);
    auto attributes_table = generate_data_frame(num_vectors, data_ids);
    index->build(data_vectors, data_ids, build_params, attributes_table);
    auto coordinator = std::make_shared<QueryCoordinator>(
        index->parent_,
        index->partition_manager_,
        nullptr,
        faiss::METRIC_L2
    );
    auto search_params = std::make_shared<SearchParams>();
    search_params->k = 2;
    search_params->filter_column = "price";
    search_params->filter_name = "less_equal";
    search_params->filter_value = arrow::Datum(1);
    search_params->filteringType = FilteringType::PRE_FILTERING;
    auto result_worker = coordinator->search(torch::randn({1, dimension_}, torch::kFloat32), search_params);
    vector<int64_t> expected_result = {0, 1};
    ASSERT_TRUE(result_worker != nullptr);
    ASSERT_EQ(result_worker->ids.sizes(), (std::vector<int64_t>{1, 2}));
    ASSERT_EQ(result_worker->distances.sizes(), (std::vector<int64_t>{1, 2}));
    std::vector<int64_t> result_worker_vector(result_worker->ids.data<int64_t>(), result_worker->ids.data<int64_t>() + result_worker->ids.numel());
    sort(result_worker_vector.begin(), result_worker_vector.end());
    ASSERT_EQ(expected_result, result_worker_vector);
}

TEST_F(QueryCoordinatorTest, PostFilteringTest) {
    auto index = std::make_shared<QuakeIndex>();
    auto build_params = std::make_shared<IndexBuildParams>();
    build_params->nlist = 1;
    build_params->metric = "l2";
    int64_t num_vectors = 10;
    auto data_vectors = generate_random_data(num_vectors, dimension_);
    auto data_ids = generate_sequential_ids(num_vectors, 0);
    auto attributes_table = generate_data_frame(num_vectors, data_ids);
    index->build(data_vectors, data_ids, build_params, attributes_table);
    auto coordinator = std::make_shared<QueryCoordinator>(
        index->parent_,
        index->partition_manager_,
        nullptr,
        faiss::METRIC_L2
    );
    auto search_params = std::make_shared<SearchParams>();
    search_params->k = 2;
    search_params->filter_column = "price";
    search_params->filter_name = "less_equal";
    search_params->filter_value = arrow::Datum(1);
    search_params->filteringType = FilteringType::POST_FILTERING;
    auto result_worker = coordinator->search(torch::randn({1, dimension_}, torch::kFloat32), search_params);
    vector<int64_t> expected_result = {0, 1};
    ASSERT_TRUE(result_worker != nullptr);
    ASSERT_EQ(result_worker->ids.sizes(), (std::vector<int64_t>{1, 2}));
    ASSERT_EQ(result_worker->distances.sizes(), (std::vector<int64_t>{1, 2}));
    std::vector<int64_t> result_worker_vector(result_worker->ids.data<int64_t>(), result_worker->ids.data<int64_t>() + result_worker->ids.numel());
    sort(result_worker_vector.begin(), result_worker_vector.end());
    ASSERT_EQ(expected_result, result_worker_vector);
}

TEST_F(QueryCoordinatorTest, FlatWorkerScan) {
    int num_workers = 4;

    // create flat index
    auto flat_index = std::make_shared<QuakeIndex>();
    auto build_params = std::make_shared<IndexBuildParams>();
    build_params->nlist = 1;
    build_params->metric = "l2";
    flat_index->build(torch::randn({20, dimension_}), torch::arange(20), build_params);

    // create coordinator with workers
    auto coordinator = std::make_shared<QueryCoordinator>(
        flat_index->parent_,
        flat_index->partition_manager_,
        nullptr,
        faiss::METRIC_L2,
        num_workers
    );

    auto search_params = std::make_shared<SearchParams>();
    search_params->k = 2;

    auto result_worker = coordinator->search(queries_, search_params);
    ASSERT_TRUE(result_worker != nullptr);
    ASSERT_EQ(result_worker->ids.sizes(), (std::vector<int64_t>{queries_.size(0), search_params->k}));
    ASSERT_EQ(result_worker->distances.sizes(), (std::vector<int64_t>{queries_.size(0), search_params->k}));
}

// Test that worker-based scan produces the same results as serial scan
TEST_F(QueryCoordinatorTest, WorkerScanCorrectnessTest) {
    // Initialize QueryCoordinator with workers
    int num_workers = 4;
    auto coordinator_worker = std::make_shared<QueryCoordinator>(
        index_->parent_,
        partition_manager_,
        nullptr,
        faiss::METRIC_L2,
        num_workers
    );

    // Define search parameters
    auto search_params = std::make_shared<SearchParams>();
    search_params->k = k_;
    search_params->nprobe = 2; // Number of partitions to scan

    // Perform worker-based scan
    auto result_worker = coordinator_worker->search(queries_, search_params);
    ASSERT_TRUE(result_worker != nullptr);
    ASSERT_EQ(result_worker->ids.sizes(), (std::vector<int64_t>{queries_.size(0), search_params->k}));
    ASSERT_EQ(result_worker->distances.sizes(), (std::vector<int64_t>{queries_.size(0), search_params->k}));

    // Initialize QueryCoordinator without workers for serial scan
    auto coordinator_serial = std::make_shared<QueryCoordinator>(
        index_->parent_,
        partition_manager_,
        nullptr,
        faiss::METRIC_L2
    );

    // Perform serial scan by forcing batched_scan = false
    auto search_params_serial = std::make_shared<SearchParams>();
    search_params_serial->k = k_;
    search_params_serial->nprobe = 2;
    search_params_serial->batched_scan = false; // Force serial scan

    auto result_serial = coordinator_serial->search(queries_, search_params_serial);
    ASSERT_TRUE(result_serial != nullptr);
    ASSERT_EQ(result_serial->ids.sizes(), (std::vector<int64_t>{queries_.size(0), search_params_serial->k}));
    ASSERT_EQ(result_serial->distances.sizes(), (std::vector<int64_t>{queries_.size(0), search_params_serial->k}));

    // Compare worker-based results with serial scan results
    for (int64_t q = 0; q < num_queries_; ++q) {
        for (int64_t i = 0; i < k_; ++i) {
            int64_t worker_id = result_worker->ids[q][i].item<int64_t>();
            int64_t serial_id = result_serial->ids[q][i].item<int64_t>();
            float worker_dist = result_worker->distances[q][i].item<float>();
            float serial_dist = result_serial->distances[q][i].item<float>();

            ASSERT_EQ(worker_id, serial_id) << "Mismatch in IDs for query " << q << ", rank " << i;
            ASSERT_NEAR(worker_dist, serial_dist, 1e-4) << "Mismatch in distances for query " << q << ", rank " << i;
        }
    }
}

// Test that workers handle empty partitions correctly
TEST_F(QueryCoordinatorTest, WorkerHandlesEmptyPartitionsTest) {

    // create a partition manager with one empty partition
    auto partition_manager = std::make_shared<PartitionManager>();
    shared_ptr<Clustering> clustering = std::make_shared<Clustering>();
    clustering->centroids = torch::randn({2, dimension_}, torch::kCPU);
    clustering->partition_ids = torch::arange(2, torch::kInt64);
    clustering->vectors = {torch::randn({0, dimension_}, torch::kCPU), torch::randn({10, dimension_}, torch::kCPU)};
    clustering->vector_ids = {torch::empty({0}, torch::kInt64), torch::arange(10, torch::kInt64)};

    auto parent = std::make_shared<QuakeIndex>();
    parent->build(clustering->centroids, clustering->partition_ids, std::make_shared<IndexBuildParams>());
    partition_manager->init_partitions(parent, clustering);

    // Initialize QueryCoordinator with workers
    int num_workers = 4;
    auto coordinator = std::make_shared<QueryCoordinator>(
        parent,
        partition_manager,
        nullptr,
        faiss::METRIC_L2,
        num_workers
    );

    // Define search parameters
    auto search_params = std::make_shared<SearchParams>();
    search_params->k = k_;
    search_params->nprobe = 2; // Scan all partitions

    // Perform worker-based scan
    auto result = coordinator->search(queries_, search_params);
    ASSERT_TRUE(result != nullptr);
    ASSERT_EQ(result->ids.sizes(), (std::vector<int64_t>{queries_.size(0), search_params->k}));
    ASSERT_EQ(result->distances.sizes(), (std::vector<int64_t>{queries_.size(0), search_params->k}));

    // Ensure that results are valid (no -1s if there are enough vectors in other partitions)
    for (int64_t q = 0; q < num_queries_; q++) {
        for (int64_t i = 0; i < k_; i++) {
            int64_t id = result->ids[q][i].item<int64_t>();
            float dist = result->distances[q][i].item<float>();

            // If k is greater than the number of vectors in other partitions, some IDs may still be -1
            // Depending on the total number of vectors, adjust the expectation
            if (total_vectors_ >= k_) {
                ASSERT_NE(id, -1) << "Found -1 ID for query " << q << ", rank " << i;
                ASSERT_GE(dist, 0.0f) << "Distance should be non-negative for query " << q << ", rank " << i;
            }
        }
    }
}

// Test that workers handle k greater than the number of vectors in a partition
TEST_F(QueryCoordinatorTest, WorkerHandlesKGreaterThanPartitionSizeTest) {
    // Simulate a partition with fewer vectors than k
    int small_partition_size = 2;
    auto partition_manager = std::make_shared<PartitionManager>();
    shared_ptr<Clustering> clustering = std::make_shared<Clustering>();
    clustering->centroids = torch::randn({2, dimension_}, torch::kCPU);
    clustering->partition_ids = torch::arange(2, torch::kInt64);
    clustering->vectors = {torch::randn({small_partition_size, dimension_}, torch::kCPU), torch::randn({small_partition_size, dimension_}, torch::kCPU)};
    clustering->vector_ids = {torch::arange({small_partition_size}, torch::kInt64) + 100, torch::arange(small_partition_size, torch::kInt64)};

    auto parent = std::make_shared<QuakeIndex>();
    parent->build(clustering->centroids, clustering->partition_ids, std::make_shared<IndexBuildParams>());
    partition_manager->init_partitions(parent, clustering);

    // Initialize QueryCoordinator with workers
    int num_workers = 4;
    auto coordinator = std::make_shared<QueryCoordinator>(
        parent,
        partition_manager,
        nullptr,
        faiss::METRIC_L2,
        num_workers
    );

    // Define search parameters
    auto search_params = std::make_shared<SearchParams>();
    search_params->k = 5; // Greater than the size of partition 1
    search_params->nprobe = 2; // Scan two partitions

    // Perform worker-based scan
    auto result = coordinator->search(queries_, search_params);
    ASSERT_TRUE(result != nullptr);
    ASSERT_EQ(result->ids.sizes(), (std::vector<int64_t>{queries_.size(0), search_params->k}));
    ASSERT_EQ(result->distances.sizes(), (std::vector<int64_t>{queries_.size(0), search_params->k}));

    // print the result
    std::cout << "Result: " << std::endl;
    std::cout << result->ids << std::endl;
    std::cout << result->distances << std::endl;

    // Check that the first two results come from the small partition and the rest are filled appropriately
    for (int64_t q = 0; q < num_queries_; q++) {
        for (int64_t i = 0; i < search_params->k; i++) {
            if (i < 2 * small_partition_size) {
                // IDs should be valid
                int64_t id = result->ids[q][i].item<int64_t>();
                float dist = result->distances[q][i].item<float>();
                ASSERT_NE(id, -1) << "Found -1 ID for query " << q << ", rank " << i;
                ASSERT_GE(dist, 0.0f) << "Distance should be non-negative for query " << q << ", rank " << i;
            } else {
                // IDs should be -1 and distances should be infinity or -infinity based on metric
                int64_t id = result->ids[q][i].item<int64_t>();
                float dist = result->distances[q][i].item<float>();
                ASSERT_EQ(id, -1) << "Expected -1 ID for query " << q << ", rank " << i;
                if (metric_ == faiss::METRIC_INNER_PRODUCT) {
                    ASSERT_EQ(dist, -std::numeric_limits<float>::infinity()) << "Expected -infinity distance for query " << q << ", rank " << i;
                } else {
                    ASSERT_EQ(dist, std::numeric_limits<float>::infinity()) << "Expected infinity distance for query " << q << ", rank " << i;
                }
            }
        }
    }
}

// Test that multiple workers can handle multiple queries simultaneously
TEST_F(QueryCoordinatorTest, MultipleWorkersMultipleQueriesTest) {
    // Initialize QueryCoordinator with multiple workers
    int num_workers = 4;
    auto coordinator = std::make_shared<QueryCoordinator>(
        index_->parent_,
        partition_manager_,
        nullptr,
        faiss::METRIC_L2,
        num_workers
    );

    // Define search parameters
    auto search_params = std::make_shared<SearchParams>();
    search_params->k = k_;
    search_params->nprobe = 3; // Scan three partitions

    // Perform worker-based scan
    auto result = coordinator->search(queries_, search_params);
    ASSERT_TRUE(result != nullptr);
    ASSERT_EQ(result->ids.sizes(), (std::vector<int64_t>{queries_.size(0), search_params->k}));
    ASSERT_EQ(result->distances.sizes(), (std::vector<int64_t>{queries_.size(0), search_params->k}));

    // Ensure that all results are valid
    for (int64_t q = 0; q < num_queries_; q++) {
        for (int64_t i = 0; i < k_; i++) {
            int64_t id = result->ids[q][i].item<int64_t>();
            float dist = result->distances[q][i].item<float>();
            ASSERT_NE(id, -1) << "Found -1 ID for query " << q << ", rank " << i;
            ASSERT_GE(dist, 0.0f) << "Distance should be non-negative for query " << q << ", rank " << i;
        }
    }
}

// Test that workers can be gracefully shut down and re-initialized
TEST_F(QueryCoordinatorTest, ShutdownWorkersTest) {
    // Initialize QueryCoordinator with workers
    int num_workers = 4;
    auto coordinator = std::make_shared<QueryCoordinator>(
        index_->parent_,
        partition_manager_,
        nullptr,
        faiss::METRIC_L2,
        num_workers
    );

    // Define search parameters
    auto search_params = std::make_shared<SearchParams>();
    search_params->k = k_;
    search_params->nprobe = 2;

    // Perform worker-based scan
    auto result_before_shutdown = coordinator->search(queries_, search_params);
    ASSERT_TRUE(result_before_shutdown != nullptr);
    ASSERT_EQ(result_before_shutdown->ids.sizes(), (std::vector<int64_t>{queries_.size(0), search_params->k}));
    ASSERT_EQ(result_before_shutdown->distances.sizes(), (std::vector<int64_t>{queries_.size(0), search_params->k}));

    // Shutdown workers
    coordinator->shutdown_workers();
    ASSERT_FALSE(coordinator->workers_initialized_);

    // Re-initialize workers
    coordinator->initialize_workers(num_workers);
    ASSERT_TRUE(coordinator->workers_initialized_);

    // Perform another worker-based scan
    auto result_after_restart = coordinator->search(queries_, search_params);
    ASSERT_TRUE(result_after_restart != nullptr);
    ASSERT_EQ(result_after_restart->ids.sizes(), (std::vector<int64_t>{queries_.size(0), search_params->k}));
    ASSERT_EQ(result_after_restart->distances.sizes(), (std::vector<int64_t>{queries_.size(0), search_params->k}));

    // Compare results before and after shutdown to ensure consistency
    for (int64_t q = 0; q < num_queries_; q++) {
        for (int64_t i = 0; i < k_; i++) {
            int64_t id_before = result_before_shutdown->ids[q][i].item<int64_t>();
            int64_t id_after = result_after_restart->ids[q][i].item<int64_t>();
            float dist_before = result_before_shutdown->distances[q][i].item<float>();
            float dist_after = result_after_restart->distances[q][i].item<float>();

            ASSERT_EQ(id_before, id_after) << "Mismatch in IDs after worker restart for query " << q << ", rank " << i;
            ASSERT_NEAR(dist_before, dist_after, 1e-4) << "Mismatch in distances after worker restart for query " << q << ", rank " << i;
        }
    }
}

// Test that workers handle zero partitions gracefully
TEST_F(QueryCoordinatorTest, WorkerScanZeroPartitionsTest) {
    // Initialize QueryCoordinator with workers
    int num_workers = 4;
    auto coordinator = std::make_shared<QueryCoordinator>(
        index_->parent_,
        partition_manager_,
        nullptr,
        faiss::METRIC_L2,
        num_workers
    );

    // Define search parameters with zero partitions to scan
    auto search_params = std::make_shared<SearchParams>();
    search_params->k = k_;
    search_params->nprobe = 0; // Zero partitions to scan

    // Generate partition_ids with all -1 (invalid)
    torch::Tensor zero_partitions = torch::full({num_queries_, 0}, -1, torch::kInt64);

    // Perform worker-based scan
    auto result = coordinator->scan_partitions(queries_, zero_partitions, search_params);
    ASSERT_TRUE(result != nullptr);
    ASSERT_EQ(result->ids.sizes(), (std::vector<int64_t>{num_queries_, search_params->k}));
    ASSERT_EQ(result->distances.sizes(), (std::vector<int64_t>{num_queries_, search_params->k}));

    // Check that all results are -1 IDs and infinity distances
    for (int64_t q = 0; q < num_queries_; q++) {
        for (int64_t i = 0; i < search_params->k; i++) {
            int64_t id = result->ids[q][i].item<int64_t>();
            float dist = result->distances[q][i].item<float>();
            ASSERT_EQ(id, -1) << "Expected -1 ID for query " << q << ", rank " << i;
            if (metric_ == faiss::METRIC_INNER_PRODUCT) {
                ASSERT_EQ(dist, -std::numeric_limits<float>::infinity()) << "Expected -infinity distance for query " << q << ", rank " << i;
            } else {
                ASSERT_EQ(dist, std::numeric_limits<float>::infinity()) << "Expected infinity distance for query " << q << ", rank " << i;
            }
        }
    }
}

class WorkerTest : public ::testing::Test {
protected:
    int64_t dimension_ = 128;
    int64_t total_vectors_ = 1000 * 1000;
    int64_t num_queries_ = 10000;
    Tensor queries_;
    Tensor vectors_;
    Tensor ids_;

    void SetUp() override {

        // Create dummy vectors and IDs
        vectors_ = torch::randn({total_vectors_, dimension_}, torch::kFloat32);
        ids_ = torch::arange(0, total_vectors_, torch::kInt64);
        queries_ = torch::randn({num_queries_, dimension_}, torch::kCPU);
    }
};

TEST_F(WorkerTest, FlatWorkerScan) {
    // create flat index
    auto build_params = std::make_shared<IndexBuildParams>();
    build_params->nlist = 1;
    build_params->metric = "l2";

    auto search_params = std::make_shared<SearchParams>();
    search_params->k = 10;
    search_params->batched_scan = true;

    vector<int64_t> num_workers = {0};
    // vector<int64_t> num_workers = {0};
    for (int64_t num_worker : num_workers) {
        auto flat_index = std::make_shared<QuakeIndex>();
        flat_index->build(vectors_, ids_, build_params);
        // create coordinator with workers
        auto coordinator = std::make_shared<QueryCoordinator>(
            flat_index->parent_,
            flat_index->partition_manager_,
            nullptr,
            faiss::METRIC_L2,
            num_worker
        );

        auto start = std::chrono::high_resolution_clock::now();
        auto result_worker = coordinator->search(queries_, search_params);
        auto end = std::chrono::high_resolution_clock::now();

        std::chrono::duration<double> elapsed_seconds = end - start;
        std::cout << "Elapsed time with " << num_worker << " workers: " << elapsed_seconds.count() << "s" << std::endl;

        ASSERT_TRUE(result_worker != nullptr);
        ASSERT_EQ(result_worker->ids.sizes(), (std::vector<int64_t>{queries_.size(0), search_params->k}));
        ASSERT_EQ(result_worker->distances.sizes(), (std::vector<int64_t>{queries_.size(0), search_params->k}));
    }

    auto faiss_flat_index = std::make_shared<faiss::IndexFlatL2>(dimension_);
    faiss_flat_index->add(total_vectors_, vectors_.data_ptr<float>());

    // search with faiss
    auto start = std::chrono::high_resolution_clock::now();
    Tensor distances = 10000000 * torch::ones({num_queries_, search_params->k}, torch::kFloat32);
    Tensor indices = -torch::ones({num_queries_, search_params->k}, torch::kInt64);
    faiss_flat_index->search(num_queries_, queries_.data_ptr<float>(), search_params->k, distances.data_ptr<float>(), indices.data_ptr<int64_t>());
    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed_seconds = end - start;
    std::cout << "Elapsed time with faiss: " << elapsed_seconds.count() << "s" << std::endl;
}

TEST_F(WorkerTest, IVFWorkerScan) {
    // create flat index
    auto build_params = std::make_shared<IndexBuildParams>();
    build_params->nlist = 1000;
    build_params->metric = "l2";

    auto search_params = std::make_shared<SearchParams>();
    search_params->k = 10;
    search_params->nprobe = 10;

    auto ivf_index = std::make_shared<QuakeIndex>();
    ivf_index->build(vectors_, ids_, build_params);

    vector<int64_t> num_workers = {0, 1};
    vector<bool> batched_scan = {true};
    // vector<int64_t> num_workers = {0};
    for (bool batch : batched_scan) {
        for (int64_t num_worker : num_workers) {
            // create coordinator with workers
            auto coordinator = std::make_shared<QueryCoordinator>(
                ivf_index->parent_,
                ivf_index->partition_manager_,
                nullptr,
                faiss::METRIC_L2,
                num_worker
            );

            search_params->batched_scan = batch;

            auto start = std::chrono::high_resolution_clock::now();
            auto result_worker = coordinator->search(queries_, search_params);
            auto end = std::chrono::high_resolution_clock::now();

            std::chrono::duration<double> elapsed_seconds = end - start;
            // std::cout << "Elapsed time with " << num_worker << " workers: " << elapsed_seconds.count() << "s" << std::endl;
            std::cout << "Elapsed time with " << num_worker << " workers and batched_scan = " << batch << ": " << elapsed_seconds.count() << "s" << std::endl;

            // print out contents of timing_info
            std::cout << "Timing info: " << std::endl;
            std::cout << "Total time: " << result_worker->timing_info->total_time_ns << std::endl;
            std::cout << "Job enqueue time: " << result_worker->timing_info->job_enqueue_time_ns << std::endl;
            std::cout << "Job wait time: " << result_worker->timing_info->job_wait_time_ns << std::endl;
            std::cout << "Buffer init time: " << result_worker->timing_info->buffer_init_time_ns << std::endl;
            std::cout << "Result agg time: " << result_worker->timing_info->result_aggregate_time_ns << std::endl;
            std::cout << "Bound dist time: " << result_worker->timing_info->boundary_distance_time_ns << std::endl;
            std::cout << "Parent total time: " << result_worker->timing_info->parent_info->total_time_ns << std::endl;

            ASSERT_TRUE(result_worker != nullptr);
            ASSERT_EQ(result_worker->ids.sizes(), (std::vector<int64_t>{queries_.size(0), search_params->k}));
            ASSERT_EQ(result_worker->distances.sizes(), (std::vector<int64_t>{queries_.size(0), search_params->k}));
        }
    }

    // search with faiss
    auto faiss_ivf_index = make_shared<faiss::IndexIVFFlat>(new faiss::IndexFlatL2(dimension_), dimension_, build_params->nlist);
    faiss_ivf_index->train(total_vectors_, vectors_.data_ptr<float>());
    faiss_ivf_index->add(total_vectors_, vectors_.data_ptr<float>());
    faiss_ivf_index->nprobe = search_params->nprobe;

    auto start = std::chrono::high_resolution_clock::now();
    Tensor distances = 10000000 * torch::ones({num_queries_, search_params->k}, torch::kFloat32);
    Tensor indices = -torch::ones({num_queries_, search_params->k}, torch::kInt64);
    faiss_ivf_index->search(num_queries_, queries_.data_ptr<float>(), search_params->k, distances.data_ptr<float>(), indices.data_ptr<int64_t>());
    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed_seconds = end - start;
    std::cout << "Elapsed time with faiss: " << elapsed_seconds.count() << "s" << std::endl;

}