#include "voxblox/integrator/esdf_occ_fiesta_integrator.h"

// marco settings, it's better to avoid them
// Use 24 neighborhood by default (according to FIESTA's paper)
#define USE_24_NEIGHBOR
// No neighborhood directional search by default
// #define DIRECTION_GUIDE

namespace voxblox {

EsdfOccFiestaIntegrator::EsdfOccFiestaIntegrator(
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
void EsdfOccFiestaIntegrator::updateFromOccLayer(bool clear_updated_flag) {
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

void EsdfOccFiestaIntegrator::updateFromOccBlocks(
    const BlockIndexList& occ_blocks) {
  CHECK_EQ(occ_layer_->voxels_per_side(), esdf_layer_->voxels_per_side());
  timing::Timer esdf_timer("update_esdf/fiesta");

  // Go through all blocks in occupancy map (that are recently updated)
  // and copy their values for relevant voxels.
  timing::Timer allocate_timer("update_esdf/fiesta/allocate_vox");
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
        esdf_voxel.behind = occupancy_voxel.behind;
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
        } else {
          // already initialized
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
void EsdfOccFiestaIntegrator::getUpdateRange() {
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
void EsdfOccFiestaIntegrator::setLocalRange() {
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

/* Delete idx from the doubly linked list
 * input:
 * occ_vox: head voxel of the list
 * cur_vox: the voxel need to be deleted
 */
void EsdfOccFiestaIntegrator::deleteFromList(
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
void EsdfOccFiestaIntegrator::insertIntoList(
    EsdfVoxel* occ_vox, EsdfVoxel* cur_vox) {
  // why insert at the head?
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
 * Implementation of FIESTA under voxblox's data structure
 * Reference: Han. L, et al., Fast Incremental Euclidean Distance Fields for
 * Online Motion Planning of Aerial Robots, IROS 2019.
 * A mapping system called Fiesta is proposed to build global ESDF map
 * incrementally from the occupancy grid map. By introducing two
 * independent updating queues for inserting and deleting obstacles separately,
 * and using Indexing Data Structures and Doubly Linked Lists for map
 * maintenance, the algorithm updates as few as possible nodes using a BFS
 * framework. The ESDF mapping has high computational performance and produces
 * near-optimal results.
 * Original Code: https://github.com/HKUST-Aerial-Robotics/FIESTA
 * The original implementation of FIESTA is actually an unsigned distance field
 * mapping. We re-implement FIESTA with Voxblox's data structure and enable the
 * signed distance mapping.
 *
 * About the patch_on and early_break settings:
 * Fastest operation can be achieved by setting patch_on=false and early_break
 * =true. Highest accuracy can be achieved with patch_on=true and early_break=
 * false. Please set them in the config file wisely.
 */
void EsdfOccFiestaIntegrator::updateESDF() {
  timing::Timer init_timer("update_esdf/fiesta/update_init");
  /**
   * update_queue_ is a priority queue, voxels with the smaller
   * absolute distance would be updated first
   * once a voxel is added to update_queue_,
   * its distance would be fixed and it would act as a seed for further updating
   */
  // Algorithm 2: ESDF Updating Initialization
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
    cur_vox->distance = 0.0f;
    cur_vox->coc_idx = cur_vox_idx;
    insertIntoList(cur_vox, cur_vox);
    update_queue_.push(cur_vox_idx, 0.0f);
  }
  // these are originally in the TSDF fixed band and now not
  while (!delete_list_.empty()) {
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
        temp_vox->distance = config_.default_distance_m;

#ifdef USE_24_NEIGHBOR
        // 24 -Neighborhood
        Neighborhood24::IndexMatrix nbr_voxs_idx;
        Neighborhood24::getFromGlobalIndex(temp_vox_idx, &nbr_voxs_idx);
#else
        // 26 -Neighborhood
        Neighborhood<>::IndexMatrix nbr_voxs_idx;
        Neighborhood<>::getFromGlobalIndex(cur_vox->self_idx, &nbr_voxs_idx);
#endif
        // Go through the neighbors and see if we can update this
        // voxel's closest occupied voxel.
        for (unsigned int idx = 0u; idx < nbr_voxs_idx.cols(); ++idx) {
          GlobalIndex nbr_vox_idx = nbr_voxs_idx.col(idx);
          if (voxInRange(nbr_vox_idx)) {
            EsdfVoxel* nbr_vox =
                esdf_layer_->getVoxelPtrByGlobalIndex(nbr_vox_idx);
            CHECK_NOTNULL(nbr_vox);
            GlobalIndex nbr_coc_vox_idx = nbr_vox->coc_idx;
            if (nbr_vox->observed && nbr_coc_vox_idx(0) != UNDEF) {
              OccupancyVoxel* nbr_coc_occ_vox =
                  occ_layer_->getVoxelPtrByGlobalIndex(nbr_coc_vox_idx);
              CHECK_NOTNULL(nbr_coc_occ_vox);
              // check if the closest occupied voxel is still occupied
              if (nbr_coc_occ_vox->occupied) {
                float temp_dist = dist(nbr_coc_vox_idx, temp_vox_idx);
                if (temp_dist < std::abs(temp_vox->distance)) {
                  temp_vox->distance = temp_dist;
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
        temp_vox->distance =
            temp_vox->behind ? -temp_vox->distance : temp_vox->distance;
        update_queue_.push(temp_vox_idx, temp_vox->distance);
        EsdfVoxel* temp_coc_vox =
            esdf_layer_->getVoxelPtrByGlobalIndex(temp_vox->coc_idx);
        CHECK_NOTNULL(temp_coc_vox);
        insertIntoList(temp_coc_vox, temp_vox);
      }
    }
    cur_vox->head_idx = GlobalIndex(UNDEF, UNDEF, UNDEF);
  }
  init_timer.Stop();
  // End of Algorithm 2
  if (config_.verbose) {
    LOG(INFO) << "Update queue's original size: [" << update_queue_.size()
              << "]";
  }

  timing::Timer update_timer("update_esdf/fiesta/update");
  // Algorithm 1 ESDF updating (BFS based on priority queue)
  int updated_count = 0, patch_count = 0;
  while (!update_queue_.empty()) {
    GlobalIndex cur_vox_idx = update_queue_.front();
    update_queue_.pop();
    EsdfVoxel* cur_vox = esdf_layer_->getVoxelPtrByGlobalIndex(cur_vox_idx);
    CHECK_NOTNULL(cur_vox);

    EsdfVoxel* coc_vox =
        esdf_layer_->getVoxelPtrByGlobalIndex(cur_vox->coc_idx);
    CHECK_NOTNULL(coc_vox);

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
    // Algorithm 3 Patch Code
    if (config_.patch_on && cur_vox->newly) {
      // only newly added voxels are required for checking
      // timing::Timer patch_timer("update_esdf/fiesta/patch");
      cur_vox->newly = false;
      // indicate if the patch works
      bool change_flag = false;
      // Go through the neighbors and see if we need to
      // update the current voxel.
      for (unsigned int idx = 0u; idx < nbr_voxs_idx.cols(); ++idx) {
        GlobalIndex nbr_vox_idx = nbr_voxs_idx.col(idx);
        if (voxInRange(nbr_vox_idx)) {
          EsdfVoxel* nbr_vox =
              esdf_layer_->getVoxelPtrByGlobalIndex(nbr_vox_idx);
          CHECK_NOTNULL(nbr_vox);
          if (nbr_vox->observed && nbr_vox->coc_idx(0) != UNDEF) {
            float temp_dist = dist(nbr_vox->coc_idx, cur_vox_idx);
            if (temp_dist < std::abs(cur_vox->distance)) {
              cur_vox->distance = temp_dist;
              cur_vox->coc_idx = nbr_vox->coc_idx;
              change_flag = true;
            }
          }
        }
      }
      if (change_flag) {
        cur_vox->distance =
            cur_vox->behind ? -cur_vox->distance : cur_vox->distance;
        deleteFromList(coc_vox, cur_vox);
        coc_vox = esdf_layer_->getVoxelPtrByGlobalIndex(cur_vox->coc_idx);
        CHECK_NOTNULL(coc_vox);
        update_queue_.push(cur_vox_idx, cur_vox->distance);
        insertIntoList(coc_vox, cur_vox);
        patch_count++;
        continue;
      }
      // patch_timer.Stop();
    }
    // End of Algorithm 3

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
    // faster version, according to the coc direction
    for (unsigned int idx = 0u; idx < used_nbr_idx.size(); ++idx) {
      GlobalIndex nbr_vox_idx = nbr_voxs_idx.col(used_nbr_idx[idx]);
#else
    // slower but more accurate version
    for (unsigned int idx = 0u; idx < nbr_voxs_idx.cols(); ++idx) {
      GlobalIndex nbr_vox_idx = nbr_voxs_idx.col(idx);
#endif
      // check if this index is in the range and not updated yet
      if (voxInRange(nbr_vox_idx)) {
        EsdfVoxel* nbr_vox = esdf_layer_->getVoxelPtrByGlobalIndex(nbr_vox_idx);
        CHECK_NOTNULL(nbr_vox);
        if (nbr_vox->observed && std::abs(nbr_vox->distance) > 0.0) {
          float temp_dist = dist(cur_vox->coc_idx, nbr_vox_idx);
          if (temp_dist < std::abs(nbr_vox->distance)) {
            nbr_vox->distance = nbr_vox->behind ? -temp_dist : temp_dist;
            if (nbr_vox->coc_idx(0) != UNDEF) {
              EsdfVoxel* nbr_coc_vox =
                  esdf_layer_->getVoxelPtrByGlobalIndex(nbr_vox->coc_idx);
              CHECK_NOTNULL(nbr_coc_vox);
              deleteFromList(nbr_coc_vox, nbr_vox);
            }
            nbr_vox->coc_idx = cur_vox->coc_idx;
            insertIntoList(coc_vox, nbr_vox);
            update_queue_.push(nbr_vox_idx, nbr_vox->distance);
          }
        }
      }
    }
  }
  update_timer.Stop();
  // LOG(INFO)<<"Alg 1 done";
  // End of Algorithm 1
  if (config_.verbose) {
    LOG(INFO) << "FIESTA: expanding [" << updated_count << "] nodes, with ["
              << patch_count << "] changes by the patch, up-to-now ["
              << total_updated_count_ << "] nodes";
  }
}

inline float EsdfOccFiestaIntegrator::dist(
    GlobalIndex vox_idx_a, GlobalIndex vox_idx_b) {
  return (vox_idx_b - vox_idx_a).cast<float>().norm() * esdf_voxel_size_;
  // TODO(py): may use square root & * resolution_ at last
  // together to speed up
}

inline int EsdfOccFiestaIntegrator::distSquare(
    GlobalIndex vox_idx_a, GlobalIndex vox_idx_b) {
  int dx = vox_idx_a(0) - vox_idx_b(0);
  int dy = vox_idx_a(1) - vox_idx_b(1);
  int dz = vox_idx_a(2) - vox_idx_b(2);
  return (dx * dx + dy * dy + dz * dz);
}

inline bool EsdfOccFiestaIntegrator::voxInRange(GlobalIndex vox_idx) {
  return (
      vox_idx(0) >= range_min_(0) && vox_idx(0) <= range_max_(0) &&
      vox_idx(1) >= range_min_(1) && vox_idx(1) <= range_max_(1) &&
      vox_idx(2) >= range_min_(2) && vox_idx(2) <= range_max_(2));
}

void EsdfOccFiestaIntegrator::loadInsertList(
    const GlobalIndexList& insert_list) {
  insert_list_ = insert_list;
}

void EsdfOccFiestaIntegrator::loadDeleteList(
    const GlobalIndexList& delete_list) {
  delete_list_ = delete_list;
}

// only for the visualization of ESDF error
void EsdfOccFiestaIntegrator::assignError(
    GlobalIndex vox_idx, float esdf_error) {
  EsdfVoxel* vox = esdf_layer_->getVoxelPtrByGlobalIndex(vox_idx);
  vox->error = esdf_error;
}

}  // namespace voxblox
