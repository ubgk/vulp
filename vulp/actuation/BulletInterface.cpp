// Copyright 2022 Stéphane Caron
// SPDX-License-Identifier: Apache-2.0

#include "vulp/actuation/BulletInterface.h"

#include <algorithm>
#include <iostream>
#include <memory>
#include <string>

#include "tools/cpp/runfiles/runfiles.h"
#include "vulp/actuation/bullet_utils.h"

using bazel::tools::cpp::runfiles::Runfiles;

namespace vulp::actuation {

std::string find_plane_urdf(const std::string argv0) {
  std::string error;
  std::unique_ptr<Runfiles> runfiles(Runfiles::Create(argv0, &error));
  if (runfiles == nullptr) {
    throw std::runtime_error(
        "Could not retrieve the package path to plane.urdf: " + error);
  }
  return runfiles->Rlocation("vulp/vulp/actuation/bullet/plane/plane.urdf");
}

BulletInterface::BulletInterface(const ServoLayout& layout,
                                 const Parameters& params)
    : Interface(layout), params_(params) {

  // Start simulator
  eCONNECT_METHOD flag;
  if (params.gui){
    if (params.server){
        std::cout << "Creating GUI server..." << "\n";
        flag = eCONNECT_GUI_SERVER;
    } else {
        std::cout << "Connecting to GUI" << "\n";
        flag = eCONNECT_GUI;
    }
  } else {
    if (params.server){
        std::cout << "Creating memory server" << "\n";
        flag = eCONNECT_SHARED_MEMORY_SERVER;
    } else {
        std::cout << "Connecting direct" << "\n";
        flag = eCONNECT_DIRECT;
    }
  }

  bool is_connected = bullet_.connect(flag);
  if (!is_connected) {
    throw std::runtime_error("Could not connect to the Bullet GUI");
  }

  // Setup simulation scene
  bullet_.configureDebugVisualizer(COV_ENABLE_GUI, 0);
  bullet_.configureDebugVisualizer(COV_ENABLE_RENDERING, 0);
  bullet_.configureDebugVisualizer(COV_ENABLE_SHADOWS, 0);
  if (params.gravity) {
    bullet_.setGravity(btVector3(0, 0, -9.81));
  }
  bullet_.setRealTimeSimulation(false);  // making sure

  // Load robot model
  robot_ = bullet_.loadURDF(params.robot_urdf_path);
  imu_link_index_ = find_link_index(bullet_, robot_, "imu");
  if (imu_link_index_ < 0) {
    throw std::runtime_error("Robot does not have a link named \"imu\"");
  }

  // Read servo layout
  for (const auto& id_joint : servo_joint_map()) {
    const auto& servo_id = id_joint.first;
    const std::string& joint_name = id_joint.second;
    moteus::ServoReply reply;
    reply.id = servo_id;
    joint_index_map_.try_emplace(joint_name, -1);
    servo_reply_.try_emplace(joint_name, reply);
  }

  // Map servo layout to Bullet
  b3JointInfo joint_info;
  const int nb_joints = bullet_.getNumJoints(robot_);
  for (int joint_index = 0; joint_index < nb_joints; ++joint_index) {
    bullet_.getJointInfo(robot_, joint_index, &joint_info);
    std::string joint_name = joint_info.m_jointName;
    if (joint_index_map_.find(joint_name) != joint_index_map_.end()) {
      joint_index_map_[joint_name] = joint_index;

      BulletJointProperties props;
      props.maximum_torque = joint_info.m_jointMaxForce;
      joint_properties_.try_emplace(joint_name, props);
    }
  }

  // Load environment URDFs
  for (const auto& urdf_path : params.env_urdf_paths) {
    spdlog::info("Loading environment URDF: ", urdf_path);
    if (bullet_.loadURDF(urdf_path) < 0) {
      throw std::runtime_error("Could not load the environment URDF: " +
                               urdf_path);
    }
  }

  // Start visualizer and configure simulation
  bullet_.configureDebugVisualizer(COV_ENABLE_RENDERING, 1);
  reset(Dictionary{});
}

BulletInterface::~BulletInterface() { bullet_.disconnect(); }

void BulletInterface::reset(const Dictionary& config) {
  params_.configure(config);
  bullet_.setTimeStep(params_.dt);
  reset_base_state(params_.position_base_in_world,
                   params_.orientation_base_in_world,
                   params_.linear_velocity_base_to_world_in_world,
                   params_.angular_velocity_base_in_base);
  reset_joint_angles();
  reset_joint_properties();
}

void BulletInterface::reset_base_state(
    const Eigen::Vector3d& position_base_in_world,
    const Eigen::Quaterniond& orientation_base_in_world,
    const Eigen::Vector3d& linear_velocity_base_to_world_in_world,
    const Eigen::Vector3d& angular_velocity_base_in_base) {
  bullet_.resetBasePositionAndOrientation(
      robot_, bullet_from_eigen(position_base_in_world),
      bullet_from_eigen(orientation_base_in_world));

  const auto& rotation_base_to_world =
      orientation_base_in_world;  // transforms are coordinates
  const Eigen::Vector3d angular_velocity_base_in_world =
      rotation_base_to_world * angular_velocity_base_in_base;
  bullet_.resetBaseVelocity(
      robot_, bullet_from_eigen(linear_velocity_base_to_world_in_world),
      bullet_from_eigen(angular_velocity_base_in_world));
}

void BulletInterface::reset_joint_angles() {
  const int nb_joints = bullet_.getNumJoints(robot_);
  for (int joint_index = 0; joint_index < nb_joints; ++joint_index) {
    bullet_.resetJointState(robot_, joint_index, 0.0);
  }
}

void BulletInterface::reset_joint_properties() {
  b3JointInfo joint_info;
  const int nb_joints = bullet_.getNumJoints(robot_);
  for (int joint_index = 0; joint_index < nb_joints; ++joint_index) {
    bullet_.getJointInfo(robot_, joint_index, &joint_info);
    std::string joint_name = joint_info.m_jointName;
    if (joint_index_map_.find(joint_name) != joint_index_map_.end()) {
      const auto friction_it = params_.joint_friction.find(joint_name);
      if (friction_it != params_.joint_friction.end()) {
        joint_properties_[joint_name].friction = friction_it->second;
      } else {
        joint_properties_[joint_name].friction = 0.0;
      }
    }
  }
}

void BulletInterface::cycle(
    const moteus::Data& data,
    std::function<void(const moteus::Output&)> callback) {
  assert(data.commands.size() == data.replies.size());
  assert(!std::isnan(params_.dt));

  read_joint_sensors();
  read_imu_data(imu_data_, bullet_, robot_, imu_link_index_, params_.dt);
  send_commands(data);
  bullet_.stepSimulation();

  if (params_.follower_camera) {
    translate_camera_to_robot();
  }

  moteus::Output output;
  for (size_t i = 0; i < data.replies.size(); ++i) {
    const auto servo_id = data.commands[i].id;
    const std::string& joint_name = servo_layout().joint_name(servo_id);
    data.replies[i].id = servo_id;
    data.replies[i].result = servo_reply_[joint_name].result;
    output.query_result_size = i + 1;
  }
  callback(output);
}

void BulletInterface::read_joint_sensors() {
  b3JointSensorState sensor_state;
  for (const auto& name_index : joint_index_map_) {
    const auto& joint_name = name_index.first;
    const auto joint_index = name_index.second;
    bullet_.getJointState(robot_, joint_index, &sensor_state);
    auto& result = servo_reply_[joint_name].result;
    result.position = sensor_state.m_jointPosition / (2.0 * M_PI);
    result.velocity = sensor_state.m_jointVelocity / (2.0 * M_PI);
    result.torque = sensor_state.m_jointMotorTorque;
  }
}

void BulletInterface::send_commands(const moteus::Data& data) {
  b3RobotSimulatorJointMotorArgs motor_args(CONTROL_MODE_VELOCITY);

  for (const auto& command : data.commands) {
    const auto servo_id = command.id;
    const std::string& joint_name = servo_layout().joint_name(servo_id);
    const int joint_index = joint_index_map_[joint_name];

    const auto previous_mode = servo_reply_[joint_name].result.mode;
    if (previous_mode == moteus::Mode::kStopped &&
        command.mode != moteus::Mode::kStopped) {
      // disable velocity controllers to enable torque control
      motor_args.m_controlMode = CONTROL_MODE_VELOCITY;
      motor_args.m_maxTorqueValue = 0.;  // [N m]
      bullet_.setJointMotorControl(robot_, joint_index, motor_args);
    }
    servo_reply_[joint_name].result.mode = command.mode;

    if (command.mode == moteus::Mode::kStopped) {
      motor_args.m_controlMode = CONTROL_MODE_VELOCITY;
      motor_args.m_maxTorqueValue = 100.;  // [N m]
      motor_args.m_targetVelocity = 0.;    // [rad] / [s]
      bullet_.setJointMotorControl(robot_, joint_index, motor_args);
      continue;
    }

    if (command.mode != moteus::Mode::kPosition) {
      throw std::runtime_error(
          "Bullet interface does not support command mode " +
          std::to_string(static_cast<unsigned>(command.mode)));
    }

    // TODO(scaron): introduce control_position/velocity intermediates
    // See https://github.com/mjbots/moteus/blob/main/docs/reference.md
    const double target_position = command.position.position * (2.0 * M_PI);
    const double target_velocity = command.position.velocity * (2.0 * M_PI);
    const double feedforward_torque = command.position.feedforward_torque;
    const double kp_scale = command.position.kp_scale;
    const double kd_scale = command.position.kd_scale;
    const double maximum_torque = command.position.maximum_torque;
    const double joint_torque = compute_joint_torque(
        joint_name, feedforward_torque, target_position, target_velocity,
        kp_scale, kd_scale, maximum_torque);
    motor_args.m_controlMode = CONTROL_MODE_TORQUE;
    motor_args.m_maxTorqueValue = joint_torque;
    servo_reply_[joint_name].result.torque = joint_torque;
    bullet_.setJointMotorControl(robot_, joint_index, motor_args);
  }
}

double BulletInterface::compute_joint_torque(
    const std::string& joint_name, const double feedforward_torque,
    const double target_position, const double target_velocity,
    const double kp_scale, const double kd_scale, const double maximum_torque) {
  assert(!std::isnan(target_velocity));
  const BulletJointProperties& joint_props = joint_properties_[joint_name];
  const auto& measurements = servo_reply_[joint_name].result;
  const double measured_position = measurements.position * (2.0 * M_PI);
  const double measured_velocity = measurements.velocity * (2.0 * M_PI);
  const double kp = kp_scale * params_.torque_control_kp;
  const double kd = kd_scale * params_.torque_control_kd;
  const double tau_max = std::min(maximum_torque, joint_props.maximum_torque);
  double torque = feedforward_torque;
  torque += kd * (target_velocity - measured_velocity);
  if (!std::isnan(target_position)) {
    torque += kp * (target_position - measured_position);
  }
  constexpr double kMaxStictionVelocity = 1e-3;  // rad/s
  if (std::abs(measured_velocity) > kMaxStictionVelocity) {
    torque += joint_props.friction * ((measured_velocity > 0.0) ? -1.0 : +1.0);
  }
  torque = std::max(std::min(torque, tau_max), -tau_max);
  return torque;
}

Eigen::Matrix4d BulletInterface::transform_base_to_world() const noexcept {
  btVector3 position_base_in_world;
  btQuaternion orientation_base_in_world;
  bullet_.getBasePositionAndOrientation(robot_, position_base_in_world,
                                        orientation_base_in_world);
  auto quat = eigen_from_bullet(orientation_base_in_world);
  Eigen::Matrix4d T = Eigen::Matrix4d::Identity();
  T.block<3, 3>(0, 0) = quat.normalized().toRotationMatrix();
  T.block<3, 1>(0, 3) = eigen_from_bullet(position_base_in_world);
  return T;
}

Eigen::Vector3d BulletInterface::linear_velocity_base_to_world_in_world()
    const noexcept {
  btVector3 linear_velocity_base_to_world_in_world;
  btVector3 _;  // anonymous, we only get the linear velocity
  bullet_.getBaseVelocity(robot_, linear_velocity_base_to_world_in_world, _);
  return eigen_from_bullet(linear_velocity_base_to_world_in_world);
}

Eigen::Vector3d BulletInterface::angular_velocity_base_in_base()
    const noexcept {
  btVector3 angular_velocity_base_to_world_in_world;
  btVector3 _;  // anonymous, we only get the angular velocity
  bullet_.getBaseVelocity(robot_, _, angular_velocity_base_to_world_in_world);
  Eigen::Matrix4d T = transform_base_to_world();
  Eigen::Matrix3d rotation_base_to_world = T.block<3, 3>(0, 0);
  return rotation_base_to_world.transpose() *
         eigen_from_bullet(angular_velocity_base_to_world_in_world);
}

void BulletInterface::translate_camera_to_robot() {
  b3OpenGLVisualizerCameraInfo camera_info;
  btVector3 position_base_in_world;
  btQuaternion orientation_base_in_world;
  bullet_.getBasePositionAndOrientation(robot_, position_base_in_world,
                                        orientation_base_in_world);
  bullet_.getDebugVisualizerCamera(&camera_info);
  bullet_.resetDebugVisualizerCamera(camera_info.m_dist, camera_info.m_pitch,
                                     camera_info.m_yaw, position_base_in_world);
}

}  // namespace vulp::actuation
