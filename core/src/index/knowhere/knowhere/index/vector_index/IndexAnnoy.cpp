// Copyright (C) 2019-2020 Zilliz. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except in compliance
// with the License. You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied. See the License for the specific language governing permissions and limitations under the License.

#include "knowhere/index/vector_index/IndexAnnoy.h"

#include <algorithm>
#include <cassert>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "hnswlib/hnswalg.h"
#include "hnswlib/space_ip.h"
#include "hnswlib/space_l2.h"
#include "knowhere/common/Exception.h"
#include "knowhere/common/Log.h"
#include "knowhere/index/vector_index/adapter/VectorAdapter.h"
#include "knowhere/index/vector_index/helpers/FaissIO.h"

namespace milvus {
namespace knowhere {

BinarySet
IndexAnnoy::Serialize(const Config& config) {
    if (!index_) {
        KNOWHERE_THROW_MSG("index not initialize or trained");
    }

    auto metric_type_length = metric_type_.length();
    std::shared_ptr<uint8_t[]> metric_type(new uint8_t[metric_type_length]);
    memcpy(metric_type.get(), metric_type_.data(), metric_type_.length());

    auto dim = Dim();
    std::shared_ptr<uint8_t[]> dim_data(new uint8_t[sizeof(uint64_t)]);
    memcpy(dim_data.get(), &dim, sizeof(uint64_t));

    auto index_length = index_->get_index_length();
    std::shared_ptr<uint8_t[]> index_data(new uint8_t[index_length]);
    memcpy(index_data.get(), index_->get_index(), (size_t)index_length);

    BinarySet res_set;
    res_set.Append("annoy_metric_type", metric_type, metric_type_length);
    res_set.Append("annoy_dim", dim_data, sizeof(uint64_t));
    res_set.Append("annoy_index_data", index_data, index_length);
    return res_set;
}

void
IndexAnnoy::Load(const BinarySet& index_binary) {
    auto metric_type = index_binary.GetByName("annoy_metric_type");
    metric_type_.resize((size_t)metric_type->size + 1);
    memcpy(metric_type_.data(), metric_type->data.get(), (size_t)metric_type->size);

    auto dim_data = index_binary.GetByName("annoy_dim");
    uint64_t dim;
    memcpy(&dim, dim_data->data.get(), (size_t)dim_data->size);

    if (metric_type_ == Metric::L2) {
        index_ = std::make_shared<AnnoyIndex<int64_t, float, ::Euclidean, ::Kiss64Random>>(dim);
    } else if (metric_type_ == Metric::IP) {
        index_ = std::make_shared<AnnoyIndex<int64_t, float, ::DotProduct, ::Kiss64Random>>(dim);
    } else {
        KNOWHERE_THROW_MSG("metric not supported " + metric_type_);
    }

    auto index_data = index_binary.GetByName("annoy_index_data");
    char* p = nullptr;
    if (!index_->load_index(index_data->data.get(), index_data->size, &p)) {
        std::string error_msg(p);
        free(p);
        KNOWHERE_THROW_MSG(error_msg);
    }
}

void
IndexAnnoy::BuildAll(const DatasetPtr& dataset_ptr, const Config& config) {
    if (index_) {
        // it is builded all
        return;
    }

    GETTENSORWITHIDS(dataset_ptr)

    metric_type_ = config[Metric::TYPE];
    if (metric_type_ == Metric::L2) {
        index_ = std::make_shared<AnnoyIndex<int64_t, float, ::Euclidean, ::Kiss64Random>>(dim);
    } else if (metric_type_ == Metric::IP) {
        index_ = std::make_shared<AnnoyIndex<int64_t, float, ::DotProduct, ::Kiss64Random>>(dim);
    } else {
        KNOWHERE_THROW_MSG("metric not supported " + metric_type_);
    }

    for (int i = 0; i < rows; ++i) {
        index_->add_item(p_ids[i], (const float*)p_data + dim * i);
    }

    index_->build(config[IndexParams::n_trees].get<int64_t>());
}

DatasetPtr
IndexAnnoy::Query(const DatasetPtr& dataset_ptr, const Config& config) {
    if (!index_) {
        KNOWHERE_THROW_MSG("index not initialize or trained");
    }

    GETTENSOR(dataset_ptr)
    auto k = config[meta::TOPK].get<int64_t>();
    auto search_k = config[IndexParams::search_k].get<int64_t>();
    auto all_num = rows * k;
    auto p_id = (int64_t*)malloc(all_num * sizeof(int64_t));
    auto p_dist = (float*)malloc(all_num * sizeof(float));
    faiss::ConcurrentBitsetPtr blacklist = GetBlacklist();

#pragma omp parallel for
    for (unsigned int i = 0; i < rows; ++i) {
        std::vector<int64_t> result;
        result.reserve(k);
        std::vector<float> distances;
        distances.reserve(k);
        index_->get_nns_by_vector((const float*)p_data + i * dim, k, search_k, &result, &distances, blacklist);

        memcpy(p_id + k * i, result.data(), k * sizeof(int64_t));
        memcpy(p_dist + k * i, distances.data(), k * sizeof(float));
    }

    auto ret_ds = std::make_shared<Dataset>();
    ret_ds->Set(meta::IDS, p_id);
    ret_ds->Set(meta::DISTANCE, p_dist);
    return ret_ds;
}

int64_t
IndexAnnoy::Count() {
    if (!index_) {
        KNOWHERE_THROW_MSG("index not initialize");
    }

    return index_->get_n_items();
}

int64_t
IndexAnnoy::Dim() {
    if (!index_) {
        KNOWHERE_THROW_MSG("index not initialize");
    }

    return index_->get_dim();
}

int64_t
IndexAnnoy::IndexSize() {
    if (index_size_ != -1) {
        return index_size_;
    }

    return index_size_ = Dim() * Count() * sizeof(float);
}
}  // namespace knowhere
}  // namespace milvus
