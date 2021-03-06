/*
 * Copyright 2017 The Cartographer Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "cartographer/mapping/halo_pose_extrapolator.h"

#include <algorithm>

#include "cartographer/common/make_unique.h"
#include "cartographer/transform/transform.h"
#include "glog/logging.h"
//james
#include "cartographer/mapping_3d/imu_integration.h"

namespace cartographer {
namespace mapping {

HaloPoseExtrapolator::HaloPoseExtrapolator(const common::Duration pose_queue_duration,
                                   double imu_gravity_time_constant)
    : pose_queue_duration_(pose_queue_duration),
    gravity_time_constant_(imu_gravity_time_constant),haloPoseState_(Eigen::Vector3d::Zero(),Eigen::Quaterniond::Identity(),Eigen::Vector3d::Zero()){}

std::unique_ptr<HaloPoseExtrapolator> HaloPoseExtrapolator::InitializeWithImu(
    const common::Duration pose_queue_duration,
    const double imu_gravity_time_constant, const sensor::ImuData& imu_data) {
  auto extrapolator = common::make_unique<HaloPoseExtrapolator>(
      pose_queue_duration, imu_gravity_time_constant);
  extrapolator->AddImuData(imu_data);
  extrapolator->imu_tracker_ =
      common::make_unique<ImuTracker>(imu_gravity_time_constant, imu_data.time);
  extrapolator->imu_tracker_->AddImuLinearAccelerationObservation(
      imu_data.linear_acceleration);
  extrapolator->imu_tracker_->AddImuAngularVelocityObservation(
      imu_data.angular_velocity);
  extrapolator->imu_tracker_->Advance(imu_data.time);
  extrapolator->AddPose(
      imu_data.time,
      transform::Rigid3d::Rotation(extrapolator->imu_tracker_->orientation()));
  //james
 /* extrapolator->AddHaloImuPose(
                          imu_data.time,
                          transform::Rigid3d::Rotation(extrapolator->imu_tracker_->orientation()));*/
  return extrapolator;
}

common::Time HaloPoseExtrapolator::GetLastPoseTime() const {
  if (timed_pose_queue_.empty()) {
    return common::Time::min();
  }
  return timed_pose_queue_.back().time;
}

void HaloPoseExtrapolator::AddPose(const common::Time time,
                               const transform::Rigid3d& pose) {
  if (imu_tracker_ == nullptr) {
    common::Time tracker_start = time;
    if (!imu_data_.empty()) {
      tracker_start = std::min(tracker_start, imu_data_.front().time);
    }
    imu_tracker_ =
        common::make_unique<ImuTracker>(gravity_time_constant_, tracker_start);
  }
  timed_pose_queue_.push_back(TimedPose{time, pose});
  while (timed_pose_queue_.size() > 2 &&
         timed_pose_queue_[1].time <= time - pose_queue_duration_) {
    timed_pose_queue_.pop_front();
  }
  UpdateVelocitiesFromPoses();
  AdvanceImuTracker(time, imu_tracker_.get());
  TrimImuData();
  TrimOdometryData();
}
//james ********************
template <typename T>
HaloPoseExtrapolator::IntegrateImuResult<T> HaloPoseExtrapolator::HaloIntegrateImu(
                                   const std::deque<sensor::ImuData>& imu_data,
                                   const common::Time start_time, const common::Time end_time,
                                   std::deque<sensor::ImuData>::const_iterator* it)
{

    const Eigen::Transform<T, 3, Eigen::Affine> linear_acceleration_calibration( Eigen::Affine3d::Identity());
    const Eigen::Transform<T, 3, Eigen::Affine> angular_velocity_calibration(Eigen::Affine3d::Identity());
    
    CHECK_LE(start_time, end_time);
    CHECK(*it != imu_data.cend());
    CHECK_LE((*it)->time, start_time);
    if ((*it) + 1 != imu_data.cend()) {
        CHECK_GT(((*it) + 1)->time, start_time);
    }
    
    common::Time current_time = start_time;
    
    IntegrateImuResult<T> result = {Eigen::Matrix<T, 3, 1>::Zero(),
        Eigen::Quaterniond::Identity().cast<T>()};
    while (current_time < end_time) {
        common::Time next_imu_data = common::Time::max();
        if ((*it) + 1 != imu_data.cend()) {
            next_imu_data = ((*it) + 1)->time;
        }
        common::Time next_time = std::min(next_imu_data, end_time);
        const T delta_t(common::ToSeconds(next_time - current_time));
        
        const Eigen::Matrix<T, 3, 1> delta_angle =
        (angular_velocity_calibration * (*it)->angular_velocity.cast<T>()) * delta_t;
        
        result.delta_rotation *= transform::AngleAxisVectorToRotationQuaternion(delta_angle);
       
        result.delta_velocity += result.delta_rotation * ((linear_acceleration_calibration *
                                  (*it)->linear_acceleration.cast<T>()) *delta_t);
        current_time = next_time;
        
        if (current_time == next_imu_data) {
            ++(*it);
        }
    }
    return result;
}

