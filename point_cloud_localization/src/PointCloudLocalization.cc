/*
 * Copyright (c) 2016, The Regents of the University of California (Regents).
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *    1. Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *
 *    2. Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *
 *    3. Neither the name of the copyright holder nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS AS IS
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * Please contact the author(s) of this library if you have any questions.
 * Authors: Erik Nelson            ( eanelson@eecs.berkeley.edu )
 */

#include <point_cloud_localization/PointCloudLocalization.h>
#include <geometry_utils/GeometryUtilsROS.h>
#include <parameter_utils/ParameterUtils.h>
#include <geometry_msgs/PoseStamped.h>
#include <geometry_msgs/TransformStamped.h>

#include <pcl/search/impl/search.hpp>

#include <pcl/registration/gicp.h>

namespace gu = geometry_utils;
namespace gr = gu::ros;
namespace pu = parameter_utils;

using pcl::GeneralizedIterativeClosestPoint;
using pcl::PointCloud;
using pcl::PointXYZI;
using pcl::transformPointCloud;

PointCloudLocalization::PointCloudLocalization() {}
PointCloudLocalization::~PointCloudLocalization() {}

bool PointCloudLocalization::Initialize(const ros::NodeHandle& n) {
  name_ = ros::names::append(n.getNamespace(), "PointCloudLocalization");

  if (!LoadParameters(n)) {
    ROS_ERROR("%s: Failed to load parameters.", name_.c_str());
    return false;
  }

  if (!RegisterCallbacks(n)) {
    ROS_ERROR("%s: Failed to register callbacks.", name_.c_str());
    return false;
  }

  return true;
}

bool PointCloudLocalization::LoadParameters(const ros::NodeHandle& n) {
  // Load frame ids.
  if (!pu::Get("frame_id/fixed", fixed_frame_id_)) return false;
  if (!pu::Get("frame_id/base", base_frame_id_)) return false;

  // Load initial position.
  double init_x = 0.0, init_y = 0.0, init_z = 0.0;
  double init_qx = 0.0, init_qy = 0.0, init_qz = 0.0, init_qw = 1.0;
  bool b_have_fiducial = true;
  if (!pu::Get("fiducial_calibration/position/x", init_x))
    b_have_fiducial = false;
  if (!pu::Get("fiducial_calibration/position/y", init_y))
    b_have_fiducial = false;
  if (!pu::Get("fiducial_calibration/position/z", init_z))
    b_have_fiducial = false;
  if (!pu::Get("fiducial_calibration/orientation/x", init_qx))
    b_have_fiducial = false;
  if (!pu::Get("fiducial_calibration/orientation/y", init_qy))
    b_have_fiducial = false;
  if (!pu::Get("fiducial_calibration/orientation/z", init_qz))
    b_have_fiducial = false;
  if (!pu::Get("fiducial_calibration/orientation/w", init_qw))
    b_have_fiducial = false;

  if (!b_have_fiducial) {
    ROS_WARN("Can't find fiducials, using origin");
  }

  // convert initial quaternion to Roll/Pitch/Yaw
  double init_roll = 0.0, init_pitch = 0.0, init_yaw = 0.0;
  gu::Quat q(gu::Quat(init_qw, init_qx, init_qy, init_qz));
  gu::Rot3 m1;
  m1 = gu::QuatToR(q);
  init_roll = m1.Roll();
  init_pitch = m1.Pitch();
  init_yaw = m1.Yaw();
  
  integrated_estimate_.translation = gu::Vec3(init_x, init_y, init_z);
  integrated_estimate_.rotation = gu::Rot3(init_roll, init_pitch, init_yaw);

  // Load algorithm parameters.
  if (!pu::Get("localization/compute_icp_covariance", params_.compute_icp_covariance)) return false;
  if (!pu::Get("localization/tf_epsilon", params_.tf_epsilon)) return false;
  if (!pu::Get("localization/corr_dist", params_.corr_dist)) return false;
  if (!pu::Get("localization/iterations", params_.iterations)) return false;

  if (!pu::Get("localization/transform_thresholding", transform_thresholding_))
    return false;
  if (!pu::Get("localization/max_translation", max_translation_)) return false;
  if (!pu::Get("localization/max_rotation", max_rotation_)) return false;

  pu::Get("b_publish_tfs", b_publish_tfs_);

  return true;
}

