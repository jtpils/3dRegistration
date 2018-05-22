#define _USE_MATH_DEFINES

#include "goicp.h"

#include <cmath>
#include <limits>
#include <queue>

#include <pcl/common/geometry.h>
#include <pcl/common/transforms.h>
#include <pcl/registration/correspondence_estimation.h>
#include <pcl/registration/icp.h>
#include <pcl/registration/icp_nl.h>
#include <pcl/registration/incremental_registration.h>

#include <Eigen/Geometry>

namespace goicp {

constexpr auto kSqrt3 = 1.732050807568877;

constexpr float rmse2sse(float rmse, int num_points) {
  return std::pow(rmse, 2) * static_cast<float>(num_points);
}

template <class T>
constexpr T &minBetween(T a, T b) {
  return a > b ? b : a;
}

template <class T>
constexpr T &maxBetween(T a, T b) {
  return a >= b ? a : b;
}

Goicp::Goicp(PointCloud::Ptr cloud_p, PointCloud::Ptr cloud_q, GoicpOptions options)
    : cloudP(new PointCloud()), cloudQ(new PointCloud()) {
  opts = options;
  inliers_num = cloudQ->points.size();  // later this will support trimming
  global_scale_ = 1;
  transform_ = Eigen::Matrix4f::Identity();
  logger_ = spdlog::stdout_color_mt("Go-ICP");

  // Build Distance Transform
  // logger_->trace("Building Distance Transform");
  // double *x = new double[cloudP->size()];
  // double *y = new double[cloudP->size()];
  // double *z = new double[cloudP->size()];
  // for (int i = 0; i < cloudP->size(); i++) {
  //   // TODO(pantonante): waste of memory, distance transform should accept point clouds
  //   x[i] = static_cast<double>(cloudP->points[i].x);
  //   y[i] = static_cast<double>(cloudP->points[i].y);
  //   z[i] = static_cast<double>(cloudP->points[i].z);
  // }
  // dt.build(x, y, z, cloudP->size());
  // delete[] x;
  // delete[] y;
  // delete[] z;

  // Node init
  opt_rot_node_.a = opt_rot_node_.b = opt_rot_node_.c = -M_PI;
  opt_rot_node_.w = 2 * M_PI;
  opt_rot_node_.l = 0;
  opt_rot_node_.lb = 0;
  opt_trans_node_.lb = 0;

  // Demean point cloud
  logger_->trace("Demeaning point clouds");
  Eigen::Vector4f centroid;
  pcl::demeanPointCloud(*cloud_p, centroid, *cloudP);
  cloudP_centroid_ = centroid.head<3>();
  pcl::demeanPointCloud(*cloud_q, centroid, *cloudQ);
  cloudQ_centroid_ = centroid.head<3>();

  // Normalization
  logger_->trace("Normalizing point clouds");
  for (auto pt : cloudP->points) {  // find global scale
    const auto l2norm = pt.getVector3fMap().norm();
    if (l2norm > global_scale_) global_scale_ = l2norm;
  }
  for (auto pt : cloudQ->points) {
    const auto l2norm = pt.getVector3fMap().norm();
    if (l2norm > global_scale_) global_scale_ = l2norm;
  }
  const auto scaling = Eigen::Matrix4f::Identity() * (1 / global_scale_);
  pcl::transformPointCloud(*cloudP, *cloudP, scaling);
  pcl::transformPointCloud(*cloudQ, *cloudQ, scaling);

  // Precompute the rotation uncertainty distance (rot_uncert) for each point in the point cloud Q
  // and each level of rotation subcube
  for (auto i = 0; i < opts.rot_subcubes; ++i) {
    // Half-side length of each level of rotation subcube
    const auto sigma = opt_rot_node_.w / pow(2.0, i) / 2.0;
    const auto max_angle = minBetween(kSqrt3 * sigma, M_PI);
    std::vector<float> max_rot_dis;
    for (auto pt : cloudQ->points) {
      max_rot_dis.push_back(2 * sin(max_angle / 2) * pt.getVector3fMap().norm());
    }
    rot_uncert.push_back(max_rot_dis);
  }
}

float Goicp::performRegistration() {
  logger_->debug("Go-ICP started, Target RMSE: {}, SSE: {}", opts.fitness_threshold,
                 rmse2sse(opts.fitness_threshold, inliers_num));

  if (opts.no_bnb) {
    logger_->info("No Branch-and-Bound flag enabled, only vanilla ICP is executed");
    return icp(transform_);
  }

  // Go-ICP
  auto fitness = outerBnB();
  if (std::isinf(fitness)) {
    logger_->error("Go-ICP has not converged.");
  }
  return fitness;
}

Eigen::Matrix4f Goicp::getTransform() {
  Eigen::Matrix3f rot(transform_.block<3, 3>(0, 0));
  Eigen::Vector3f t(transform_.block<3, 1>(0, 3));

  Eigen::Matrix4f global_transform = Eigen::Matrix4f::Identity();
  global_transform.block<3, 3>(0, 0) = rot;
  global_transform.block<3, 1>(0, 3) =
      -rot * cloudQ_centroid_ + t * global_scale_ + cloudP_centroid_;

  return global_transform;
}

float Goicp::icp(Eigen::Matrix4f &transform) {
  logger_->trace("ICP, max. iterations {}", opts.icp_max_iterations);
  auto fitness = std::numeric_limits<float>::infinity();
  pcl::IterativeClosestPoint<Point, Point> icp;
  icp.setMaximumIterations(opts.icp_max_iterations);
  icp.setInputSource(cloudP);
  icp.setInputTarget(cloudQ);
  icp.align(*cloudP, transform);  // ICP with initial guess
  logger_->trace("ICP done, converged: {}", icp.hasConverged());

  if (icp.hasConverged()) {
    fitness = icp.getFitnessScore();
    transform = icp.getFinalTransformation();
  } else {
    logger_->error("Local ICP not converged");
  }

  return rmse2sse(fitness, inliers_num);
}

float Goicp::outerBnB() {
  logger_->trace("Starting Go-ICP outer-BnB");
  unsigned int iter = 1;  // iteration counter, just for visualization
  Eigen::Matrix3f rotm;
  std::priority_queue<Node> rot_queue;

  // Initial guess
  fitness = icp(transform_);
  rot_queue.push(opt_rot_node_);  // init. rotation node

  while (!rot_queue.empty()) {
    logger_->info("E* = {} (iter. {})", fitness, iter);

    if (fitness < rmse2sse(opts.fitness_threshold, inliers_num)) {
      logger_->debug("Stopping criteria: registration error below threshold");
      break;
    }

    // Access rotation cube with lower bound...
    Node rot_node_parent = rot_queue.top();
    // ...and remove it from the queue
    rot_queue.pop();

    logger_->trace("Pulled new rotation cube, lower bound: {}", rot_node_parent.lb);

    if ((fitness - rot_node_parent.lb) < rmse2sse(opts.fitness_threshold, inliers_num)) {
      logger_->debug("Stopping criteria: lower bound of current cube is less than a threshold");
      return fitness;
    }

    // Subdivide rotation cube into octant subcubes and calculate upper and lower bounds for each
    Node rot_node;
    rot_node.w = rot_node_parent.w / 2;
    rot_node.l = rot_node_parent.l + 1;

    // For each subcube
    for (auto i = 0; i < 8; i++) {
      logger_->trace("Exploring subcube {}", i);
      rot_node.a = rot_node_parent.a + (i & 1) * rot_node.w;
      rot_node.b = rot_node_parent.b + (i >> 1 & 1) * rot_node.w;
      rot_node.c = rot_node_parent.c + (i >> 2 & 1) * rot_node.w;
      // Subcube centre
      // clang-format off
      auto ax_angle_vector = Eigen::Vector3f(
          rot_node.a + rot_node.w / 2, 
          rot_node.b + rot_node.w / 2, 
          rot_node.c + rot_node.w / 2);
      // clang-format on

      // Skip subcube if it is completely outside the rotation pi-ball
      if (ax_angle_vector.norm() - kSqrt3 * rot_node.w / 2 > M_PI) {
        continue;
      }

      // Angle-axis to rotation-matrix conversion
      if (ax_angle_vector.norm() > 0) {
        rotm = Eigen::AngleAxisf(ax_angle_vector.norm(), ax_angle_vector.normalized());
      } else {
        rotm = Eigen::Matrix3f::Identity();
      }

      // Upper Bound
      // Run inner loop to find rotation upper bound.
      // Calculates the rotation upper bound by finding the translation upper bound for a given
      // rotation, assuming that the rotation is known (zero rotation uncertainty radius)
      Node trans_node;
      auto ub = innerBnB(rotm, nullptr /*Rotation Uncertainty Radius*/, &trans_node);

      // If the upper bound is the best so far, run ICP
      if (ub < fitness) {
        logger_->trace("Better rotation subcube found");

        // Update optimal error and rotation/translation nodes
        fitness = ub;
        opt_rot_node_ = rot_node;
        opt_trans_node_ = trans_node;

        // Update optimal transform
        Eigen::Matrix4f local_transform;
        local_transform.block<3, 3>(0, 0) = rotm;
        // clang-format off
        local_transform.block<3, 1>(0, 3) = Eigen::Vector3f(
            opt_trans_node_.x + opt_trans_node_.w / 2, 
            opt_trans_node_.y + opt_trans_node_.w / 2,
            opt_trans_node_.z + opt_trans_node_.w / 2);
        // clang-format on

        // Run vanilla ICP to find local minima in the subcube
        auto local_fitness = icp(local_transform);
        if (local_fitness < fitness) {
          logger_->trace("Better transformation matrix found");
          fitness = local_fitness;
          transform_ = local_transform;
        }

        // Discard all rotation nodes with high lower bounds in the queue
        // The `priority_queue` purposefully provides a limited interface, which excludes iteration.
        // Creating a new queue to do the work
        std::priority_queue<Node> temp_queue;
        while (!rot_queue.empty()) {
          Node n = rot_queue.top();
          rot_queue.pop();
          if (n.lb < fitness)
            temp_queue.push(n);
          else
            break;
        }
        rot_queue = temp_queue;

        // Lower Bound
        // Run Inner Branch-and-Bound to find rotation lower bound
        // Calculates the rotation lower bound by finding the translation upper bound for a given
        // rotation, assuming that the rotation is uncertain (a positive rotation uncertainty
        // radius) Pass an array of rotation uncertainties for every point in data cloud at this
        // level
        auto lb = innerBnB(rotm, &rot_uncert[rot_node.l], nullptr /*Translation Node*/);

        // If the best error so far is less than the lower bound, remove the rotation subcube from
        // the queue
        if (lb >= fitness) {
          continue;
        }

        // Update node and put it in queue
        logger_->trace("Cube {} added to the queue, UB: {}, LB: {}", i, ub, lb);
        rot_node.ub = ub;
        rot_node.lb = lb;
        rot_queue.push(rot_node);
      }
    }

    ++iter;  // let's got to a new iteration
  }

  if (rot_queue.empty()) {
    logger_->debug("Stopping criteria: subcube exploration completed");
  }

  return fitness;
}

float Goicp::innerBnB(const Eigen::Matrix3f &base_rot, std::vector<float> *gamma_rot,
                      Node *out_node) {
  std::priority_queue<Node> trans_queue;
  // Set optimal translation error to overall so-far optimal error
  // Explore translation nodes that are sub-optimal is redundant
  float so_far_fitness = fitness;

  // Push top-level translation node into the priority queue
  Node init_node;
  init_node.lb = 0;
  trans_queue.push(init_node);

  while (!trans_queue.empty()) {
    Node trans_node_parent = trans_queue.top();
    trans_queue.pop();

    if ((so_far_fitness - trans_node_parent.lb) < rmse2sse(opts.fitness_threshold, inliers_num)) {
      break;
    }

    // Subdivide translation cube into octant subcubes and calculate upper and lower bounds for each
    Node trans_node;
    trans_node.w = trans_node_parent.w / 2;
    trans_node.l = kSqrt3 / 2.0 * trans_node_parent.l;
    // For each subcube
    for (auto i = 0; i < 8; ++i) {
      trans_node.a = trans_node_parent.a + (i & 1) * trans_node.w;
      trans_node.b = trans_node_parent.b + (i >> 1 & 1) * trans_node.w;
      trans_node.c = trans_node_parent.c + (i >> 2 & 1) * trans_node.w;

      Eigen::Vector3f translation(trans_node.x + trans_node.w / 2, trans_node.y + trans_node.w / 2,
                                  trans_node.z + trans_node.w / 2);

      // TODO(pantonante) pass the right transformation matrix
      Eigen::Matrix4f local_transform = Eigen::Matrix4f::Identity();
      local_transform.block<3, 3>(0, 0) = base_rot;
      local_transform.block<3, 1>(0, 3) = translation;
      transCubeBounds(local_transform, gamma_rot, trans_node);

      // If upper bound is better than best so far, update it and the optimal translation
      // node
      if (trans_node.ub < so_far_fitness) {
        logger_->trace("Better translation subcube found");
        so_far_fitness = trans_node.ub;
        if (out_node) *out_node = trans_node;
      }

      // Remove subcube from queue if lower bound is bigger than bcorr so far
      if (trans_node.lb >= so_far_fitness) {
        continue;
      }

      trans_queue.push(trans_node);
    }
  }
  return so_far_fitness;
}

void Goicp::transCubeBounds(Eigen::Matrix4f &transform, std::vector<float> *gamma_rot,
                            Node &trans_node) {
  PointCloud::Ptr local_cloud(new PointCloud());
  float upper_bound = 0;
  float lower_bound = 0;
  float best_error = fitness;
  const float gamma_trans = kSqrt3 / 2.0 * trans_node.w;

  pcl::transformPointCloud(*cloudP, *local_cloud, transform);

  // Find correspondences
  pcl::Correspondences correspondences;
  pcl::registration::CorrespondenceEstimation<Point, Point> est;
  est.setInputSource(local_cloud);
  est.setInputTarget(cloudQ);
  est.determineReciprocalCorrespondences(correspondences);

  // Compute upper bound
  for (auto corr : correspondences) {
    const auto i = corr.index_query;
    const auto j = corr.index_match;
    const auto dist = pcl::geometry::distance(local_cloud->points[i], cloudQ->points[j]);
    if (gamma_rot != nullptr) {
      upper_bound += std::pow(maxBetween(dist - gamma_rot->at(i), .0f), 2);
    } else {
      upper_bound += std::pow(maxBetween(dist, .0f), 2);
    }
    if (gamma_rot != nullptr) {
      lower_bound += std::pow(maxBetween(dist - gamma_rot->at(i) - gamma_trans, .0f), 2);
    } else {
      lower_bound += std::pow(maxBetween(dist - gamma_trans, .0f), 2);
    }
  }

  trans_node.ub = upper_bound;
  trans_node.lb = lower_bound;
}

}  // namespace goicp