transform::Rigid3d HaloPoseExtrapolator::GetHaloPose( common::Time time)
{
    if(imu_data_.size() > 1)
    {
        haloPoseState_ = PredictState(haloPoseState_,haloTime_,time);
        haloTime_ = time;
    }
    Eigen::Quaterniond r(haloPoseState_.rotation[0],haloPoseState_.rotation[1], haloPoseState_.rotation[2],haloPoseState_.rotation[3]);
    Eigen::Vector3d p = Eigen::Map<const Eigen::Vector3d>(haloPoseState_.translation.data());
    return transform::Rigid3d(p,r);
}
    

HaloPoseExtrapolator::State HaloPoseExtrapolator::PredictState(const State& start_state,
                                               const common::Time start_time,
                                               const common::Time end_time) {
    auto it = --imu_data_.cend();
    while (it->time > start_time) {
        CHECK(it != imu_data_.cbegin());
        --it;
    }
   const Eigen::Vector3d start_velocity = Eigen::Map<const Eigen::Vector3d>(start_state.velocity.data());
   const Eigen::Vector3d start_position =  Eigen::Map<const Eigen::Vector3d>(start_state.translation.data());
    const Eigen::Quaterniond start_rotation(start_state.rotation[0], start_state.rotation[1], start_state.rotation[2],
                                            start_state.rotation[3]);

    
    const IntegrateImuResult<double> result = HaloIntegrateImu<double>(imu_data_, start_time, end_time, &it);
    /*
    std::cout << " james:PredictState:"  << "start_position:" << transform::Rigid3d::Translation(start_position) << " start_rotation:" << transform::Rigid3d::Rotation(start_rotation) << " velocity:" <<  transform::Rigid3d::Translation( result.delta_velocity) << " rotation:" << transform::Rigid3d::Rotation(result.delta_rotation)  << std::endl;*/
    
    const Eigen::Quaterniond orientation = start_rotation * result.delta_rotation;
    const double delta_time_seconds = common::ToSeconds(end_time - start_time);
    
    // TODO(hrapp): IntegrateImu should integration position as well.
    const Eigen::Vector3d position = start_position +
    delta_time_seconds * Eigen::Map<const Eigen::Vector3d>(start_state.velocity.data());
    
    const Eigen::Vector3d gravity_velocity = imu_tracker_->gravity_velocity();
    const Eigen::Vector3d velocity = start_velocity +  start_rotation*result.delta_velocity
                                                                    - gravity_velocity;
    /*
    const Eigen::Vector3d velocity =
    Eigen::Map<const Eigen::Vector3d>(start_state.velocity.data()) + start_rotation * result.delta_velocity -
    9.8 * delta_time_seconds * Eigen::Vector3d::UnitZ();
    */
  
    
    std::cout << "james:PredictState:"  << " time:" <<end_time  << " pose:" << transform::Rigid3d(position,orientation) << " velocity:" << transform::Rigid3d(velocity,result.delta_rotation) << std::endl;
    
    return State(position, orientation, velocity);
}
/*
void HaloPoseExtrapolator::AddHaloImuPose(const common::Time time,
                               const transform::Rigid3d& pose) {
    if (imu_tracker_ == nullptr) {
        common::Time tracker_start = time;
        if (!imu_data_.empty()) {
            tracker_start = std::min(tracker_start, imu_data_.front().time);
        }
        imu_tracker_ =
        common::make_unique<ImuTracker>(gravity_time_constant_, tracker_start);
    }
  
    halo_timed_pose_queue_.push_back(TimedPose{time, pose});
    while (halo_timed_pose_queue_.size() > 2 &&
           halo_timed_pose_queue_[1].time <= time - pose_queue_duration_) {
        halo_timed_pose_queue_.pop_front();
    }
   
    UpdateVelocitiesFromPoses();
    AdvanceImuTracker(time, imu_tracker_.get());
    TrimImuData();
    TrimOdometryData();
}
    
transform::Rigid3d HaloPoseExtrapolator::ExtrapolateHaloImuPose(const common::Time time)
{
    // TODO(whess): Keep the last extrapolated pose.
    const TimedPose& newest_timed_pose = halo_timed_pose_queue_.back();
    CHECK_GE(time, newest_timed_pose.time);
    return transform::Rigid3d::Translation(ExtrapolateTranslation(time)) *
    newest_timed_pose.pose *
    transform::Rigid3d::Rotation(ExtrapolateRotation(time));
}*/
    
