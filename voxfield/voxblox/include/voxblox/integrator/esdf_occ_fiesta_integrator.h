#ifndef VOXBLOX_INTEGRATOR_ESDF_OCC_FIESTA_INTEGRATOR_H_
#define VOXBLOX_INTEGRATOR_ESDF_OCC_FIESTA_INTEGRATOR_H_

#include <algorithm>
#include <queue>
#include <utility>
#include <vector>

#include <Eigen/Core>
#include <glog/logging.h>

#include "voxblox/core/layer.h"
#include "voxblox/core/voxel.h"
#include "voxblox/integrator/integrator_utils.h"
#include "voxblox/utils/bucket_queue.h"
#include "voxblox/utils/neighbor_tools.h"
#include "voxblox/utils/neighbor_tools_ex.h"
#include "voxblox/utils/timing.h"

namespace voxblox {

/**
 * Builds an ESDF layer out of a given occupancy layer efficiently.
 * For a description of this algorithm, please check
 * the paper of FIESTA (https://arxiv.org/abs/1903.02144)
 */
class EsdfOccFiestaIntegrator {
 public:
  EIGEN_MAKE_ALIGNED_OPERATOR_NEW

  struct Config {
    EIGEN_MAKE_ALIGNED_OPERATOR_NEW

    bool verbose = false;

    /**
     * Maximum distance to calculate the actual distance to.
     * Any values above this will be set to default_distance_m.
     */
    FloatingPoint max_distance_m = 10.0;

    // Default distance set for unknown values and values > max_distance_m.
    FloatingPoint default_distance_m = 10.0;

    // Trunction distance behind surface, unit: m
    FloatingPoint max_behind_surface_m = 1.0f;

    // Default voxel unit distance square (deprecated)
    // int default_dist_square = 50 * 50;

    // Number of buckets for the bucketed priority queue.
    int num_buckets = 20;

    // Number of the neighbor voxels (select from 6, 18, 24 and 26)
    int num_neighbor = 24;

    // Turn on the patch code (Algorithm 3 in FIESTA) or not
    bool patch_on = true;
    // Early break the increasing update or not
    bool early_break = true;

    // Local map boundary size (unit: voxel)
    GlobalIndex range_boundary_offset = GlobalIndex(10, 10, 5);
  };

  EsdfOccFiestaIntegrator(
      const Config& config, Layer<OccupancyVoxel>* occ_layer,
      Layer<EsdfVoxel>* esdf_layer);

  void updateFromOccLayer(bool clear_updated_flag);

  void updateFromOccBlocks(const BlockIndexList& occ_blocks);

  /**
   * In FIESTA we set a range for the ESDF update.
   * This range is expanded from the bounding box of the TSDF voxels
   * get updated during the last time interval
   */
  // Get the range of the updated tsdf grid (inserted or deleted)
  void getUpdateRange();
  // Expand the updated range with a given margin and then allocate memory
  void setLocalRange();
  // Judge if a voxel is in the update range, if not, leave it still
  inline bool voxInRange(GlobalIndex vox_idx);

  // main ESDF updating function
  void updateESDF();

  // basic operations of a doubly linked list
  // delete operation
  void deleteFromList(EsdfVoxel* occ_vox, EsdfVoxel* cur_vox);
  // insert operation
  void insertIntoList(EsdfVoxel* occ_vox, EsdfVoxel* cur_vox);

  // calculate distance between two voxel centers
  inline float dist(GlobalIndex vox_idx_a, GlobalIndex vox_idx_b);
  inline int distSquare(GlobalIndex vox_idx_a, GlobalIndex vox_idx_b);

  // Insert list contains the occupied voxels that are previously free
  void loadInsertList(const GlobalIndexList& insert_list);
  // Delete list contains the free voxels that are previously occupied
  void loadDeleteList(const GlobalIndexList& delete_list);

  void assignError(GlobalIndex vox_idx, float esdf_error);

  inline void clear() {
    GlobalIndexList().swap(insert_list_);
    GlobalIndexList().swap(delete_list_);
    update_queue_.clear();
    updated_voxel_.clear();
  }

  /// Update some specific settings.
  float getEsdfMaxDistance() const {
    return config_.max_distance_m;
  }
  void setEsdfMaxDistance(float max_distance) {
    config_.max_distance_m = max_distance;
    if (config_.default_distance_m < max_distance) {
      config_.default_distance_m = max_distance;
    }
  }

 protected:
  Config config_;

  // Involved map layers (From Occupancy map to ESDF)
  Layer<OccupancyVoxel>* occ_layer_;
  Layer<EsdfVoxel>* esdf_layer_;

  // Data structure used for FIESTA
  GlobalIndexList insert_list_;
  GlobalIndexList delete_list_;
  BucketQueue<GlobalIndex> update_queue_;
  LongIndexSet updated_voxel_;
  size_t esdf_voxels_per_side_;
  FloatingPoint esdf_voxel_size_;

  // Update (inseted and deleted occupied voxels) range, unit: voxel
  GlobalIndex update_range_min_;
  GlobalIndex update_range_max_;

  // Local map range (update range + boundary size), unit: voxel
  GlobalIndex range_min_;
  GlobalIndex range_max_;

  // for recording and logging
  int total_updated_count_ = 0;
};

}  // namespace voxblox

#endif  // VOXBLOX_INTEGRATOR_ESDF_OCC_FIESTA_INTEGRATOR_H_
