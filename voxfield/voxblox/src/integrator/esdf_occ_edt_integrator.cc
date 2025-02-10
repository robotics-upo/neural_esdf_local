#include "voxblox/integrator/esdf_occ_edt_integrator.h"
// No neighborhood directional search by default
// #define DIRECTION_GUIDE

namespace voxblox {

EsdfOccEdtIntegrator::EsdfOccEdtIntegrator(
    const Config& config, Layer<OccupancyVoxel>* occ_layer,
    Layer<EsdfVoxel>* esdf_layer)
    : config_(config), occ_layer_(occ_layer), esdf_layer_(esdf_layer) {
  CHECK_NOTNULL(occ_layer_);
  CHECK_NOTNULL(esdf_layer_);

  esdf_voxels_per_side_ = esdf_layer_->voxels_per_side();
  esdf_voxel_size_ = esdf_layer_->voxel_size();

  update_queue_.setNumBuckets(config_.num_buckets, config_.default_distance_m);
}

// Main entrance
void EsdfOccEdtIntegrator::updateFromOccLayer(bool clear_updated_flag) {
  BlockIndexList occ_blocks;
  occ_layer_->getAllUpdatedBlocks(Update::kEsdf, &occ_blocks);

  // LOG(INFO) << "count of the updated occupancy block: [" << occ_blocks.size()
  //           << "]";

  updateFromOccBlocks(occ_blocks);

  if (clear_updated_flag) {
    for (const BlockIndex& block_index : occ_blocks) {
      if (occ_layer_->hasBlock(block_index)) {
        occ_layer_->getBlockByIndex(block_index)
            .setUpdated(Update::kEsdf, false);
      }
    }
  }
}

void EsdfOccEdtIntegrator::updateFromOccBlocks(
    const BlockIndexList& occ_blocks) {
  CHECK_EQ(occ_layer_->voxels_per_side(), esdf_layer_->voxels_per_side());
  timing::Timer esdf_timer("upate_esdf/edt");

  // Go through all blocks in occupancy map (that are recently updated)
  // and copy their values for relevant voxels.
  timing::Timer allocate_timer("upate_esdf/edt/allocate_vox");
  VLOG(3) << "[ESDF update]: Propagating " << occ_blocks.size()
          << " updated blocks from the Occupancy.";

  for (const BlockIndex& block_index : occ_blocks) {
    Block<OccupancyVoxel>::ConstPtr occ_block =
        occ_layer_->getBlockPtrByIndex(block_index);
    if (!occ_block) {
      continue;
    }

    // Allocate the same block in the ESDF layer.
    // Block indices are the same across all layers.
    Block<EsdfVoxel>::Ptr esdf_block =
        esdf_layer_->allocateBlockPtrByIndex(block_index);
    esdf_block->setUpdatedAll();

    const size_t num_voxels_per_block = occ_block->num_voxels();
    for (size_t lin_index = 0u; lin_index < num_voxels_per_block; ++lin_index) {
      const OccupancyVoxel& occupancy_voxel =
          occ_block->getVoxelByLinearIndex(lin_index);
      // If this voxel is unobserved in the original map, skip it.
      // Initialization
      if (occupancy_voxel.observed) {
        EsdfVoxel& esdf_voxel = esdf_block->getVoxelByLinearIndex(lin_index);
        esdf_voxel.behind = occupancy_voxel.behind;  // add signed
        if (esdf_voxel.self_idx(0) == UNDEF) {
          esdf_voxel.observed = true;
          esdf_voxel.newly = true;
          VoxelIndex voxel_index =
              esdf_block->computeVoxelIndexFromLinearIndex(lin_index);
          GlobalIndex global_index = getGlobalVoxelIndexFromBlockAndVoxelIndex(
              block_index, voxel_index, esdf_layer_->voxels_per_side());
          esdf_voxel.self_idx = global_index;
          esdf_voxel.distance = esdf_voxel.behind
                                    ? -config_.max_behind_surface_m
                                    : config_.default_distance_m;
          esdf_voxel.in_queue = false;
          esdf_voxel.raise = -1.0;
        } else {
          esdf_voxel.newly = false;
        }
      }
    }
  }

  getUpdateRange();
  setLocalRange();

  allocate_timer.Stop();
  updateESDF();

  esdf_timer.Stop();
}

// Get the range of the changed occupancy grid (inserted or deleted)
void EsdfOccEdtIntegrator::getUpdateRange() {
  // initialization
  update_range_min_ << UNDEF, UNDEF, UNDEF;
  update_range_max_ << -UNDEF, -UNDEF, -UNDEF;

  for (auto it = insert_list_.begin(); it != insert_list_.end(); it++) {
    GlobalIndex cur_vox_idx = *it;
    for (int j = 0; j <= 2; j++) {
      update_range_min_(j) = std::min(cur_vox_idx(j), update_range_min_(j));
      update_range_max_(j) = std::max(cur_vox_idx(j), update_range_max_(j));
    }
  }

  for (auto it = delete_list_.begin(); it != delete_list_.end(); it++) {
    GlobalIndex cur_vox_idx = *it;
    for (int j = 0; j <= 2; j++) {
      update_range_min_(j) = std::min(cur_vox_idx(j), update_range_min_(j));
      update_range_max_(j) = std::max(cur_vox_idx(j), update_range_max_(j));
    }
  }
}

// Expand the updated range with a given margin and then allocate memory
void EsdfOccEdtIntegrator::setLocalRange() {
  range_min_ = update_range_min_ - config_.range_boundary_offset;
  range_max_ = update_range_max_ + config_.range_boundary_offset;

  if (config_.verbose) {
    LOG(INFO) << "range_min: " << range_min_;
    LOG(INFO) << "range_max: " << range_max_;
  }

  // Allocate memory for the local ESDF map
  BlockIndex block_range_min, block_range_max;
  for (int i = 0; i <= 2; i++) {
    block_range_min(i) = range_min_(i) / esdf_voxels_per_side_;
    block_range_max(i) = range_max_(i) / esdf_voxels_per_side_;
  }

  for (int x = block_range_min(0); x <= block_range_max(0); x++) {
    for (int y = block_range_min(1); y <= block_range_max(1); y++) {
      for (int z = block_range_min(2); z <= block_range_max(2); z++) {
        BlockIndex cur_block_idx = BlockIndex(x, y, z);
        Block<EsdfVoxel>::Ptr esdf_block =
            esdf_layer_->allocateBlockPtrByIndex(cur_block_idx);
        esdf_block->setUpdatedAll();
      }
    }
  }
}

/**
 * Implementation of VDB-EDT under Voxblox's data structure
 * Reference: Zhu. D, et al., VDB-EDT: An Efficient Euclidean
 * Distance Transform Algorithm Based on VDB Data Structure
 * Original Code: https://github.com/zhudelong/VDB-EDT
 * VDB-EDT's contributions are in two folds. The first part is using
 * the VDB-tree data structure to store the map. The second part is the
 * efficient update scheduling strategy during ESDF mapping.
 * To have a fair comparison on ESDF mapping performance, We implement the
 * second part (the EDT part) with the same voxel hashing-based data
 * structure as Voxblox.
 * Similar to FIESTA, EDT integrate ESDF map from the occupancy map,
 * TSDF map is not involved.
 */
void EsdfOccEdtIntegrator::updateESDF() {
  timing::Timer init_timer("upate_esdf/edt/update_init");
  // update_queue_ is a priority queue, voxels with the
  // smaller absolute distance would be updated first

  // Set Obstacle
  while (!insert_list_.empty()) {
    GlobalIndex cur_vox_idx = *insert_list_.begin();
    insert_list_.erase(insert_list_.begin());
    EsdfVoxel* cur_vox = esdf_layer_->getVoxelPtrByGlobalIndex(cur_vox_idx);
    CHECK_NOTNULL(cur_vox);
    cur_vox->coc_idx = cur_vox_idx;
    cur_vox->distance = 0.0f;
    // not raised
    cur_vox->raise = -1.0f;
    cur_vox->in_queue = true;
    update_queue_.push(cur_vox_idx, 0.0f);
  }

  // Remove Obstacle
  while (!delete_list_.empty()) {
    GlobalIndex cur_vox_idx = *delete_list_.begin();
    delete_list_.erase(delete_list_.begin());
    EsdfVoxel* cur_vox = esdf_layer_->getVoxelPtrByGlobalIndex(cur_vox_idx);
    CHECK_NOTNULL(cur_vox);
    cur_vox->coc_idx = GlobalIndex(UNDEF, UNDEF, UNDEF);
    cur_vox->distance = config_.default_distance_m;
    // waiting for raise
    cur_vox->raise = 0.0f;
    cur_vox->in_queue = true;
    update_queue_.push(cur_vox_idx, 0.0f);
  }
  init_timer.Stop();

  sum_occ_changed_ += update_queue_.size();

  timing::Timer update_timer("upate_esdf/edt/update");
  // Distance transform
  while (!update_queue_.empty()) {
    GlobalIndex cur_vox_idx = update_queue_.front();
    update_queue_.pop();
    EsdfVoxel* cur_vox = esdf_layer_->getVoxelPtrByGlobalIndex(cur_vox_idx);
    CHECK_NOTNULL(cur_vox);

    if (!cur_vox->in_queue)
      continue;
    if (cur_vox->raise >= 0) {
      // timing::Timer raise_timer("upate_esdf/edt/raise");
      processRaise(cur_vox);
      // raise_timer.Stop();
      sum_raise_++;
    } else {
      // timing::Timer lower_timer("upate_esdf/edt/lower");
      processLower(cur_vox);
      // lower_timer.Stop();
      sum_lower_++;
    }
  }
  update_timer.Stop();

  if (config_.verbose) {
    LOG(INFO) << "changed: [" << sum_occ_changed_ << "] raised: [" << sum_raise_
              << "] lowered: [" << sum_lower_ << "]";
  }
}

void EsdfOccEdtIntegrator::processRaise(EsdfVoxel* cur_vox) {
  // Get the global indices of neighbors.
  Neighborhood<>::IndexMatrix nbr_voxs_idx;
  Neighborhood<>::getFromGlobalIndex(cur_vox->self_idx, &nbr_voxs_idx);

  // Go through the neighbors and see if we can update any of them.
  for (unsigned int idx = 0u; idx < nbr_voxs_idx.cols(); ++idx) {
    GlobalIndex nbr_vox_idx = nbr_voxs_idx.col(idx);
    if (!voxInRange(nbr_vox_idx))
      continue;
    EsdfVoxel* nbr_vox = esdf_layer_->getVoxelPtrByGlobalIndex(nbr_vox_idx);
    CHECK_NOTNULL(nbr_vox);
    GlobalIndex nbr_coc_vox_idx = nbr_vox->coc_idx;
    if (!nbr_vox->observed || nbr_coc_vox_idx(0) == UNDEF)
      continue;
    OccupancyVoxel* nbr_coc_occ_vox =
        occ_layer_->getVoxelPtrByGlobalIndex(nbr_coc_vox_idx);
    CHECK_NOTNULL(nbr_coc_occ_vox);
    // check if the closest occupied voxel is still occupied
    if (!nbr_coc_occ_vox->occupied) {
      // unoccupied, need to raise
      update_queue_.push(nbr_vox_idx, std::abs(nbr_vox->distance));
      nbr_vox->raise = std::abs(nbr_vox->distance);
      nbr_vox->in_queue = true;
      nbr_vox->coc_idx = GlobalIndex(UNDEF, UNDEF, UNDEF);
      nbr_vox->distance = config_.default_distance_m;
    } else if (!nbr_vox->in_queue) {
      nbr_vox->in_queue = true;
      update_queue_.push(nbr_vox_idx, nbr_vox->distance);
    }
  }
  // not Raise
  cur_vox->raise = -1.0f;
  cur_vox->in_queue = false;
}

void EsdfOccEdtIntegrator::processLower(EsdfVoxel* cur_vox) {
  // Get the global indices of neighbors. (26 neighborhood is used)
  Neighborhood<>::IndexMatrix nbr_voxs_idx;
  Neighborhood<>::getFromGlobalIndex(cur_vox->self_idx, &nbr_voxs_idx);
#ifdef DIRECTION_GUIDE
  std::vector<int> used_nbr_idx;
  Neighborhood<>::getFromGlobalIndexAndObstacle(
      cur_vox->self_idx,
      cur_vox->coc_idx,  // NOLINT
      used_nbr_idx);
// #else
//   Neighborhood<>::IndexMatrix nbr_voxs_idx;
//   Neighborhood<>::getFromGlobalIndex(cur_vox->self_idx, &nbr_voxs_idx);
#endif
#ifdef DIRECTION_GUIDE
  // better to have another shorter vector according to cur_coc
  for (unsigned int idx = 0u; idx < used_nbr_idx.size(); ++idx) {
    GlobalIndex nbr_vox_idx = nbr_voxs_idx.col(used_nbr_idx[idx]);
#else
  // Go through the neighbors and see if we can update any of them.
  for (unsigned int idx = 0u; idx < nbr_voxs_idx.cols(); ++idx) {
    GlobalIndex nbr_vox_idx = nbr_voxs_idx.col(idx);
#endif
    if (!voxInRange(nbr_vox_idx))
      continue;
    EsdfVoxel* nbr_vox = esdf_layer_->getVoxelPtrByGlobalIndex(nbr_vox_idx);
    CHECK_NOTNULL(nbr_vox);
    if (!nbr_vox->observed)
      continue;
    float temp_dist = dist(cur_vox->coc_idx, nbr_vox_idx);
    temp_dist = std::min(temp_dist, config_.default_distance_m);

    if (nbr_vox->raise >= temp_dist) {
      nbr_vox->coc_idx = cur_vox->coc_idx;
      nbr_vox->distance =
          nbr_vox->behind ? -std::min(temp_dist, config_.max_behind_surface_m)
                          : temp_dist;
      // not raise
      nbr_vox->raise = -1.0f;
      nbr_vox->in_queue = true;
      update_queue_.push(nbr_vox_idx, temp_dist);
    } else if (nbr_vox->raise < 0.0f) {
      if (temp_dist < std::abs(nbr_vox->distance)) {
        nbr_vox->coc_idx = cur_vox->coc_idx;
        nbr_vox->distance =
            nbr_vox->behind ? -std::min(temp_dist, config_.max_behind_surface_m)
                            : temp_dist;
        // not raise
        nbr_vox->raise = -1.0f;
        if (temp_dist < config_.default_distance_m) {
          nbr_vox->in_queue = true;
          update_queue_.push(nbr_vox_idx, temp_dist);
        }
      }
    }
  }
  // cur_vox->raise = -1.0f;
  cur_vox->in_queue = false;
}

void EsdfOccEdtIntegrator::loadInsertList(const GlobalIndexList& insert_list) {
  insert_list_ = insert_list;
}

void EsdfOccEdtIntegrator::loadDeleteList(const GlobalIndexList& delete_list) {
  delete_list_ = delete_list;
}

inline int EsdfOccEdtIntegrator::distSquare(
    GlobalIndex vox_idx_a, GlobalIndex vox_idx_b) {
  int dx = vox_idx_a(0) - vox_idx_b(0);
  int dy = vox_idx_a(1) - vox_idx_b(1);
  int dz = vox_idx_a(2) - vox_idx_b(2);
  return (dx * dx + dy * dy + dz * dz);
}

inline float EsdfOccEdtIntegrator::dist(
    GlobalIndex vox_idx_a, GlobalIndex vox_idx_b) {
  return (vox_idx_b - vox_idx_a).cast<float>().norm() * esdf_voxel_size_;
  // TODO(py): may use square root & * resolution_ at last
  // together to speed up
}

inline bool EsdfOccEdtIntegrator::voxInRange(GlobalIndex vox_idx) {
  return (
      vox_idx(0) >= range_min_(0) && vox_idx(0) <= range_max_(0) &&
      vox_idx(1) >= range_min_(1) && vox_idx(1) <= range_max_(1) &&
      vox_idx(2) >= range_min_(2) && vox_idx(2) <= range_max_(2));
}

// only for the visualization of ESDF error
void EsdfOccEdtIntegrator::assignError(GlobalIndex vox_idx, float esdf_error) {
  EsdfVoxel* vox = esdf_layer_->getVoxelPtrByGlobalIndex(vox_idx);
  vox->error = esdf_error;
}

}  // namespace voxblox
