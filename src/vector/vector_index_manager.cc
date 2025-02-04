// Copyright (c) 2023 dingodb.com, Inc. All Rights Reserved
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "vector/vector_index_manager.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include "common/logging.h"
#include "fmt/core.h"
#include "proto/error.pb.h"
#include "server/server.h"
#include "vector/codec.h"
#include "vector/vector_index.h"
namespace dingodb {

bool VectorIndexManager::Init(std::vector<store::RegionPtr> regions) {
  for (auto& region : regions) {
    // init vector index map
    const auto& definition = region->InnerRegion().definition();
    if (definition.index_parameter().index_type() == pb::common::IndexType::INDEX_TYPE_VECTOR) {
      DINGO_LOG(INFO) << fmt::format("Init load region {} vector index", region->Id());

      auto status = LoadVectorIndex(region);
      if (!status.ok()) {
        DINGO_LOG(ERROR) << fmt::format("Load region {} vector index failed, ", region->Id());
        return false;
      }

      status = SaveVectorIndex(region);
      if (!status.ok()) {
        DINGO_LOG(ERROR) << fmt::format("Save region {} vector index failed, ", region->Id());
        return false;
      }
    }
  }

  // Set apply log index
  std::vector<pb::common::KeyValue> kvs;
  if (!meta_reader_->Scan(Prefix(), kvs)) {
    DINGO_LOG(ERROR) << "Scan vector index meta failed!";
    return false;
  }
  DINGO_LOG(INFO) << "Init vector index meta num: " << kvs.size();

  if (!kvs.empty()) {
    TransformFromKv(kvs);
  }

  return true;
}

bool VectorIndexManager::AddVectorIndex(uint64_t region_id, std::shared_ptr<VectorIndex> vector_index) {
  return vector_indexs_.Put(region_id, vector_index) > 0;
}

bool VectorIndexManager::AddVectorIndex(uint64_t region_id, const pb::common::IndexParameter& index_parameter) {
  auto vector_index = VectorIndex::New(region_id, index_parameter);
  if (vector_index == nullptr) {
    return false;
  }
  return AddVectorIndex(region_id, vector_index);
}

void VectorIndexManager::DeleteVectorIndex(uint64_t region_id) {
  vector_indexs_.Erase(region_id);
  meta_writer_->Delete(GenKey(region_id));
}

std::shared_ptr<VectorIndex> VectorIndexManager::GetVectorIndex(uint64_t region_id) {
  auto vector_index = vector_indexs_.Get(region_id);
  if (vector_index == nullptr) {
    DINGO_LOG(WARNING) << fmt::format("Not found vector index {}", region_id);
  }

  return vector_index;
}

// Load vector index for already exist vector index at bootstrap.
butil::Status VectorIndexManager::LoadVectorIndex(store::RegionPtr region) {
  assert(region != nullptr);

  std::shared_ptr<VectorIndex> vector_index;

  // try to LoadVectorIndexFromDisk
  vector_index = LoadVectorIndexFromDisk(region);
  if (vector_index) {
    // replay wal
    DINGO_LOG(INFO) << fmt::format("Load vector index from disk, id {} success, will ReplayWal", region->Id());
    auto status = ReplayValToVectorIndex(vector_index, vector_index->ApplyLogIndex() + 1, UINT64_MAX);
    if (status.ok()) {
      DINGO_LOG(INFO) << fmt::format("ReplayWal success, id {}, log_id {}", region->Id(),
                                     vector_index->ApplyLogIndex());
      // set vector index to vector index map
      vector_indexs_.Put(region->Id(), vector_index);
      return status;
    }
  }

  DINGO_LOG(INFO) << fmt::format("Load vector index from disk, id {} failed, will build vector_index", region->Id());

  // build a new vector_index from scratch
  vector_index = BuildVectorIndex(region);
  if (!vector_index) {
    DINGO_LOG(WARNING) << fmt::format("Build vector index failed, id {}", region->Id());
    return butil::Status(pb::error::Errno::EINTERNAL, "Build vector index failed");
  }

  // add vector index to vector index map
  vector_indexs_.Put(region->Id(), vector_index);

  DINGO_LOG(INFO) << fmt::format("Build vector index success, id {}", region->Id());

  return butil::Status::OK();
}

// Load vector index for already exist vector index at bootstrap.
std::shared_ptr<VectorIndex> VectorIndexManager::LoadVectorIndexFromDisk(store::RegionPtr region) {
  assert(region != nullptr);

  // read vector index file log_id
  std::string vector_index_file_log_id_file_path =
      fmt::format("{}/{}.log_id", Server::GetInstance()->GetIndexPath(), region->Id());

  // open vector_index_file_log_id_file_path and read its content to a std::string
  uint64_t vector_index_file_log_id;
  if (!std::filesystem::exists(vector_index_file_log_id_file_path)) {
    DINGO_LOG(WARNING) << fmt::format(
        "Vector index {} log id file {} not exist, can't load, need to build vector_index", region->Id(),
        vector_index_file_log_id_file_path);
    return nullptr;
  } else {
    std::ifstream vector_index_file_log_id_file(vector_index_file_log_id_file_path);
    if (!vector_index_file_log_id_file.is_open()) {
      DINGO_LOG(ERROR) << fmt::format("Vector index {} log id file open failed, can't load, need to build vector_index",
                                      region->Id());
      return nullptr;
    } else {
      // read vector_index_file_log_id_file_path content to vector_index_file_log_id, then close it
      vector_index_file_log_id_file >> vector_index_file_log_id;
      vector_index_file_log_id_file.close();
    }
  }

  DINGO_LOG(INFO) << fmt::format("Vector index {} log id is {}", region->Id(), vector_index_file_log_id);

  // check if can load from file
  std::string vector_index_file_path =
      fmt::format("{}/{}_{}.idx", Server::GetInstance()->GetIndexPath(), region->Id(), vector_index_file_log_id);

  // check if file vector_index_file_path exists
  if (!std::filesystem::exists(vector_index_file_path)) {
    DINGO_LOG(ERROR) << fmt::format("Vector index {} file {} not exist, can't load, need to build vector_index",
                                    region->Id(), vector_index_file_path);
    return nullptr;
  }

  // create a new vector_index
  auto vector_index = VectorIndex::New(region->Id(), region->InnerRegion().definition().index_parameter());
  if (!vector_index) {
    DINGO_LOG(WARNING) << fmt::format("New vector index failed, id {}", region->Id());
    return nullptr;
  }

  // load index from file
  auto ret = vector_index->Load(vector_index_file_path);
  if (!ret.ok()) {
    DINGO_LOG(WARNING) << fmt::format("Load vector index failed, id {}", region->Id());
    return nullptr;
  }

  // set vector_index apply log id
  vector_index->SetApplyLogIndex(vector_index_file_log_id);

  return vector_index;
}

// Replay vector index from wal
butil::Status VectorIndexManager::ReplayValToVectorIndex(std::shared_ptr<VectorIndex> vector_index,
                                                         uint64_t start_log_id, uint64_t end_log_id) {
  assert(vector_index != nullptr);

  DINGO_LOG(INFO) << fmt::format("Replay vector index {} from log id {} to log id {}", vector_index->Id(), start_log_id,
                                 end_log_id);

  std::string start_key;
  std::string end_key;

  VectorCodec::EncodeVectorWal(vector_index->Id(), 0, start_log_id, start_key);
  VectorCodec::EncodeVectorWal(vector_index->Id(), UINT64_MAX, end_log_id, end_key);

  IteratorOptions options;
  options.lower_bound = start_key;
  options.upper_bound = end_key;
  auto iter = raw_engine_->NewIterator(Constant::kStoreDataCF, options);

  // replay vector wal data to vector index
  uint64_t last_log_id = vector_index->ApplyLogIndex();
  for (iter->Seek(start_key); iter->Valid(); iter->Next()) {
    pb::common::VectorWithId vector;

    std::string key(iter->Key());
    uint64_t vector_id = 0;
    uint64_t log_id = 0;
    VectorCodec::DecodeVectorWal(key, vector_id, log_id);

    if (log_id >= last_log_id) {
      last_log_id = log_id;
    } else {
      DINGO_LOG(WARNING) << fmt::format(
          "Replay vector index {} from log id {} to log id {}, now log id {}, last_log_id {}, vector id {}",
          vector_index->Id(), start_log_id, end_log_id, log_id, last_log_id, vector_id);
      continue;
    }

    DINGO_LOG(DEBUG) << fmt::format(
        "Replay vector index {} from log id {} to log id {}, now log id {}, vector id {}, value size {}",
        vector_index->Id(), start_log_id, end_log_id, log_id, vector_id, iter->Value().size());

    vector.set_id(vector_id);

    std::string value(iter->Value());

    // vector deleted
    if (value.empty() || value.length() == 0) {
      DINGO_LOG(DEBUG) << fmt::format("Replay vector index vector delete, vector id {}, log_id {}", vector.id(),
                                      log_id);
      vector_index->Delete(vector.id());
      continue;
    }

    if (!vector.mutable_vector()->ParseFromString(value)) {
      DINGO_LOG(WARNING) << fmt::format("vector ParseFromString failed, id {}", vector.id());
      continue;
    }

    if (vector.vector().float_values_size() <= 0) {
      DINGO_LOG(WARNING) << fmt::format("vector values_size error, id {}", vector.id());
      continue;
    }

    DINGO_LOG(DEBUG) << fmt::format("Replay vector index vector add, vector id {}, log_id {}", vector.id(), log_id);
    vector_index->Add(vector);
  }

  vector_index->SetApplyLogIndex(last_log_id);

  DINGO_LOG(INFO) << fmt::format("Replay vector index {} from log id {} to log id {} success, last_log_id {}",
                                 vector_index->Id(), start_log_id, end_log_id, last_log_id);

  return butil::Status::OK();
}

// Build vector index from scratch
std::shared_ptr<VectorIndex> VectorIndexManager::BuildVectorIndex(store::RegionPtr region) {
  assert(region != nullptr);

  std::string start_key;
  std::string end_key;
  VectorCodec::EncodeVectorId(region->Id(), 0, start_key);
  VectorCodec::EncodeVectorId(region->Id(), UINT64_MAX, end_key);

  IteratorOptions options;
  options.lower_bound = start_key;
  options.upper_bound = end_key;

  auto vector_index = VectorIndex::New(region->Id(), region->InnerRegion().definition().index_parameter());
  if (!vector_index) {
    DINGO_LOG(WARNING) << fmt::format("New vector index failed, id {}", region->Id());
    return nullptr;
  }

  // set snapshot_log_index and apply_log_index
  uint64_t snapshot_log_index = 0;
  uint64_t apply_log_index = 0;
  GetVectorIndexLogIndex(region->Id(), snapshot_log_index, apply_log_index);
  vector_index->SetSnapshotLogIndex(snapshot_log_index);
  vector_index->SetApplyLogIndex(apply_log_index);

  DINGO_LOG(INFO) << fmt::format("Load vector index, id {}, snapshot_log_index {}, apply_log_index {}", region->Id(),
                                 snapshot_log_index, apply_log_index);

  // load vector data to vector index
  auto iter = raw_engine_->NewIterator(Constant::kStoreDataCF, options);
  for (iter->Seek(start_key); iter->Valid(); iter->Next()) {
    pb::common::VectorWithId vector;

    std::string key(iter->Key());
    vector.set_id(VectorCodec::DecodeVectorId(key));

    std::string value(iter->Value());
    if (!vector.mutable_vector()->ParseFromString(value)) {
      DINGO_LOG(WARNING) << fmt::format("vector ParseFromString failed, id {}", vector.id());
      continue;
    }

    if (vector.vector().float_values_size() <= 0) {
      DINGO_LOG(WARNING) << fmt::format("vector values_size error, id {}", vector.id());
      continue;
    }

    vector_index->Add(vector);
  }

  return vector_index;
}

// Rebuild vector index from rocksdb
butil::Status VectorIndexManager::RebuildVectorIndex(store::RegionPtr region) {
  assert(region != nullptr);

  // build a new vector_index from scratch
  auto vector_index = BuildVectorIndex(region);
  if (!vector_index) {
    DINGO_LOG(WARNING) << fmt::format("Build vector index failed, id {}", region->Id());
    return butil::Status(pb::error::Errno::EINTERNAL, "Build vector index failed");
  }

  auto status = ReplayValToVectorIndex(vector_index, vector_index->ApplyLogIndex() + 1, UINT64_MAX);
  if (status.ok()) {
    DINGO_LOG(INFO) << fmt::format("ReplayWal success first-round, id {}, log_id {}", region->Id(),
                                   vector_index->ApplyLogIndex());
    // set vector index to vector index map
    vector_indexs_.Put(region->Id(), vector_index);
    return status;
  }

  // TODO: need to lock vector_index add/delete, to catch up and switch to new vector_index
  status = ReplayValToVectorIndex(vector_index, vector_index->ApplyLogIndex() + 1, UINT64_MAX);
  if (status.ok()) {
    DINGO_LOG(INFO) << fmt::format("ReplayWal success catch up, id {}, log_id {}", region->Id(),
                                   vector_index->ApplyLogIndex());
    // set vector index to vector index map
    vector_indexs_.Put(region->Id(), vector_index);
    return status;
  }

  // save index to disk
  auto ret = SaveVectorIndex(vector_index);
  if (!ret.ok()) {
    DINGO_LOG(WARNING) << fmt::format("Save vector index failed, id {}", region->Id());
    return ret;
  }

  // add vector index to vector index map
  vector_indexs_.Put(region->Id(), vector_index);

  // TODO: unlock vector_index add/delete lock

  return butil::Status::OK();
}

butil::Status VectorIndexManager::SaveVectorIndex(store::RegionPtr region) {
  auto vector_index = vector_indexs_.Get(region->Id());
  if (!vector_index) {
    DINGO_LOG(WARNING) << fmt::format("Save vector index failed, id {}", region->Id());
    return butil::Status(pb::error::Errno::EINTERNAL, "Save vector index failed");
  }

  return SaveVectorIndex(vector_index);
}

butil::Status VectorIndexManager::SaveVectorIndex(std::shared_ptr<VectorIndex> vector_index) {
  // check if vector_index is null
  if (!vector_index) {
    DINGO_LOG(WARNING) << fmt::format("Save vector index failed, vector_index is null");
    return butil::Status(pb::error::Errno::EINTERNAL, "Save vector index failed, vector_index is null");
  }

  // get vector index file path
  std::string vector_index_file_path = fmt::format("{}/{}_{}.idx", Server::GetInstance()->GetIndexPath(),
                                                   vector_index->Id(), vector_index->ApplyLogIndex());

  // get vector index tmp file path
  std::string vector_index_tmp_file_path = fmt::format("{}/{}_{}.tmp", Server::GetInstance()->GetIndexPath(),
                                                       vector_index->Id(), vector_index->ApplyLogIndex());

  DINGO_LOG(INFO) << fmt::format("Save vector index {} to file {}", vector_index->Id(), vector_index_file_path);

  // save vector index to tmp file
  auto ret = vector_index->Save(vector_index_tmp_file_path);
  if (!ret.ok()) {
    DINGO_LOG(ERROR) << fmt::format("Save vector index {} to tmp file failed, {}", vector_index->Id(), ret.error_str());
    std::filesystem::remove(vector_index_tmp_file_path);
    return ret;
  }

  // rename tmp file to vector index file
  std::filesystem::rename(vector_index_tmp_file_path, vector_index_file_path);

  // write vector index file log_id
  std::string vector_index_file_log_id_file_path =
      fmt::format("{}/{}.log_id", Server::GetInstance()->GetIndexPath(), vector_index->Id());

  std::ofstream vector_index_file_log_id_file(vector_index_file_log_id_file_path);
  if (!vector_index_file_log_id_file.is_open()) {
    DINGO_LOG(ERROR) << fmt::format("Open vector index file log_id file {} failed", vector_index_file_log_id_file_path);
    return butil::Status(pb::error::Errno::EINTERNAL, "Open vector index file log_id file failed");
  }

  vector_index_file_log_id_file << vector_index->ApplyLogIndex();

  // delete old vector index file
  // read dir file list
  std::vector<std::filesystem::path> file_path_list;
  std::filesystem::directory_iterator dir_iter(Server::GetInstance()->GetIndexPath());
  for (const auto& iter : dir_iter) {
    if (iter.is_regular_file()) {
      // add file with extension .idx/.tmp and file_name != vector_index_file_path
      if ((iter.path().extension() == ".idx" || iter.path().extension() == ".tmp") &&
          iter.path().string() != vector_index_file_path) {
        auto file_name = iter.path().filename().string();
        auto pos = file_name.find('_');
        if (pos == std::string::npos) {
          DINGO_LOG(WARNING) << fmt::format("Find old vector index file failed, file_name {}", file_name);
          continue;
        }

        uint64_t old_vector_index_id = 0;
        try {
          old_vector_index_id = std::stoull(file_name.substr(0, pos));
        } catch (std::exception& e) {
          DINGO_LOG(WARNING) << fmt::format("Find old vector index file failed, file_name {}", file_name);
          continue;
        }

        if (old_vector_index_id == vector_index->Id()) {
          DINGO_LOG(INFO) << fmt::format("Find old vector index file {} -> {}", iter.path().string(),
                                         vector_index_file_path);
          file_path_list.emplace_back(iter.path());
          continue;
        }
      }
    }
  }

  // delete file in file_path_list
  for (const auto& file_path : file_path_list) {
    DINGO_LOG(INFO) << fmt::format("Delete old vector index file {}", file_path.string());
    std::filesystem::remove(file_path);
  }

  // TODO: trim wal log here

  return butil::Status::OK();
}

void VectorIndexManager::UpdateApplyLogIndex(std::shared_ptr<VectorIndex> vector_index, uint64_t apply_log_index) {
  assert(vector_index != nullptr);

  vector_index->SetApplyLogIndex(apply_log_index);
  meta_writer_->Put(TransformToKv(vector_index));
}

void VectorIndexManager::UpdateApplyLogIndex(uint64_t region_id, uint64_t apply_log_index) {
  auto vector_index = GetVectorIndex(region_id);
  if (vector_index != nullptr) {
    UpdateApplyLogIndex(vector_index, apply_log_index);
  }
}

std::shared_ptr<pb::common::KeyValue> VectorIndexManager::TransformToKv(std::any obj) {
  auto vector_index = std::any_cast<std::shared_ptr<VectorIndex>>(obj);
  auto kv = std::make_shared<pb::common::KeyValue>();
  kv->set_key(GenKey(vector_index->Id()));
  kv->set_value(
      VectorCodec::EncodeVectorIndexLogIndex(vector_index->SnapshotLogIndex(), vector_index->ApplyLogIndex()));

  return kv;
}

void VectorIndexManager::TransformFromKv(const std::vector<pb::common::KeyValue>& kvs) {
  for (const auto& kv : kvs) {
    uint64_t region_id = ParseRegionId(kv.key());
    uint64_t snapshot_log_index = 0;
    uint64_t apply_log_index = 0;

    VectorCodec::DecodeVectorIndexLogIndex(kv.value(), snapshot_log_index, apply_log_index);

    DINGO_LOG(INFO) << fmt::format("TransformFromKv, region_id {}, snapshot_log_index {}, apply_log_index {}",
                                   region_id, snapshot_log_index, apply_log_index);

    auto vector_index = GetVectorIndex(region_id);
    if (vector_index != nullptr) {
      vector_index->SetSnapshotLogIndex(snapshot_log_index);
      vector_index->SetApplyLogIndex(apply_log_index);
    }
  }
}

void VectorIndexManager::GetVectorIndexLogIndex(uint64_t region_id, uint64_t& snapshot_log_index,
                                                uint64_t& apply_log_index) {
  auto kv = meta_reader_->Get(GenKey(region_id));
  if (kv == nullptr) {
    snapshot_log_index = 0;
    apply_log_index = 0;
    return;
  }

  auto ret = VectorCodec::DecodeVectorIndexLogIndex(kv->value(), snapshot_log_index, apply_log_index);
  if (ret < 0) {
    DINGO_LOG(ERROR) << fmt::format("DecodeVectorIndexLogIndex failed, region_id {}", region_id);
  }
}

}  // namespace dingodb