bool PointCloudLocalization::RegisterCallbacks(const ros::NodeHandle& n) {
  // Create a local nodehandle to manage callback subscriptions.
  ros::NodeHandle nl(n);

  query_pub_ = nl.advertise<PointCloud>("localization_query_points", 10, false);
  reference_pub_ =
      nl.advertise<PointCloud>("localization_reference_points", 10, false);
  aligned_pub_ =
      nl.advertise<PointCloud>("localization_aligned_points", 10, false);
  incremental_estimate_pub_ = nl.advertise<geometry_msgs::PoseWithCovarianceStamped>(
      "localization_incremental_estimate", 10, false);
  integrated_estimate_pub_ = nl.advertise<geometry_msgs::PoseWithCovarianceStamped>(
      "localization_integrated_estimate", 10, false);
  condition_number_pub_ = nl.advertise<std_msgs::Float64>("condition_number", 10, false);

  return true;
}

const gu::Transform3& PointCloudLocalization::GetIncrementalEstimate() const {
  return incremental_estimate_;
}

const gu::Transform3& PointCloudLocalization::GetIntegratedEstimate() const {
  return integrated_estimate_;
}

void PointCloudLocalization::SetIntegratedEstimate(
    const gu::Transform3& integrated_estimate) {
  integrated_estimate_ = integrated_estimate;

  // Publish transform between fixed frame and localization frame.
  if (b_publish_tfs_) {
    geometry_msgs::TransformStamped tf;
    tf.transform = gr::ToRosTransform(integrated_estimate_);
    tf.header.stamp = stamp_;
    tf.header.frame_id = fixed_frame_id_;
    tf.child_frame_id = base_frame_id_;
    tfbr_.sendTransform(tf);
  }
}

bool PointCloudLocalization::MotionUpdate(
    const gu::Transform3& incremental_odom) {
  // Store the incremental transform from odometry.
  incremental_estimate_ = incremental_odom;
  return true;
}

bool PointCloudLocalization::TransformPointsToFixedFrame(
    const PointCloud& points, PointCloud* points_transformed) const {
  if (points_transformed == NULL) {
    ROS_ERROR("%s: Output is null.", name_.c_str());
    return false;
  }

  // Compose the current incremental estimate (from odometry) with the
  // integrated estimate, and transform the incoming point cloud.
  const gu::Transform3 estimate =
      gu::PoseUpdate(integrated_estimate_, incremental_estimate_);
  const Eigen::Matrix<double, 3, 3> R = estimate.rotation.Eigen();
  const Eigen::Matrix<double, 3, 1> T = estimate.translation.Eigen();

  Eigen::Matrix4d tf;
  tf.block(0, 0, 3, 3) = R;
  tf.block(0, 3, 3, 1) = T;

  pcl::transformPointCloud(points, *points_transformed, tf);

  return true;
}

bool PointCloudLocalization::TransformPointsToSensorFrame(
    const PointCloud& points, PointCloud* points_transformed) const {
  if (points_transformed == NULL) {
    ROS_ERROR("%s: Output is null.", name_.c_str());
    return false;
  }

  // Compose the current incremental estimate (from odometry) with the
  // integrated estimate, then invert to go from world to sensor frame.
  const gu::Transform3 estimate = gu::PoseInverse(
      gu::PoseUpdate(integrated_estimate_, incremental_estimate_));
  const Eigen::Matrix<double, 3, 3> R = estimate.rotation.Eigen();
  const Eigen::Matrix<double, 3, 1> T = estimate.translation.Eigen();

  Eigen::Matrix4d tf;
  tf.block(0, 0, 3, 3) = R;
  tf.block(0, 3, 3, 1) = T;

  pcl::transformPointCloud(points, *points_transformed, tf);

  return true;
}

