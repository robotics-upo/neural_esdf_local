#ifndef VOXBLOX_ROS_ROS_PARAMS_H_
#define VOXBLOX_ROS_ROS_PARAMS_H_

#include <ros/node_handle.h>

#include <voxblox/alignment/icp.h>
#include <voxblox/core/esdf_map.h>
#include <voxblox/core/occupancy_map.h>
#include <voxblox/core/tsdf_map.h>
#include <voxblox/integrator/esdf_integrator.h>
#include <voxblox/integrator/esdf_occ_edt_integrator.h>
#include <voxblox/integrator/esdf_occ_fiesta_integrator.h>
#include <voxblox/integrator/esdf_voxfield_integrator.h>
#include <voxblox/integrator/np_tsdf_integrator.h>
#include <voxblox/integrator/occupancy_integrator.h>
#include <voxblox/integrator/occupancy_tsdf_integrator.h>
#include <voxblox/integrator/tsdf_integrator.h>
#include <voxblox/mesh/mesh_integrator.h>

namespace voxblox {

inline TsdfMap::Config getTsdfMapConfigFromRosParam(
    const ros::NodeHandle& nh_private) {
  TsdfMap::Config tsdf_config;

  /**
   * Workaround for OS X on mac mini not having specializations for float
   * for some reason.
   */
  double voxel_size = tsdf_config.tsdf_voxel_size;
  int voxels_per_side = tsdf_config.tsdf_voxels_per_side;
  nh_private.param("tsdf_voxel_size", voxel_size, voxel_size);
  nh_private.param("tsdf_voxels_per_side", voxels_per_side, voxels_per_side);
  if (!isPowerOfTwo(voxels_per_side)) {
    ROS_ERROR("voxels_per_side must be a power of 2, setting to default value");
    voxels_per_side = tsdf_config.tsdf_voxels_per_side;
  }

  tsdf_config.tsdf_voxel_size = static_cast<FloatingPoint>(voxel_size);
  tsdf_config.tsdf_voxels_per_side = voxels_per_side;

  return tsdf_config;
}

inline ICP::Config getICPConfigFromRosParam(const ros::NodeHandle& nh_private) {
  ICP::Config icp_config;

  nh_private.param(
      "icp_min_match_ratio", icp_config.min_match_ratio,
      icp_config.min_match_ratio);
  nh_private.param(
      "icp_subsample_keep_ratio", icp_config.subsample_keep_ratio,
      icp_config.subsample_keep_ratio);
  nh_private.param(
      "icp_mini_batch_size", icp_config.mini_batch_size,
      icp_config.mini_batch_size);
  nh_private.param(
      "icp_refine_roll_pitch", icp_config.refine_roll_pitch,
      icp_config.refine_roll_pitch);
  nh_private.param(
      "icp_inital_translation_weighting",
      icp_config.inital_translation_weighting,
      icp_config.inital_translation_weighting);
  nh_private.param(
      "icp_inital_rotation_weighting", icp_config.inital_rotation_weighting,
      icp_config.inital_rotation_weighting);

  return icp_config;
}

inline TsdfIntegratorBase::Config getTsdfIntegratorConfigFromRosParam(
    const ros::NodeHandle& nh_private) {
  TsdfIntegratorBase::Config integrator_config;

  integrator_config.voxel_carving_enabled = true;

  const TsdfMap::Config tsdf_config = getTsdfMapConfigFromRosParam(nh_private);

  double max_weight = integrator_config.max_weight;
  // if negative, it means the multiplier of the voxel size
  float truncation_distance = -2.0;

  nh_private.param(
      "truncation_distance", truncation_distance, truncation_distance);

  integrator_config.default_truncation_distance =
      truncation_distance > 0
          ? truncation_distance
          : -truncation_distance * tsdf_config.tsdf_voxel_size;

  nh_private.param(
      "voxel_carving_enabled", integrator_config.voxel_carving_enabled,
      integrator_config.voxel_carving_enabled);

  nh_private.param(
      "max_ray_length_m", integrator_config.max_ray_length_m,
      integrator_config.max_ray_length_m);
  nh_private.param(
      "min_ray_length_m", integrator_config.min_ray_length_m,
      integrator_config.min_ray_length_m);
  nh_private.param("max_weight", max_weight, max_weight);
  integrator_config.max_weight = static_cast<float>(max_weight);
  nh_private.param(
      "use_const_weight", integrator_config.use_const_weight,
      integrator_config.use_const_weight);
  nh_private.param(
      "use_weight_dropoff", integrator_config.use_weight_dropoff,
      integrator_config.use_weight_dropoff);
  nh_private.param(
      "allow_clear", integrator_config.allow_clear,
      integrator_config.allow_clear);
  nh_private.param(
      "start_voxel_subsampling_factor",
      integrator_config.start_voxel_subsampling_factor,
      integrator_config.start_voxel_subsampling_factor);
  nh_private.param(
      "max_consecutive_ray_collisions",
      integrator_config.max_consecutive_ray_collisions,
      integrator_config.max_consecutive_ray_collisions);
  nh_private.param(
      "clear_checks_every_n_frames",
      integrator_config.clear_checks_every_n_frames,
      integrator_config.clear_checks_every_n_frames);
  nh_private.param(
      "max_integration_time_s", integrator_config.max_integration_time_s,
      integrator_config.max_integration_time_s);
  nh_private.param(
      "anti_grazing", integrator_config.enable_anti_grazing,
      integrator_config.enable_anti_grazing);
  nh_private.param(
      "use_sparsity_compensation_factor",
      integrator_config.use_sparsity_compensation_factor,
      integrator_config.use_sparsity_compensation_factor);
  nh_private.param(
      "sparsity_compensation_factor",
      integrator_config.sparsity_compensation_factor,
      integrator_config.sparsity_compensation_factor);
  nh_private.param(
      "integration_order_mode", integrator_config.integration_order_mode,
      integrator_config.integration_order_mode);
  float integrator_threads = std::thread::hardware_concurrency();
  nh_private.param(
      "integrator_threads", integrator_threads, integrator_threads);
  integrator_config.integrator_threads = static_cast<int>(integrator_threads);
  nh_private.param(
      "merge_with_clear", integrator_config.merge_with_clear,
      integrator_config.merge_with_clear);

  return integrator_config;
}

inline NpTsdfIntegratorBase::Config getNpTsdfIntegratorConfigFromRosParam(
    const ros::NodeHandle& nh_private) {
  NpTsdfIntegratorBase::Config integrator_config;

  integrator_config.voxel_carving_enabled = true;

  const TsdfMap::Config tsdf_config = getTsdfMapConfigFromRosParam(nh_private);

  double max_weight = integrator_config.max_weight;
  // if negative, it means the multiplier of the voxel size
  float truncation_distance = -2.0;

  nh_private.param(
      "truncation_distance", truncation_distance, truncation_distance);

  integrator_config.default_truncation_distance =
      truncation_distance > 0
          ? truncation_distance
          : -truncation_distance * tsdf_config.tsdf_voxel_size;

  nh_private.param(
      "voxel_carving_enabled", integrator_config.voxel_carving_enabled,
      integrator_config.voxel_carving_enabled);

  nh_private.param(
      "max_ray_length_m", integrator_config.max_ray_length_m,
      integrator_config.max_ray_length_m);
  nh_private.param(
      "min_ray_length_m", integrator_config.min_ray_length_m,
      integrator_config.min_ray_length_m);
  nh_private.param("max_weight", max_weight, max_weight);
  integrator_config.max_weight = static_cast<float>(max_weight);
  nh_private.param(
      "use_const_weight", integrator_config.use_const_weight,
      integrator_config.use_const_weight);
  nh_private.param(
      "weight_reduction_exp",
      integrator_config.weight_reduction_exp,  // NOLINT
      integrator_config.weight_reduction_exp);
  nh_private.param(
      "use_weight_dropoff", integrator_config.use_weight_dropoff,
      integrator_config.use_weight_dropoff);
  nh_private.param(
      "weight_dropoff_epsilon",
      integrator_config.weight_dropoff_epsilon,  // NOLINT
      integrator_config.weight_dropoff_epsilon);
  nh_private.param(
      "allow_clear", integrator_config.allow_clear,
      integrator_config.allow_clear);
  nh_private.param(
      "start_voxel_subsampling_factor",
      integrator_config.start_voxel_subsampling_factor,
      integrator_config.start_voxel_subsampling_factor);
  nh_private.param(
      "max_consecutive_ray_collisions",
      integrator_config.max_consecutive_ray_collisions,
      integrator_config.max_consecutive_ray_collisions);
  nh_private.param(
      "clear_checks_every_n_frames",
      integrator_config.clear_checks_every_n_frames,
      integrator_config.clear_checks_every_n_frames);
  nh_private.param(
      "max_integration_time_s", integrator_config.max_integration_time_s,
      integrator_config.max_integration_time_s);
  nh_private.param(
      "anti_grazing", integrator_config.enable_anti_grazing,
      integrator_config.enable_anti_grazing);
  nh_private.param(
      "use_sparsity_compensation_factor",
      integrator_config.use_sparsity_compensation_factor,
      integrator_config.use_sparsity_compensation_factor);
  nh_private.param(
      "sparsity_compensation_factor",
      integrator_config.sparsity_compensation_factor,
      integrator_config.sparsity_compensation_factor);
  nh_private.param(
      "integration_order_mode", integrator_config.integration_order_mode,
      integrator_config.integration_order_mode);
  float integrator_threads = std::thread::hardware_concurrency();
  nh_private.param(
      "integrator_threads", integrator_threads, integrator_threads);
  integrator_config.integrator_threads = static_cast<int>(integrator_threads);
  nh_private.param(
      "merge_with_clear", integrator_config.merge_with_clear,
      integrator_config.merge_with_clear);
  nh_private.param(
      "normal_available", integrator_config.normal_available,
      integrator_config.normal_available);
  nh_private.param(
      "reliable_band_ratio", integrator_config.reliable_band_ratio,
      integrator_config.reliable_band_ratio);
  nh_private.param(
      "curve_assumption", integrator_config.curve_assumption,
      integrator_config.curve_assumption);
  nh_private.param(
      "reliable_normal_ratio_thre",
      integrator_config.reliable_normal_ratio_thre,
      integrator_config.reliable_normal_ratio_thre);

  return integrator_config;
}

inline EsdfMap::Config getEsdfMapConfigFromRosParam(
    const ros::NodeHandle& nh_private) {
  EsdfMap::Config esdf_config;

  const TsdfMap::Config tsdf_config = getTsdfMapConfigFromRosParam(nh_private);
  esdf_config.esdf_voxel_size = tsdf_config.tsdf_voxel_size;
  esdf_config.esdf_voxels_per_side = tsdf_config.tsdf_voxels_per_side;

  return esdf_config;
}

inline EsdfIntegrator::Config getEsdfIntegratorConfigFromRosParam(
    const ros::NodeHandle& nh_private) {
  EsdfIntegrator::Config esdf_integrator_config;

  TsdfIntegratorBase::Config tsdf_integrator_config =
      getTsdfIntegratorConfigFromRosParam(nh_private);

  esdf_integrator_config.min_distance_m =
      tsdf_integrator_config.default_truncation_distance / 2.0;

  nh_private.param(
      "esdf_euclidean_distance", esdf_integrator_config.full_euclidean_distance,
      esdf_integrator_config.full_euclidean_distance);
  nh_private.param(
      "esdf_max_distance_m", esdf_integrator_config.max_distance_m,
      esdf_integrator_config.max_distance_m);
  nh_private.param(
      "esdf_min_distance_m", esdf_integrator_config.min_distance_m,
      esdf_integrator_config.min_distance_m);
  nh_private.param(
      "esdf_default_distance_m", esdf_integrator_config.default_distance_m,
      esdf_integrator_config.default_distance_m);
  nh_private.param(
      "esdf_min_diff_m", esdf_integrator_config.min_diff_m,
      esdf_integrator_config.min_diff_m);
  nh_private.param(
      "clear_sphere_radius", esdf_integrator_config.clear_sphere_radius,
      esdf_integrator_config.clear_sphere_radius);
  nh_private.param(
      "occupied_sphere_radius", esdf_integrator_config.occupied_sphere_radius,
      esdf_integrator_config.occupied_sphere_radius);
  nh_private.param(
      "esdf_add_occupied_crust", esdf_integrator_config.add_occupied_crust,
      esdf_integrator_config.add_occupied_crust);

  if (esdf_integrator_config.default_distance_m <
      esdf_integrator_config.max_distance_m) {
    esdf_integrator_config.default_distance_m =
        esdf_integrator_config.max_distance_m;
  }

  return esdf_integrator_config;
}

inline MeshIntegratorConfig getMeshIntegratorConfigFromRosParam(
    const ros::NodeHandle& nh_private) {
  MeshIntegratorConfig mesh_integrator_config;

  nh_private.param(
      "mesh_min_weight", mesh_integrator_config.min_weight,
      mesh_integrator_config.min_weight);
  nh_private.param(
      "mesh_use_color", mesh_integrator_config.use_color,
      mesh_integrator_config.use_color);

  return mesh_integrator_config;
}

inline OccupancyMap::Config getOccupancyMapConfigFromRosParam(
    const ros::NodeHandle& nh_private) {
  OccupancyMap::Config occ_config;

  /**
   * Workaround for OS X on mac mini not having specializations for float
   * for some reason.
   */
  double voxel_size = occ_config.occupancy_voxel_size;
  int voxels_per_side = occ_config.occupancy_voxels_per_side;
  nh_private.param("occ_voxel_size", voxel_size, voxel_size);
  nh_private.param(
      "occ_voxels_per_side", voxels_per_side,
      voxels_per_side);  // block size (unit: voxel)
  if (!isPowerOfTwo(voxels_per_side)) {
    ROS_ERROR("voxels_per_side must be a power of 2, setting to default value");
    voxels_per_side = occ_config.occupancy_voxels_per_side;
  }

  occ_config.occupancy_voxel_size = static_cast<FloatingPoint>(voxel_size);
  occ_config.occupancy_voxels_per_side = voxels_per_side;

  return occ_config;
}

inline OccTsdfIntegrator::Config getOccTsdfIntegratorConfigFromRosParam(
    const ros::NodeHandle& nh_private) {
  OccTsdfIntegrator::Config integrator_config;

  nh_private.param(
      "occ_min_weight", integrator_config.min_weight,
      integrator_config.min_weight);

  nh_private.param(
      "occ_voxel_size_ratio", integrator_config.occ_voxel_size_ratio,
      integrator_config.occ_voxel_size_ratio);

  return integrator_config;
}

inline EsdfMap::Config getEsdfMapConfigFromOccMapRosParam(
    const ros::NodeHandle& nh_private) {
  EsdfMap::Config esdf_config;

  const OccupancyMap::Config occ_config =
      getOccupancyMapConfigFromRosParam(nh_private);
  esdf_config.esdf_voxel_size = occ_config.occupancy_voxel_size;
  esdf_config.esdf_voxels_per_side = occ_config.occupancy_voxels_per_side;

  return esdf_config;
}

inline TsdfMap::Config getTsdfMapConfigFromOccMapRosParam(
    const ros::NodeHandle& nh_private) {
  TsdfMap::Config tsdf_config;

  const OccupancyMap::Config occ_config =
      getOccupancyMapConfigFromRosParam(nh_private);
  tsdf_config.tsdf_voxel_size = occ_config.occupancy_voxel_size;
  tsdf_config.tsdf_voxels_per_side = occ_config.occupancy_voxels_per_side;

  return tsdf_config;
}

inline TsdfMap::Config getTsdfMapConfigFromEsdfMapRosParam(
    const ros::NodeHandle& nh_private) {
  TsdfMap::Config tsdf_config;

  const EsdfMap::Config esdf_config = getEsdfMapConfigFromRosParam(nh_private);
  tsdf_config.tsdf_voxel_size = esdf_config.esdf_voxel_size;
  tsdf_config.tsdf_voxels_per_side = esdf_config.esdf_voxels_per_side;

  return tsdf_config;
}

inline EsdfVoxfieldIntegrator::Config
getEsdfVoxfieldIntegratorConfigFromRosParam(const ros::NodeHandle& nh_private) {
  EsdfVoxfieldIntegrator::Config esdf_integrator_config;

  int range_boundary_offset_x = esdf_integrator_config.range_boundary_offset(0);
  int range_boundary_offset_y = esdf_integrator_config.range_boundary_offset(1);
  int range_boundary_offset_z = esdf_integrator_config.range_boundary_offset(2);

  nh_private.param(
      "local_range_offset_x", range_boundary_offset_x, range_boundary_offset_x);

  nh_private.param(
      "local_range_offset_y", range_boundary_offset_y, range_boundary_offset_y);

  nh_private.param(
      "local_range_offset_z", range_boundary_offset_z, range_boundary_offset_z);

  nh_private.param(
      "esdf_max_distance_m", esdf_integrator_config.max_distance_m,
      esdf_integrator_config.max_distance_m);

  nh_private.param(
      "esdf_default_distance_m", esdf_integrator_config.default_distance_m,
      esdf_integrator_config.default_distance_m);

  nh_private.param(
      "fix_band_distance_m",
      esdf_integrator_config.band_distance_m,  // NOLINT
      esdf_integrator_config.band_distance_m);

  nh_private.param(
      "max_behind_surface_m", esdf_integrator_config.max_behind_surface_m,
      esdf_integrator_config.max_behind_surface_m);
  // max_behind_surface_m should be at least sqrt(3) * truncation_dist

  nh_private.param(
      "occ_min_weight", esdf_integrator_config.min_weight,
      esdf_integrator_config.min_weight);

  nh_private.param(
      "occ_voxel_size_ratio", esdf_integrator_config.occ_voxel_size_ratio,
      esdf_integrator_config.occ_voxel_size_ratio);

  nh_private.param(
      "num_buckets", esdf_integrator_config.num_buckets,
      esdf_integrator_config.num_buckets);

  nh_private.param(
      "patch_on", esdf_integrator_config.patch_on,
      esdf_integrator_config.patch_on);

  nh_private.param(
      "early_break", esdf_integrator_config.early_break,
      esdf_integrator_config.early_break);

  nh_private.param(
      "finer_esdf_on", esdf_integrator_config.finer_esdf_on,
      esdf_integrator_config.finer_esdf_on);

  esdf_integrator_config.range_boundary_offset(0) = range_boundary_offset_x;
  esdf_integrator_config.range_boundary_offset(1) = range_boundary_offset_y;
  esdf_integrator_config.range_boundary_offset(2) = range_boundary_offset_z;

  return esdf_integrator_config;
}

inline EsdfOccFiestaIntegrator::Config
getEsdfOccFiestaIntegratorConfigFromRosParam(
    const ros::NodeHandle& nh_private) {  // NOLINT
  EsdfOccFiestaIntegrator::Config esdf_integrator_config;

  int range_boundary_offset_x = esdf_integrator_config.range_boundary_offset(0);
  int range_boundary_offset_y = esdf_integrator_config.range_boundary_offset(1);
  int range_boundary_offset_z = esdf_integrator_config.range_boundary_offset(2);

  // esdf_integrator_config.min_distance_m =
  //     tsdf_integrator_config.default_truncation_distance / 2.0;

  nh_private.param(
      "local_range_offset_x", range_boundary_offset_x, range_boundary_offset_x);

  nh_private.param(
      "local_range_offset_y", range_boundary_offset_y, range_boundary_offset_y);

  nh_private.param(
      "local_range_offset_z", range_boundary_offset_z, range_boundary_offset_z);

  nh_private.param(
      "esdf_max_distance_m", esdf_integrator_config.max_distance_m,
      esdf_integrator_config.max_distance_m);

  nh_private.param(
      "esdf_default_distance_m", esdf_integrator_config.default_distance_m,
      esdf_integrator_config.default_distance_m);

  nh_private.param(
      "max_behind_surface_m", esdf_integrator_config.max_behind_surface_m,
      esdf_integrator_config.max_behind_surface_m);
  // max_behind_surface_m should be at least sqrt(3) * truncation_dist

  nh_private.param(
      "num_buckets", esdf_integrator_config.num_buckets,
      esdf_integrator_config.num_buckets);

  nh_private.param(
      "patch_on", esdf_integrator_config.patch_on,
      esdf_integrator_config.patch_on);

  nh_private.param(
      "early_break", esdf_integrator_config.early_break,
      esdf_integrator_config.early_break);

  esdf_integrator_config.range_boundary_offset(0) = range_boundary_offset_x;
  esdf_integrator_config.range_boundary_offset(1) = range_boundary_offset_y;
  esdf_integrator_config.range_boundary_offset(2) = range_boundary_offset_z;

  return esdf_integrator_config;
}

inline EsdfOccEdtIntegrator::Config getEsdfEdtIntegratorConfigFromRosParam(
    const ros::NodeHandle& nh_private) {
  EsdfOccEdtIntegrator::Config esdf_integrator_config;

  int range_boundary_offset_x = esdf_integrator_config.range_boundary_offset(0);
  int range_boundary_offset_y = esdf_integrator_config.range_boundary_offset(1);
  int range_boundary_offset_z = esdf_integrator_config.range_boundary_offset(2);

  // esdf_integrator_config.min_distance_m =
  //     tsdf_integrator_config.default_truncation_distance / 2.0;

  nh_private.param(
      "local_range_offset_x", range_boundary_offset_x, range_boundary_offset_x);

  nh_private.param(
      "local_range_offset_y", range_boundary_offset_y, range_boundary_offset_y);

  nh_private.param(
      "local_range_offset_z", range_boundary_offset_z, range_boundary_offset_z);

  nh_private.param(
      "esdf_max_distance_m", esdf_integrator_config.max_distance_m,
      esdf_integrator_config.max_distance_m);

  nh_private.param(
      "esdf_default_distance_m", esdf_integrator_config.default_distance_m,
      esdf_integrator_config.default_distance_m);

  nh_private.param(
      "max_behind_surface_m", esdf_integrator_config.max_behind_surface_m,
      esdf_integrator_config.max_behind_surface_m);
  // max_behind_surface_m should be at least sqrt(3) * truncation_dist

  nh_private.param(
      "num_buckets", esdf_integrator_config.num_buckets,
      esdf_integrator_config.num_buckets);

  if (esdf_integrator_config.default_distance_m <
      esdf_integrator_config.max_distance_m) {
    esdf_integrator_config.default_distance_m =
        esdf_integrator_config.max_distance_m;
  }

  esdf_integrator_config.range_boundary_offset(0) = range_boundary_offset_x;
  esdf_integrator_config.range_boundary_offset(1) = range_boundary_offset_y;
  esdf_integrator_config.range_boundary_offset(2) = range_boundary_offset_z;

  return esdf_integrator_config;
}

}  // namespace voxblox

#endif  // VOXBLOX_ROS_ROS_PARAMS_H_