///////////////////////
void HaloPoseExtrapolator::AddImuData(const sensor::ImuData& imu_data)
{
  CHECK(timed_pose_queue_.empty() ||
        imu_data.time >= timed_pose_queue_.back().time);
  if(imu_data_.empty())
      haloTime_ = imu_data.time;
    
  imu_data_.push_back(imu_data);
  TrimImuData();
}

void HaloPoseExtrapolator::AddOdometryData(
    const sensor::OdometryData& odometry_data) {
  CHECK(timed_pose_queue_.empty() ||
        odometry_data.time >= timed_pose_queue_.back().time);
  odometry_data_.push_back(odometry_data);
  TrimOdometryData();
  if (odometry_data_.size() < 2) {
    return;
  }
  // TODO(whess): Improve by using more than just the last two odometry poses.
  // Compute extrapolation in the tracking frame.
  const sensor::OdometryData& odometry_data_oldest =
      odometry_data_.front();
  const sensor::OdometryData& odometry_data_newest =
      odometry_data_.back();
  const double odometry_time_delta =
      common::ToSeconds(odometry_data_oldest.time - odometry_data_newest.time);
  const transform::Rigid3d odometry_pose_delta =
      odometry_data_newest.pose.inverse() * odometry_data_oldest.pose;
  angular_velocity_from_odometry_ =
      transform::RotationQuaternionToAngleAxisVector(
          odometry_pose_delta.rotation()) /
      odometry_time_delta;
  if (timed_pose_queue_.empty()) {
    return;
  }
  const Eigen::Vector3d
      linear_velocity_in_tracking_frame_at_newest_odometry_time =
          odometry_pose_delta.translation() / odometry_time_delta;
  const Eigen::Quaterniond orientation_at_newest_odometry_time =
      timed_pose_queue_.back().pose.rotation() *
      ExtrapolateRotation(odometry_data_newest.time);
  linear_velocity_from_odometry_ =
      orientation_at_newest_odometry_time *
      linear_velocity_in_tracking_frame_at_newest_odometry_time;
}

transform::Rigid3d HaloPoseExtrapolator::ExtrapolatePose(const common::Time time) {
  // TODO(whess): Keep the last extrapolated pose.
  const TimedPose& newest_timed_pose = timed_pose_queue_.back();
  CHECK_GE(time, newest_timed_pose.time);
  return transform::Rigid3d::Translation(ExtrapolateTranslation(time)) *
         newest_timed_pose.pose *
         transform::Rigid3d::Rotation(ExtrapolateRotation(time));
}

Eigen::Quaterniond HaloPoseExtrapolator::EstimateGravityOrientation(
    const common::Time time) {
  ImuTracker imu_tracker = *imu_tracker_;
  AdvanceImuTracker(time, &imu_tracker);
  return imu_tracker.orientation();
}

