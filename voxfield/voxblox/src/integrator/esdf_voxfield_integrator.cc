#include "voxblox/integrator/esdf_voxfield_integrator.h"

// marco settings, it's better to avoid them
#define USE_24_NEIGHBOR
#define DIRECTION_GUIDE

namespace voxblox {

EsdfVoxfieldIntegrator::EsdfVoxfieldIntegrator(
    const Config& config, Layer<TsdfVoxel>* tsdf_layer,
    Layer<EsdfVoxel>* esdf_layer)
    : config_(config), tsdf_layer_(tsdf_layer), esdf_layer_(esdf_layer) {
  CHECK_NOTNULL(tsdf_layer);
  CHECK_NOTNULL(esdf_layer_);

  esdf_voxels_per_side_ = esdf_layer_->voxels_per_side();
  esdf_voxel_size_ = esdf_layer_->voxel_size();

  update_queue_.setNumBuckets(config_.num_buckets, config_.default_distance_m);
}

// Main entrance
void EsdfVoxfieldIntegrator::updateFromTsdfLayer(bool clear_updated_flag) {
  BlockIndexList tsdf_blocks;
  tsdf_layer_->getAllUpdatedBlocks(Update::kEsdf, &tsdf_blocks);

  // LOG(INFO) << "count of the updated tsdf block: [" << tsdf_blocks.size()
  //           << "]";
  if (tsdf_blocks.size() > 0) {
    updateFromTsdfBlocks(tsdf_blocks);

    if (clear_updated_flag) {
      for (const BlockIndex& block_index : tsdf_blocks) {
        if (tsdf_layer_->hasBlock(block_index)) {
          tsdf_layer_->getBlockByIndex(block_index)
              .setUpdated(Update::kEsdf, false);
        }
      }
    }
  }
}

void EsdfVoxfieldIntegrator::updateFromTsdfBlocks(
    const BlockIndexList& tsdf_blocks) {
  CHECK_EQ(tsdf_layer_->voxels_per_side(), esdf_layer_->voxels_per_side());
  timing::Timer esdf_timer("update_esdf/voxfield");

  // Go through all blocks in tsdf map (that are recently updated)
  // and copy their values for relevant voxels.
  timing::Timer allocate_timer("update_esdf/voxfield/allocate_vox");
  VLOG(3) << "[ESDF update]: Propagating " << tsdf_blocks.size()
          << " updated blocks from the Tsdf.";

  for (const BlockIndex& block_index : tsdf_blocks) {
    Block<TsdfVoxel>::Ptr tsdf_block =
        tsdf_layer_->getBlockPtrByIndex(block_index);
    if (!tsdf_block) {
      continue;
    }

    // Allocate the same block in the ESDF layer.
    // Block indices are the same across all layers.
    Block<EsdfVoxel>::Ptr esdf_block =
        esdf_layer_->allocateBlockPtrByIndex(block_index);
    esdf_block->setUpdatedAll();

    const size_t num_voxels_per_block = tsdf_block->num_voxels();
    for (size_t lin_index = 0u; lin_index < num_voxels_per_block; ++lin_index) {
      TsdfVoxel& tsdf_voxel = tsdf_block->getVoxelByLinearIndex(lin_index);
      // If this voxel is unobserved in the original map, skip it.
      if (isObserved(tsdf_voxel.weight)) {
        EsdfVoxel& esdf_voxel = esdf_block->getVoxelByLinearIndex(lin_index);
        esdf_voxel.behind = tsdf_voxel.distance < 0.0 ? true : false;
        // if (esdf_voxel.behind) // behind the surface, directly use tsdf
        //   esdf_voxel.distance = tsdf_voxel.distance;
        // esdf_voxel.distance = tsdf_voxel.distance; // initailize as the
        // truncated distance

        bool current_occupied;

        if (config_.finer_esdf_on) {
          // determine occupancy state with tsdf and gradient
          current_occupied =
              isOccupied(tsdf_voxel.distance, tsdf_voxel.gradient);
        } else {
          // determine occupancy state with only the tsdf
          current_occupied = isOccupied(tsdf_voxel.distance);
        }

        if (esdf_voxel.self_idx(0) == UNDEF) {  // not yet initialized
          esdf_voxel.observed = true;
          esdf_voxel.newly = true;
          VoxelIndex voxel_index =
              esdf_block->computeVoxelIndexFromLinearIndex(lin_index);
          GlobalIndex global_index = getGlobalVoxelIndexFromBlockAndVoxelIndex(
              block_index, voxel_index, esdf_layer_->voxels_per_side());
          esdf_voxel.self_idx = global_index;

          esdf_voxel.raw_distance = esdf_voxel.behind
                                        ? -config_.max_behind_surface_m
                                        : config_.default_distance_m;

          // Newly found occupied --> insert_list
          if (current_occupied)
            insert_list_.push_back(global_index);
        } else {
          esdf_voxel.newly = false;

          // Originally occupied but not occupied now --> delete_list
          if (tsdf_voxel.occupied && !current_occupied)
            delete_list_.push_back(esdf_voxel.self_idx);
          // Originally not occupied but occupied now --> insert_list
          if (!tsdf_voxel.occupied && current_occupied)
            insert_list_.push_back(esdf_voxel.self_idx);
        }

        tsdf_voxel.occupied = current_occupied;

        const bool tsdf_fixed = isFixed(tsdf_voxel.distance);
        if (config_.fixed_band_esdf_on &&
            tsdf_fixed /*|| tsdf_voxel.distance < 0*/) {
          // In fixed band, initialize with the current tsdf.
          esdf_voxel.distance = tsdf_voxel.distance;
          esdf_voxel.fixed = true;
        } else {
          esdf_voxel.fixed = false;  // default
        }
      }
    }
  }

  if (insert_list_.size() + delete_list_.size() > 0) {
    if (config_.verbose) {
      LOG(INFO) << "Insert [" << insert_list_.size() << "] and delete ["
                << delete_list_.size() << "]";
    }
    getUpdateRange();
    setLocalRange();
    allocate_timer.Stop();

    updateESDF();  // main func
  } else {
    return;
  }
  esdf_timer.Stop();
}

// Get the range of the updated tsdf grid (inserted or deleted)
void EsdfVoxfieldIntegrator::getUpdateRange() {
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
void EsdfVoxfieldIntegrator::setLocalRange() {
  // Keep updating
  range_min_ = update_range_min_ - config_.range_boundary_offset;
  range_max_ = update_range_max_ + config_.range_boundary_offset;

  // LOG(INFO) << "range_min: " << range_min_;
  // LOG(INFO) << "range_max: " << range_max_;

  // Allocate memory for the local ESDF map
  BlockIndex block_range_min, block_range_max;
  for (int i = 0; i <= 2; i++) {
    block_range_min(i) = range_min_(i) / esdf_voxels_per_side_;
    block_range_max(i) = range_max_(i) / esdf_voxels_per_side_;
  }

  // LOG(INFO) << "block_range_min: " << block_range_min;
  // LOG(INFO) << "block_range_max: " << block_range_max;

  for (int x = block_range_min(0); x <= block_range_max(0); x++) {
    for (int y = block_range_min(1); y <= block_range_max(1); y++) {
      for (int z = block_range_min(2); z <= block_range_max(2); z++) {
        BlockIndex cur_block_idx = BlockIndex(x, y, z);
        // Allocate Esdf Block if it hasn't been allocated
        Block<EsdfVoxel>::Ptr esdf_block =
            esdf_layer_->allocateBlockPtrByIndex(cur_block_idx);
        esdf_block->setUpdatedAll();

        // Allocate Tsdf Block (TO CHECK !!!)
        // NOTE(py): might be not neccessary, added to deal with the possible
        // case when the nbr_coc_vox is NULL
        if (config_.allocate_tsdf_in_range) {
          Block<TsdfVoxel>::Ptr tsdf_block =
              tsdf_layer_->allocateBlockPtrByIndex(cur_block_idx);
          // tsdf_block->setUpdatedAll();
        }
      }
    }
  }
}

// Set all the voxels in the range to be unfixed
// Deprecated
void EsdfVoxfieldIntegrator::resetFixed() {
  for (int x = range_min_(0); x <= range_max_(0); x++) {
    for (int y = range_min_(1); y <= range_max_(1); y++) {
      for (int z = range_min_(2); z <= range_max_(2); z++) {
        GlobalIndex cur_voxel_idx = GlobalIndex(x, y, z);
        EsdfVoxel* cur_vox =
            esdf_layer_->getVoxelPtrByGlobalIndex(cur_voxel_idx);
        cur_vox->fixed = false;
      }
    }
  }
}

/* Delete idx from the doubly linked list
 * input:
 * occ_vox: head voxel of the list
 * cur_vox: the voxel need to be deleted
 */
void EsdfVoxfieldIntegrator::deleteFromList(
    EsdfVoxel* occ_vox, EsdfVoxel* cur_vox) {
  if (cur_vox->prev_idx(0) != UNDEF) {
    EsdfVoxel* prev_vox =
        esdf_layer_->getVoxelPtrByGlobalIndex(cur_vox->prev_idx);
    // a <-> b <-> c , delete b, a <-> c
    prev_vox->next_idx = cur_vox->next_idx;
  } else {
    // b <-> c, b is already the head
    occ_vox->head_idx = cur_vox->next_idx;
  }
  if (cur_vox->next_idx(0) != UNDEF) {
    EsdfVoxel* next_vox =
        esdf_layer_->getVoxelPtrByGlobalIndex(cur_vox->next_idx);
    next_vox->prev_idx = cur_vox->prev_idx;
  }
  cur_vox->next_idx = GlobalIndex(UNDEF, UNDEF, UNDEF);
  cur_vox->prev_idx = GlobalIndex(UNDEF, UNDEF, UNDEF);
}

/* Insert idx to the doubly linked list at the head
 * input:
 * occ_vox: head voxel of the list
 * cur_vox: the voxel need to be insert
 */
void EsdfVoxfieldIntegrator::insertIntoList(
    EsdfVoxel* occ_vox, EsdfVoxel* cur_vox) {
  if (occ_vox->head_idx(0) == UNDEF) {
    occ_vox->head_idx = cur_vox->self_idx;
  } else {
    EsdfVoxel* head_occ_vox =
        esdf_layer_->getVoxelPtrByGlobalIndex(occ_vox->head_idx);
    // b <-> c to a <-> b <-> c
    head_occ_vox->prev_idx = cur_vox->self_idx;
    cur_vox->next_idx = occ_vox->head_idx;
    occ_vox->head_idx = cur_vox->self_idx;
  }
}

/**
 * Intuitively, Voxfield is a combination of Voxblox and FIESTA
 * On one hand, it uses the efficient incremental updating idea from FIESTA
 * (book keeping with doubly linked list).
 * On the other hand, it uses the underlying data structure of Voxblox and
 * also update ESDF map from TSDF map instead of occupancy grids map like
 * Voxblox so that the distance from the occupied voxel's center to the object
 * surface is also take into account. The discretization error present in FIESTA
 * and EDT can be avoided. Besides, unlike Voxblox, the true Euclidean distance
 * is adopted.
 */
void EsdfVoxfieldIntegrator::updateESDF() {
  timing::Timer init_timer("update_esdf/voxfield/update_init");

  /**
   * update_queue_ is a priority queue, voxels with the smaller absolute
   * distance would be updated first once a voxel is added to update_queue_, its
   * distance would be fixed and it would act as a seed for further updating
   */

  // ESDF Updating Initialization
  while (!insert_list_.empty()) {
    // these inserted list must all in the TSDF fixed band
    GlobalIndex cur_vox_idx = *insert_list_.begin();
    insert_list_.erase(insert_list_.begin());
    // delete previous link & create a new linked-list
    EsdfVoxel* cur_vox = esdf_layer_->getVoxelPtrByGlobalIndex(cur_vox_idx);
    CHECK_NOTNULL(cur_vox);
    if (cur_vox->coc_idx(0) != UNDEF) {
      EsdfVoxel* coc_vox =
          esdf_layer_->getVoxelPtrByGlobalIndex(cur_vox->coc_idx);
      CHECK_NOTNULL(coc_vox);
      deleteFromList(coc_vox, cur_vox);
    }
    cur_vox->raw_distance = 0.0f;
    cur_vox->coc_idx = cur_vox_idx;
    insertIntoList(cur_vox, cur_vox);
    update_queue_.push(cur_vox_idx, 0.0f);
  }

  // ESDF "increasing" update
  while (!delete_list_.empty()) {
    // these are originally occupied but now not occupied any more
    GlobalIndex cur_vox_idx = *delete_list_.begin();
    delete_list_.erase(delete_list_.begin());
    EsdfVoxel* cur_vox = esdf_layer_->getVoxelPtrByGlobalIndex(cur_vox_idx);
    CHECK_NOTNULL(cur_vox);
    GlobalIndex next_vox_idx = GlobalIndex(UNDEF, UNDEF, UNDEF);
    // for each voxel in current voxel's doubly linked list
    // (regard current voxel as the closest occupied voxel)
    for (GlobalIndex temp_vox_idx = cur_vox_idx; temp_vox_idx(0) != UNDEF;
         temp_vox_idx = next_vox_idx) {
      EsdfVoxel* temp_vox = esdf_layer_->getVoxelPtrByGlobalIndex(temp_vox_idx);
      CHECK_NOTNULL(temp_vox);

      // deleteFromList(cur_vox, temp_vox);
      temp_vox->coc_idx = GlobalIndex(UNDEF, UNDEF, UNDEF);

      if (voxInRange(temp_vox_idx)) {
        temp_vox->raw_distance = config_.default_distance_m;

#ifdef USE_24_NEIGHBOR
        // 24 -Neighborhood
        Neighborhood24::IndexMatrix nbr_voxs_idx;
        Neighborhood24::getFromGlobalIndex(temp_vox_idx, &nbr_voxs_idx);
#else
        // 26 -Neighborhood
        Neighborhood<>::IndexMatrix nbr_voxs_idx;
        Neighborhood<>::getFromGlobalIndex(cur_vox->self_idx, &nbr_voxs_idx);
#endif
        // Go through the neighbors and see if we can update
        // this voxel's closest occupied voxel.
        for (unsigned int idx = 0u; idx < nbr_voxs_idx.cols(); ++idx) {
          GlobalIndex nbr_vox_idx = nbr_voxs_idx.col(idx);
          if (voxInRange(nbr_vox_idx)) {
            EsdfVoxel* nbr_vox =
                esdf_layer_->getVoxelPtrByGlobalIndex(nbr_vox_idx);
            CHECK_NOTNULL(nbr_vox);
            GlobalIndex nbr_coc_vox_idx = nbr_vox->coc_idx;
            if (nbr_vox->observed && nbr_coc_vox_idx(0) != UNDEF) {
              TsdfVoxel* nbr_coc_tsdf_vox =
                  tsdf_layer_->getVoxelPtrByGlobalIndex(nbr_coc_vox_idx);
              CHECK_NOTNULL(nbr_coc_tsdf_vox);
              // check if the closest occupied voxel is still occupied
              if (nbr_coc_tsdf_vox->occupied) {
                float temp_dist = dist(nbr_coc_vox_idx, temp_vox_idx);
                if (temp_dist < std::abs(temp_vox->raw_distance)) {
                  temp_vox->raw_distance = temp_dist;
                  temp_vox->coc_idx = nbr_coc_vox_idx;
                }
                if (config_.early_break) {
                  temp_vox->newly = true;
                  break;
                }
              }
            }
          }
        }
      }
      next_vox_idx = temp_vox->prev_idx;
      temp_vox->next_idx = GlobalIndex(UNDEF, UNDEF, UNDEF);
      temp_vox->prev_idx = GlobalIndex(UNDEF, UNDEF, UNDEF);

      if (temp_vox->coc_idx(0) != UNDEF) {
        temp_vox->raw_distance =
            temp_vox->behind ? -temp_vox->raw_distance : temp_vox->raw_distance;
        update_queue_.push(temp_vox_idx, temp_vox->raw_distance);
        EsdfVoxel* temp_coc_vox =
            esdf_layer_->getVoxelPtrByGlobalIndex(temp_vox->coc_idx);
        CHECK_NOTNULL(temp_coc_vox);
        insertIntoList(temp_coc_vox, temp_vox);
      }
    }
    cur_vox->head_idx = GlobalIndex(UNDEF, UNDEF, UNDEF);
  }
  init_timer.Stop();

  timing::Timer update_timer("update_esdf/voxfield/update");
  // ESDF "decreasing" updating (BFS based on priority queue)
  int updated_count = 0, patch_count = 0;
  while (!update_queue_.empty()) {
    GlobalIndex cur_vox_idx = update_queue_.front();
    update_queue_.pop();
    EsdfVoxel* cur_vox = esdf_layer_->getVoxelPtrByGlobalIndex(cur_vox_idx);
    CHECK_NOTNULL(cur_vox);

    EsdfVoxel* coc_vox =
        esdf_layer_->getVoxelPtrByGlobalIndex(cur_vox->coc_idx);
    CHECK_NOTNULL(coc_vox);

    // cur_vox->distance = 100.0; // for updating status check
    // if the voxel lies in the fixed band, then default value is its tsdf
    // if out, apply the finer esdf correction, add the sub-voxel part
    // of the esdf from the voxel center to the actual surface
    if (config_.finer_esdf_on) {
      // out of the fixed band
      if (!cur_vox->fixed) {
        TsdfVoxel* coc_tsdf_vox =
            tsdf_layer_->getVoxelPtrByGlobalIndex(cur_vox->coc_idx);
        CHECK_NOTNULL(coc_tsdf_vox);
        if (coc_tsdf_vox->gradient.norm() > kFloatEpsilon &&
            coc_tsdf_vox->occupied) {
          // gurantee the gradient here is a unit vector
          coc_tsdf_vox->gradient.normalize();
          Point cur_vox_center = cur_vox_idx.cast<float>() * esdf_voxel_size_;
          Point coc_vox_center =
              cur_vox->coc_idx.cast<float>() * esdf_voxel_size_;
          // gradient is pointing toward the sensor, sign should be positive
          Point coc_vox_surface = coc_vox_center + config_.gradient_sign *
                                                       coc_tsdf_vox->gradient *
                                                       coc_tsdf_vox->distance;
          cur_vox->distance = (coc_vox_surface - cur_vox_center).norm();
          cur_vox->distance =
              cur_vox->behind ? -cur_vox->distance : cur_vox->distance;
        } else {
          // if the gradient is not available, we would directly use the
          // voxel center (distance would be equal to raw_distance)
          cur_vox->distance = cur_vox->raw_distance;
        }
      }
      // else, fixed, then directly use the tsdf
    } else {
      // use the original voxel center to center distance
      if (!cur_vox->fixed || !config_.fixed_band_esdf_on)
        cur_vox->distance = cur_vox->raw_distance;
    }
    updated_count++;
    total_updated_count_++;

    // Get the global indices of neighbors.
#ifdef USE_24_NEIGHBOR
    Neighborhood24::IndexMatrix nbr_voxs_idx;
    Neighborhood24::getFromGlobalIndex(cur_vox_idx, &nbr_voxs_idx);
#else
    Neighborhood<>::IndexMatrix nbr_voxs_idx;
    Neighborhood<>::getFromGlobalIndex(cur_vox->self_idx, &nbr_voxs_idx);
#endif
    // Patch Code (optional)
    if (config_.patch_on && cur_vox->newly) {
      // only newly added voxels are required for checking
      // timing::Timer patch_timer("upate_esdf/patch(alg3)");
      cur_vox->newly = false;
      // indicate if the patch works
      bool change_flag = false;
      // Go through the neighbors and see if we need
      // to update the current voxel.
      for (unsigned int idx = 0u; idx < nbr_voxs_idx.cols(); ++idx) {
        GlobalIndex nbr_vox_idx = nbr_voxs_idx.col(idx);
        if (voxInRange(nbr_vox_idx)) {
          EsdfVoxel* nbr_vox =
              esdf_layer_->getVoxelPtrByGlobalIndex(nbr_vox_idx);
          CHECK_NOTNULL(nbr_vox);
          if (nbr_vox->observed && nbr_vox->coc_idx(0) != UNDEF) {
            float temp_dist = dist(nbr_vox->coc_idx, cur_vox_idx);
            if (temp_dist < std::abs(cur_vox->raw_distance)) {
              cur_vox->raw_distance = temp_dist;
              cur_vox->coc_idx = nbr_vox->coc_idx;
              change_flag = true;
            }
          }
        }
      }
      if (change_flag) {
        cur_vox->raw_distance =
            cur_vox->behind ? -cur_vox->raw_distance : cur_vox->raw_distance;
        deleteFromList(coc_vox, cur_vox);
        coc_vox = esdf_layer_->getVoxelPtrByGlobalIndex(cur_vox->coc_idx);
        CHECK_NOTNULL(coc_vox);
        update_queue_.push(cur_vox_idx, cur_vox->raw_distance);
        insertIntoList(coc_vox, cur_vox);
        patch_count++;
        continue;
      }
      // patch_timer.Stop();
    }

    // Update
#ifdef DIRECTION_GUIDE
    std::vector<int> used_nbr_idx;
#ifdef USE_24_NEIGHBOR
    Neighborhood24::getFromGlobalIndexAndObstacle(
        cur_vox->self_idx,
        cur_vox->coc_idx,  // NOLINT
        used_nbr_idx);
#else
    Neighborhood<>::getFromGlobalIndexAndObstacle(
        cur_vox->self_idx,
        cur_vox->coc_idx,  // NOLINT
        used_nbr_idx);
#endif
    // faster version, expand only a subset of the neighborhood
    // according to the relative orientation of the closest
    // occupied voxel
    for (unsigned int idx = 0u; idx < used_nbr_idx.size(); ++idx) {
      GlobalIndex nbr_vox_idx = nbr_voxs_idx.col(used_nbr_idx[idx]);
#else
    // slower version, expand all the neighbors
    for (unsigned int idx = 0u; idx < nbr_voxs_idx.cols(); ++idx) {
      GlobalIndex nbr_vox_idx = nbr_voxs_idx.col(idx);
#endif
      // check if this index is in the range and not updated yet
      if (voxInRange(nbr_vox_idx)) {
        EsdfVoxel* nbr_vox = esdf_layer_->getVoxelPtrByGlobalIndex(nbr_vox_idx);
        CHECK_NOTNULL(nbr_vox);
        if (nbr_vox->observed && std::abs(nbr_vox->raw_distance) > 0.0) {
          float temp_dist = dist(cur_vox->coc_idx, nbr_vox_idx);
          if (temp_dist < std::abs(nbr_vox->raw_distance)) {
            nbr_vox->raw_distance = nbr_vox->behind ? -temp_dist : temp_dist;
            if (nbr_vox->coc_idx(0) != UNDEF) {
              EsdfVoxel* nbr_coc_vox =
                  esdf_layer_->getVoxelPtrByGlobalIndex(nbr_vox->coc_idx);
              CHECK_NOTNULL(nbr_coc_vox);
              deleteFromList(nbr_coc_vox, nbr_vox);
            }
            nbr_vox->coc_idx = cur_vox->coc_idx;
            insertIntoList(coc_vox, nbr_vox);
            update_queue_.push(nbr_vox_idx, nbr_vox->raw_distance);
          }
        }
      }
    }
  }
  update_timer.Stop();
  if (config_.verbose) {
    LOG(INFO) << "Voxfield: expanding [" << updated_count << "] nodes, with ["
              << patch_count << "] changes by the patch, up-to-now ["
              << total_updated_count_ << "] nodes";
  }
}

inline float EsdfVoxfieldIntegrator::dist(
    GlobalIndex vox_idx_a, GlobalIndex vox_idx_b) {
  return (vox_idx_b - vox_idx_a).cast<float>().norm() * esdf_voxel_size_;
  // TODO(py): may use square root & * resolution_ at last
  // together to speed up
}

inline int EsdfVoxfieldIntegrator::distSquare(
    GlobalIndex vox_idx_a, GlobalIndex vox_idx_b) {
  int dx = vox_idx_a(0) - vox_idx_b(0);
  int dy = vox_idx_a(1) - vox_idx_b(1);
  int dz = vox_idx_a(2) - vox_idx_b(2);
  return (dx * dx + dy * dy + dz * dz);
}

inline bool EsdfVoxfieldIntegrator::voxInRange(GlobalIndex vox_idx) {
  return (
      vox_idx(0) >= range_min_(0) && vox_idx(0) <= range_max_(0) &&
      vox_idx(1) >= range_min_(1) && vox_idx(1) <= range_max_(1) &&
      vox_idx(2) >= range_min_(2) && vox_idx(2) <= range_max_(2));
}

// only for the visualization of ESDF error
void EsdfVoxfieldIntegrator::assignError(
    GlobalIndex vox_idx, float esdf_error) {
  EsdfVoxel* vox = esdf_layer_->getVoxelPtrByGlobalIndex(vox_idx);
  vox->error = esdf_error;
}

}  // namespace voxblox