bool PointCloudLocalization::MeasurementUpdate(const PointCloud::Ptr& query,
                                               const PointCloud::Ptr& reference,
                                               PointCloud* aligned_query) {
  if (aligned_query == NULL) {
    ROS_ERROR("%s: Output is null.", name_.c_str());
    return false;
  }

  // Store time stamp.
  stamp_.fromNSec(query->header.stamp*1e3);

  // ICP-based alignment. Generalized ICP does (roughly) plane-to-plane
  // matching, and is much more robust than standard ICP.
  GeneralizedIterativeClosestPoint<pcl::PointXYZI, pcl::PointXYZI> icp;
  icp.setTransformationEpsilon(params_.tf_epsilon);
  icp.setMaxCorrespondenceDistance(params_.corr_dist);
  icp.setMaximumIterations(params_.iterations);
  icp.setRANSACIterations(0);
  icp.setMaximumOptimizerIterations(50); // default 20

  icp.setInputSource(query);
  icp.setInputTarget(reference);

  PointCloud icpAlignedPointsLocalization_;
  icp.align(icpAlignedPointsLocalization_);
  icpFitnessScore_ = icp.getFitnessScore();

  ROS_INFO_STREAM("ICP Fitness score in PointCloudLocalization::MeasurementUpdate is " << icpFitnessScore_);

  // Retrieve transformation and estimate and update.
  const Eigen::Matrix4f T = icp.getFinalTransformation();
  pcl::transformPointCloud(*query, *aligned_query, T);

  gu::Transform3 pose_update;
  pose_update.translation = gu::Vec3(T(0, 3), T(1, 3), T(2, 3));
  pose_update.rotation = gu::Rot3(T(0, 0), T(0, 1), T(0, 2),
                                  T(1, 0), T(1, 1), T(1, 2),
                                  T(2, 0), T(2, 1), T(2, 2));

  // Only update if the transform is small enough.
  if (!transform_thresholding_ ||
      (pose_update.translation.Norm() <= max_translation_ &&
       pose_update.rotation.ToEulerZYX().Norm() <= max_rotation_)) {
    incremental_estimate_ = gu::PoseUpdate(incremental_estimate_, pose_update);
  } else {
    ROS_WARN(
        " %s: Discarding incremental transformation with norm (t: %lf, r: %lf)",
        name_.c_str(), pose_update.translation.Norm(),
        pose_update.rotation.ToEulerZYX().Norm());
  }

  integrated_estimate_ =
    gu::PoseUpdate(integrated_estimate_, incremental_estimate_);

  Eigen::Matrix<double, 6, 6> icp_covariance;
  icp_covariance = Eigen::Matrix<double, 6, 6>::Zero();

  if (params_.compute_icp_covariance) {
    // Compute the covariance matrix for the estimated transform.
    ComputeICPCovariance(icpAlignedPointsLocalization_, T, icp_covariance);
    
    // Convert pose estimates to ROS format and publish.
    PublishPose(incremental_estimate_, icp_covariance, incremental_estimate_pub_);
    PublishPose(integrated_estimate_, icp_covariance, integrated_estimate_pub_);
  } else {
    // Convert pose estimates to ROS format and publish.
    PublishPose(incremental_estimate_, icp_covariance, incremental_estimate_pub_);
    PublishPose(integrated_estimate_, icp_covariance, integrated_estimate_pub_);
  }
  // Publish point clouds for visualization.
  PublishPoints(*query, query_pub_);
  PublishPoints(*reference, reference_pub_);
  PublishPoints(*aligned_query, aligned_pub_);

  // Publish transform between fixed frame and localization frame.
  if (b_publish_tfs_){
    geometry_msgs::TransformStamped tf;
    tf.transform = gr::ToRosTransform(integrated_estimate_);
    tf.header.stamp = stamp_;
    tf.header.frame_id = fixed_frame_id_;
    tf.child_frame_id = base_frame_id_;
    tfbr_.sendTransform(tf);
  }

  return true;
}