void HaloPoseExtrapolator::UpdateVelocitiesFromPoses() {
  if (timed_pose_queue_.size() < 2) {
    // We need two poses to estimate velocities.
    return;
  }
  CHECK(!timed_pose_queue_.empty());
  const TimedPose& newest_timed_pose = timed_pose_queue_.back();
  const auto newest_time = newest_timed_pose.time;
  const TimedPose& oldest_timed_pose = timed_pose_queue_.front();
  const auto oldest_time = oldest_timed_pose.time;
  const double queue_delta = common::ToSeconds(newest_time - oldest_time);
  if (queue_delta < 0.001) {  // 1 ms
    LOG(WARNING) << "Queue too short for velocity estimation. Queue duration: "
                 << queue_delta << " ms";
    return;
  }
  const transform::Rigid3d& newest_pose = newest_timed_pose.pose;
  const transform::Rigid3d& oldest_pose = oldest_timed_pose.pose;
  linear_velocity_from_poses_ =
      (newest_pose.translation() - oldest_pose.translation()) / queue_delta;
  angular_velocity_from_poses_ =
      transform::RotationQuaternionToAngleAxisVector(
          oldest_pose.rotation().inverse() * newest_pose.rotation()) /
      queue_delta;
}

void HaloPoseExtrapolator::TrimImuData() {
  while (imu_data_.size() > 1 && !timed_pose_queue_.empty() &&
         imu_data_[1].time <= timed_pose_queue_.back().time) {
    imu_data_.pop_front();
  }
}

void HaloPoseExtrapolator::TrimOdometryData() {
  while (odometry_data_.size() > 2 && !timed_pose_queue_.empty() &&
         odometry_data_[1].time <= timed_pose_queue_.back().time) {
    odometry_data_.pop_front();
  }
}

void HaloPoseExtrapolator::AdvanceImuTracker(const common::Time time,
                                         ImuTracker* const imu_tracker) {
  CHECK_GE(time, imu_tracker->time());
  if (imu_data_.empty() || time < imu_data_.front().time) {
    // There is no IMU data until 'time', so we advance the ImuTracker and use
    // the angular velocities from poses and fake gravity to help 2D stability.
    imu_tracker->Advance(time);
    imu_tracker->AddImuLinearAccelerationObservation(Eigen::Vector3d::UnitZ());
    imu_tracker->AddImuAngularVelocityObservation(
        odometry_data_.size() < 2 ? angular_velocity_from_poses_
                                  : angular_velocity_from_odometry_);
    return;
  }
  if (imu_tracker->time() < imu_data_.front().time) {
    // Advance to the beginning of 'imu_data_'.
    imu_tracker->Advance(imu_data_.front().time);
  }
  auto it = std::lower_bound(
      imu_data_.begin(), imu_data_.end(), imu_tracker->time(),
      [](const sensor::ImuData& imu_data, const common::Time& time) {
        return imu_data.time < time;
      });
  while (it != imu_data_.end() && it->time < time) {
    imu_tracker->Advance(it->time);
    imu_tracker->AddImuLinearAccelerationObservation(it->linear_acceleration);
    imu_tracker->AddImuAngularVelocityObservation(it->angular_velocity);
    ++it;
  }
  imu_tracker->Advance(time);
}

Eigen::Quaterniond HaloPoseExtrapolator::ExtrapolateRotation(
    const common::Time time) {
  ImuTracker imu_tracker = *imu_tracker_;
  AdvanceImuTracker(time, &imu_tracker);
  const Eigen::Quaterniond last_orientation = imu_tracker_->orientation();
   const Eigen::Quaterniond orientationn = imu_tracker.orientation();
  //  std::cout << "james ExtrapolateRotation : last_orientation:" << transform::Rigid3d::Rotation(last_orientation) <<
   // " orientationn:" << transform::Rigid3d::Rotation(orientationn) << std::endl;
  return last_orientation.inverse() * orientationn;
}

Eigen::Vector3d HaloPoseExtrapolator::ExtrapolateTranslation(common::Time time) {
  const TimedPose& newest_timed_pose = timed_pose_queue_.back();
  const double extrapolation_delta =
      common::ToSeconds(time - newest_timed_pose.time);
  if (odometry_data_.size() < 2) {
    return extrapolation_delta * linear_velocity_from_poses_;
  }
  return extrapolation_delta * linear_velocity_from_odometry_;
}

}  // namespace mapping
}  // namespace cartographer