bool PointCloudLocalization::ComputeICPCovariance(const pcl::PointCloud<pcl::PointXYZI> pointCloud, const Eigen::Matrix4f T, Eigen::Matrix<double, 6, 6> &covariance){
  geometry_utils::Transform3 ICP_transformation;

  // Extract translation values from T
  double t_x = T(0,3);
  double t_y = T(1,3);
  double t_z = T(2,3);

  // Extract roll, pitch and yaw from T
  ICP_transformation.rotation = gu::Rot3(T(0, 0), T(0, 1), T(0, 2),
                                            T(1, 0), T(1, 1), T(1, 2),
                                            T(2, 0), T(2, 1), T(2, 2));
  double r = ICP_transformation.rotation.Roll();
  double p = ICP_transformation.rotation.Pitch();
  double y = ICP_transformation.rotation.Yaw();

  // Symbolic expression of the Jacobian matrix
  double J11,    J12,	   J13,   J14,  J15,    J16,
         J21,    J22,	   J23,   J24,  J25,    J26,
         J31,    J32,	   J33,   J34,  J35,    J36;
  
  Eigen::Matrix<double, 6, 6> H;
  H = Eigen::MatrixXd::Zero(6, 6);

  // Compute the entries of Jacobian
  // Entries of Jacobian matrix are obtained from MATLAB Symbolic Toolbox
  for (size_t i = 0; i < pointCloud.points.size(); ++i){
    double p_x = pointCloud.points[i].x;
    double p_y = pointCloud.points[i].y;
    double p_z = pointCloud.points[i].z;

    J11 = 0.0;
    J12 = -2.0*(p_z*sin(p) + p_x*cos(p)*cos(y) - p_y*cos(p)*sin(y))*(t_x - p_x + p_z*cos(p) - p_x*cos(y)*sin(p) + p_y*sin(p)*sin(y));
    J13 = 2.0*(p_y*cos(y)*sin(p) + p_x*sin(p)*sin(y))*(t_x - p_x + p_z*cos(p) - p_x*cos(y)*sin(p) + p_y*sin(p)*sin(y));
    J14 = 2.0*t_x - 2.0*p_x + 2.0*p_z*cos(p) - 2.0*p_x*cos(y)*sin(p) + 2.0*p_y*sin(p)*sin(y);
    J15 = 0.0;
    J16 = 0.0;

    J21 = 2.0*(p_x*(cos(r)*sin(y) + cos(p)*cos(y)*sin(r)) + p_y*(cos(r)*cos(y) - cos(p)*sin(r)*sin(y)) +
           p_z*sin(p)*sin(r))*(p_y - t_y + p_x*(sin(r)*sin(y) - cos(p)*cos(r)*cos(y)) + p_y*(cos(y)*sin(r) +
           cos(p)*cos(r)*sin(y)) - p_z*cos(r)*sin(p));
    J22 = -2.0*(p_z*cos(p)*cos(r) - p_x*cos(r)*cos(y)*sin(p) + p_y*cos(r)*sin(p)*sin(y))*(p_y - t_y + 
          p_x*(sin(r)*sin(y) - cos(p)*cos(r)*cos(y)) + p_y*(cos(y)*sin(r) + cos(p)*cos(r)*sin(y)) - p_z*cos(r)*sin(p));
    J23 = 2.0*(p_x*(cos(y)*sin(r) + cos(p)*cos(r)*sin(y)) - p_y*(sin(r)*sin(y) - cos(p)*cos(r)*cos(y)))*(p_y - t_y + 
          p_x*(sin(r)*sin(y) - cos(p)*cos(r)*cos(y)) + p_y*(cos(y)*sin(r) + cos(p)*cos(r)*sin(y)) - p_z*cos(r)*sin(p));
    J24 = 0.0;
    J25 = 2.0*t_y - 2.0*p_y - 2.0*p_x*(sin(r)*sin(y) - cos(p)*cos(r)*cos(y)) - 2.0*p_y*(cos(y)*sin(r) + cos(p)*cos(r)*sin(y)) + 2.0*p_z*cos(r)*sin(p);
    J26 = 0.0;

    J31 = -2.0*(p_x*(sin(r)*sin(y) - cos(p)*cos(r)*cos(y)) + p_y*(cos(y)*sin(r) + cos(p)*cos(r)*sin(y)) - p_z*cos(r)*sin(p))*(t_z - p_z +
           p_x*(cos(r)*sin(y) + cos(p)*cos(y)*sin(r)) + p_y*(cos(r)*cos(y) - cos(p)*sin(r)*sin(y)) + p_z*sin(p)*sin(r));
    J32 = 2.0*(p_z*cos(p)*sin(r) - p_x*cos(y)*sin(p)*sin(r) + p_y*sin(p)*sin(r)*sin(y))*(t_z - p_z + p_x*(cos(r)*sin(y) +
          cos(p)*cos(y)*sin(r)) + p_y*(cos(r)*cos(y) - cos(p)*sin(r)*sin(y)) + p_z*sin(p)*sin(r));
    J33 = 2.0*(p_x*(cos(r)*cos(y) - cos(p)*sin(r)*sin(y)) - p_y*(cos(r)*sin(y) + cos(p)*cos(y)*sin(r)))*(t_z - p_z +
          p_x*(cos(r)*sin(y) + cos(p)*cos(y)*sin(r)) + p_y*(cos(r)*cos(y) - cos(p)*sin(r)*sin(y)) + p_z*sin(p)*sin(r));
    J34 = 0.0;
    J35 = 0.0;
    J36 = 2.0*t_z - 2.0*p_z + 2.0*p_x*(cos(r)*sin(y) + cos(p)*cos(y)*sin(r)) + 2.0*p_y*(cos(r)*cos(y) - cos(p)*sin(r)*sin(y)) + 2.0*p_z*sin(p)*sin(r);

    // Form the 3X6 Jacobian matrix
    Eigen::Matrix<double, 3, 6> J;
    J << J11,    J12,	   J13,   J14,  J15,    J16,
         J21,    J22,	   J23,   J24,  J25,    J26,
         J31,    J32,	   J33,   J34,  J35,    J36;
    // Compute J'XJ (6X6) matrix and keep adding for all the points in the point cloud
    H += J.transpose() * J;
  }
  covariance = H.inverse() * icpFitnessScore_;

  // Compute the SVD of the covariance matrix
  Eigen::JacobiSVD<Eigen::MatrixXd> svd(covariance, Eigen::ComputeThinU | Eigen::ComputeThinV);

  //Extract the singular values from SVD
  auto singular_values = svd.singularValues();
  // The covariance matrix is a symmetric matrix, so its  singular  values  are  the absolute values of its nonzero eigenvalues
  // Condition number is the ratio of the largest and smallest eigenvalues.
  double condition_number = singular_values(0)/singular_values(5);
  PublishConditionNumber(condition_number, condition_number_pub_);
   
  return true;
}

void PointCloudLocalization::PublishPoints(const PointCloud& points,
                                           const ros::Publisher& pub) const {
  // Check for subscribers before doing any work.
  if (pub.getNumSubscribers() > 0) {
    PointCloud out;
    out = points;
    out.header.frame_id = base_frame_id_;
    pub.publish(out);
  }
}

void PointCloudLocalization::PublishPose(const geometry_utils::Transform3& pose,
                                         const Eigen::Matrix<double, 6, 6>& covariance,
                                         const ros::Publisher& pub){
  // Check for subscribers before doing any work.
  if (pub.getNumSubscribers() == 0)
   return;

  // Convert from gu::Transform3 to ROS's Pose with covariance stamped type and publish.
  geometry_msgs::PoseWithCovarianceStamped ros_pose;
  ros_pose.pose.pose = gr::ToRosPose(pose);
  ros_pose.header.frame_id = fixed_frame_id_;
  ros_pose.header.stamp = stamp_;

  for (size_t i = 0; i < 36; i++) {
    size_t row = static_cast<size_t>(i / 6);
    size_t col = i % 6;
    ros_pose.pose.covariance[i] = covariance(row, col);
  }

  pub.publish(ros_pose);
}

// inline geometry_msgs::Pose ToRosPose(const Transform3& trans) {
//   geometry_msgs::Pose msg;
//   msg.position = ToRosPoint(trans.translation);
//   msg.orientation = ToRosQuat(RToQuat(trans.rotation));
//   return msg;
// }

void PointCloudLocalization::PublishPoseNoUpdate() {
  // Convert pose estimates to ROS format and publish.
  Eigen::Matrix<double, 6, 6> covariance;
  covariance = Eigen::MatrixXd::Zero(6, 6);
  PublishPose(incremental_estimate_, covariance, incremental_estimate_pub_);
  PublishPose(integrated_estimate_, covariance, integrated_estimate_pub_);
}

void PointCloudLocalization::PublishConditionNumber(double& k, const ros::Publisher& pub) {
  // Convert condition number value to ROS format and publish.
  std_msgs::Float64 condition_number;
  condition_number.data = k;
  pub.publish(condition_number);
}

void PointCloudLocalization::UpdateTimestamp(ros::Time& stamp) {
  stamp_ = stamp;
}
