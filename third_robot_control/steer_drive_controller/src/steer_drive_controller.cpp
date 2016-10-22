/*********************************************************************
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2013, PAL Robotics, S.L.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of the PAL Robotics nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *********************************************************************/

/*
 * Author: Bence Magyar
 */

#include <cmath>

#include <tf/transform_datatypes.h>

#include <urdf_parser/urdf_parser.h>

#include <boost/assign.hpp>

#include <steer_drive_controller/steer_drive_controller.h>

static double euclideanOfVectors(const urdf::Vector3& vec1, const urdf::Vector3& vec2)
{
  return std::sqrt(std::pow(vec1.x-vec2.x,2) +
                   std::pow(vec1.y-vec2.y,2) +
                   std::pow(vec1.z-vec2.z,2));
}

/*
 * \brief Check if the link is modeled as a cylinder
 * \param link Link
 * \return true if the link is modeled as a Cylinder; false otherwise
 */
static bool isCylinder(const boost::shared_ptr<const urdf::Link>& link)
{
  if (!link)
  {
    ROS_ERROR("Link == NULL.");
    return false;
  }

  if (!link->collision)
  {
    ROS_ERROR_STREAM("Link " << link->name << " does not have collision description. Add collision description for link to urdf.");
    return false;
  }

  if (!link->collision->geometry)
  {
    ROS_ERROR_STREAM("Link " << link->name << " does not have collision geometry description. Add collision geometry description for link to urdf.");
    return false;
  }

  if (link->collision->geometry->type != urdf::Geometry::CYLINDER)
  {
    ROS_ERROR_STREAM("Link " << link->name << " does not have cylinder geometry");
    return false;
  }

  return true;
}

/*
 * \brief Get the wheel radius
 * \param [in]  wheel_link   Wheel link
 * \param [out] wheel_radius Wheel radius [m]
 * \return true if the wheel radius was found; false otherwise
 */
static bool getWheelRadius(const boost::shared_ptr<const urdf::Link>& wheel_link, double& wheel_radius)
{
  if (!isCylinder(wheel_link))
  {
    ROS_ERROR_STREAM("Wheel link " << wheel_link->name << " is NOT modeled as a cylinder!");
    return false;
  }

  wheel_radius = (static_cast<urdf::Cylinder*>(wheel_link->collision->geometry.get()))->radius;
  return true;
}

namespace steer_drive_controller{

  SteerDriveController::SteerDriveController()
    : open_loop_(false)
    , command_struct_()
    , wheel_separation_w_(0.0)
    , wheel_separation_h_(0.0)
    , wheel_radius_(0.0)
    , wheel_separation_multiplier_(1.0)
    , wheel_radius_multiplier_(1.0)
    , linear_vel_multiplier_(1.0)
    , angle_vel_multiplier_(1.0)
    , cmd_vel_timeout_(0.5)
    , allow_multiple_cmd_vel_publishers_(true)
    , base_frame_id_("base_link")
    , odom_frame_id_("odom")
    , enable_odom_tf_(true)
    , wheel_joints_size_(0)
  #ifdef GUI_DEBUG
    , ns_("steer_drive_controller/")
    , isCmdVelImput_(false)
  #else
    , ns_("")
  #endif
  {
  }

  bool SteerDriveController::init(hardware_interface::RobotHW* robot_hw,
                                   ros::NodeHandle& root_nh,
                                   ros::NodeHandle& controller_nh)
  {
    typedef hardware_interface::VelocityJointInterface VelIface;
    typedef hardware_interface::PositionJointInterface PosIface;
    typedef hardware_interface::JointStateInterface StateIface;

    // get multiple types of hardware_interface
    VelIface *vel_joint_if = robot_hw->get<VelIface>(); // vel for wheels
    PosIface *pos_joint_if = robot_hw->get<PosIface>(); // pos for steers

    const std::string complete_ns = controller_nh.getNamespace();

    std::size_t id = complete_ns.find_last_of("/");
    name_ = complete_ns.substr(id + 1);

    // Get joint names from the parameter server
    //-- wheels
    std::vector<std::string> rear_wheel_names, front_wheel_names, front_steer_names;
    if (!getWheelNames(controller_nh, ns_ + "rear_wheels", rear_wheel_names) or
        !getWheelNames(controller_nh, ns_ + "front_wheels", front_wheel_names) or
        !getWheelNames(controller_nh, ns_ + "front_steers", front_steer_names))
    {
      return false;
    }

    if (rear_wheel_names.size() != front_wheel_names.size())
    {
      ROS_ERROR_STREAM_NAMED(name_,
          "#rear wheels (" << rear_wheel_names.size() << ") != " <<
          "#front wheels (" << front_wheel_names.size() << ").");
      return false;
    }
    else
    {
      wheel_joints_size_ = rear_wheel_names.size();

      rear_wheel_joints_.resize(wheel_joints_size_);
      front_wheel_joints_.resize(wheel_joints_size_);
    }

    //-- single rear drive
    std::string wheel_name = "wheel_joint";
    controller_nh.param(ns_ + "rear_wheel", wheel_name, wheel_name);

    //-- single front steer drive
    std::string steer_name = "steer_joint";
    controller_nh.param(ns_ + "front_steer", steer_name, steer_name);


    // Odometry related:
    double publish_rate;
    controller_nh.param("publish_rate", publish_rate, 50.0);
    ROS_INFO_STREAM_NAMED(name_, "Controller state will be published at "
                          << publish_rate << "Hz.");
    publish_period_ = ros::Duration(1.0 / publish_rate);

    controller_nh.param(ns_ + "open_loop", open_loop_, open_loop_);

    controller_nh.param(ns_ + "wheel_separation_multiplier", wheel_separation_multiplier_, wheel_separation_multiplier_);
    ROS_INFO_STREAM_NAMED(name_, "Wheel separation will be multiplied by "
                          << wheel_separation_multiplier_ << ".");

    controller_nh.param(ns_ + "wheel_radius_multiplier", wheel_radius_multiplier_, wheel_radius_multiplier_);
    ROS_INFO_STREAM_NAMED(name_, "Wheel radius will be multiplied by "
                          << wheel_radius_multiplier_ << ".");

    controller_nh.param(ns_ + "linear_vel_multiplier", linear_vel_multiplier_, linear_vel_multiplier_);
    ROS_INFO_STREAM_NAMED(name_, "linear velocity will be multiplied by "
                          << linear_vel_multiplier_ << ".");

    controller_nh.param(ns_ + "angle_vel_multiplier", angle_vel_multiplier_, angle_vel_multiplier_);
    ROS_INFO_STREAM_NAMED(name_, "angle velocity will be multiplied by "
                          << angle_vel_multiplier_ << ".");

    int velocity_rolling_window_size = 10;
    controller_nh.param(ns_ + "velocity_rolling_window_size", velocity_rolling_window_size, velocity_rolling_window_size);
    ROS_INFO_STREAM_NAMED(name_, "Velocity rolling window size of "
                          << velocity_rolling_window_size << ".");

    odometry_.setVelocityRollingWindowSize(velocity_rolling_window_size);

    // Twist command related:
    controller_nh.param(ns_ + "cmd_vel_timeout", cmd_vel_timeout_, cmd_vel_timeout_);
    ROS_INFO_STREAM_NAMED(name_, "Velocity commands will be considered old if they are older than "
                          << cmd_vel_timeout_ << "s.");

    controller_nh.param(ns_ + "allow_multiple_cmd_vel_publishers", allow_multiple_cmd_vel_publishers_, allow_multiple_cmd_vel_publishers_);
    ROS_INFO_STREAM_NAMED(name_, "Allow mutiple cmd_vel publishers is "
                          << (allow_multiple_cmd_vel_publishers_?"enabled":"disabled"));

    controller_nh.param(ns_ + "base_frame_id", base_frame_id_, base_frame_id_);
    ROS_INFO_STREAM_NAMED(name_, "Base frame_id set to " << base_frame_id_);

    controller_nh.param(ns_ + "odom_frame_id", odom_frame_id_, odom_frame_id_);
    ROS_INFO_STREAM_NAMED(name_, "Odometry frame_id set to " << odom_frame_id_);

    controller_nh.param(ns_ + "enable_odom_tf", enable_odom_tf_, enable_odom_tf_);
    ROS_INFO_STREAM_NAMED(name_, "Publishing to tf is " << (enable_odom_tf_?"enabled":"disabled"));

    // Velocity and acceleration limits:
    controller_nh.param(ns_ + "linear/x/has_velocity_limits"    , limiter_lin_.has_velocity_limits    , limiter_lin_.has_velocity_limits    );
    controller_nh.param(ns_ + "linear/x/has_acceleration_limits", limiter_lin_.has_acceleration_limits, limiter_lin_.has_acceleration_limits);
    controller_nh.param(ns_ + "linear/x/has_jerk_limits"        , limiter_lin_.has_jerk_limits        , limiter_lin_.has_jerk_limits        );
    controller_nh.param(ns_ + "linear/x/max_velocity"           , limiter_lin_.max_velocity           ,  limiter_lin_.max_velocity          );
    controller_nh.param(ns_ + "linear/x/min_velocity"           , limiter_lin_.min_velocity           , -limiter_lin_.max_velocity          );
    controller_nh.param(ns_ + "linear/x/max_acceleration"       , limiter_lin_.max_acceleration       ,  limiter_lin_.max_acceleration      );
    controller_nh.param(ns_ + "linear/x/min_acceleration"       , limiter_lin_.min_acceleration       , -limiter_lin_.max_acceleration      );
    controller_nh.param(ns_ + "linear/x/max_jerk"               , limiter_lin_.max_jerk               ,  limiter_lin_.max_jerk              );
    controller_nh.param(ns_ + "linear/x/min_jerk"               , limiter_lin_.min_jerk               , -limiter_lin_.max_jerk              );

    controller_nh.param(ns_ + "angular/z/has_velocity_limits"    , limiter_ang_.has_velocity_limits    , limiter_ang_.has_velocity_limits    );
    controller_nh.param(ns_ + "angular/z/has_acceleration_limits", limiter_ang_.has_acceleration_limits, limiter_ang_.has_acceleration_limits);
    controller_nh.param(ns_ + "angular/z/has_jerk_limits"        , limiter_ang_.has_jerk_limits        , limiter_ang_.has_jerk_limits        );
    controller_nh.param(ns_ + "angular/z/max_velocity"           , limiter_ang_.max_velocity           ,  limiter_ang_.max_velocity          );
    controller_nh.param(ns_ + "angular/z/min_velocity"           , limiter_ang_.min_velocity           , -limiter_ang_.max_velocity          );
    controller_nh.param(ns_ + "angular/z/max_acceleration"       , limiter_ang_.max_acceleration       ,  limiter_ang_.max_acceleration      );
    controller_nh.param(ns_ + "angular/z/min_acceleration"       , limiter_ang_.min_acceleration       , -limiter_ang_.max_acceleration      );
    controller_nh.param(ns_ + "angular/z/max_jerk"               , limiter_ang_.max_jerk               ,  limiter_ang_.max_jerk              );
    controller_nh.param(ns_ + "angular/z/min_jerk"               , limiter_ang_.min_jerk               , -limiter_ang_.max_jerk              );

    // If either parameter is not available, we need to look up the value in the URDF
    bool lookup_wheel_separation_w = !controller_nh.getParam(ns_ + "wheel_separation_w", wheel_separation_w_);
    bool lookup_wheel_separation_h = !controller_nh.getParam(ns_ + "wheel_separation_h", wheel_separation_h_);
    bool lookup_wheel_radius = !controller_nh.getParam(ns_ + "wheel_radius", wheel_radius_);

    if (!setOdomParamsFromUrdf(root_nh,
                               rear_wheel_names,
                               front_wheel_names,
                               front_steer_names,
                               lookup_wheel_separation_w,
                               lookup_wheel_separation_h,
                               lookup_wheel_radius))
    {
      return false;
    }

    // Regardless of how we got the separation and radius, use them
    // to set the odometry parameters
    const double ws_w = wheel_separation_multiplier_ * wheel_separation_w_;
    const double ws_h = wheel_separation_multiplier_ * wheel_separation_h_;
    const double wr = wheel_radius_multiplier_     * wheel_radius_;
    odometry_.setWheelParams(ws_w, ws_h, wr);
    ROS_INFO_STREAM_NAMED(name_,
                          "Odometry params : wheel separation width " << ws_w
                          << ", wheel separation height " << ws_h
                          << ", wheel radius " << wr);

    setOdomPubFields(root_nh, controller_nh);

    // Get the joint object to use in the realtime loop
    //-- wheels
    for (int i = 0; i < wheel_joints_size_; ++i)
    {
      ROS_INFO_STREAM_NAMED(name_,
                            "Adding left wheel with joint name: " << rear_wheel_names[i]
                            << " and right wheel with joint name: " << front_wheel_names[i]);

      rear_wheel_joints_[i] = vel_joint_if->getHandle(rear_wheel_names[i]);  // throws on failure
      front_wheel_joints_[i] = vel_joint_if->getHandle(front_wheel_names[i]);  // throws on failure
    }
    //-- rear wheel
    //---- handles need to be previously registerd in steer_drive_test.cpp
    ROS_INFO_STREAM_NAMED(name_,
                          "Adding the wheel with joint name: " << wheel_name);
    wheel_joint_ = vel_joint_if->getHandle(wheel_name); // throws on failure
    //-- front steer
    ROS_INFO_STREAM_NAMED(name_,
                          "Adding the steer with joint name: " << steer_name);
    steer_joint_ = pos_joint_if->getHandle(steer_name); // throws on failure
    ROS_INFO_STREAM_NAMED(name_,
                          "Adding the subscriber: cmd_vel");
    sub_command_ = controller_nh.subscribe("cmd_vel", 1, &SteerDriveController::cmdVelCallback, this);
    ROS_INFO_STREAM_NAMED(name_, "Finished controller initialization");

    return true;
  }

  void SteerDriveController::update(const ros::Time& time, const ros::Duration& period)
  {
    // COMPUTE AND PUBLISH ODOMETRY
    if (open_loop_)
    {
      odometry_.updateOpenLoop(last0_cmd_.lin, last0_cmd_.ang, time);
    }
    else
    {
      double left_pos  = rear_wheel_joints_[INDEX_LEFT].getPosition();
      double right_pos = rear_wheel_joints_[INDEX_RIGHT].getPosition();
      double steer_pos = steer_joint_.getPosition();

      // Estimate linear and angular velocity using joint information
      odometry_.update(left_pos, right_pos, steer_pos, time);
    }

    // Publish odometry message
    if (last_state_publish_time_ + publish_period_ < time)
    {
      last_state_publish_time_ += publish_period_;
      // Compute and store orientation info
      const geometry_msgs::Quaternion orientation(
            tf::createQuaternionMsgFromYaw(odometry_.getHeading()));

      // Populate odom message and publish
      if (odom_pub_->trylock())
      {
        odom_pub_->msg_.header.stamp = time;
        odom_pub_->msg_.pose.pose.position.x = odometry_.getX();
        odom_pub_->msg_.pose.pose.position.y = odometry_.getY();
        odom_pub_->msg_.pose.pose.orientation = orientation;
        odom_pub_->msg_.twist.twist.linear.x  = odometry_.getLinear();
        odom_pub_->msg_.twist.twist.angular.z = odometry_.getAngular();
        odom_pub_->unlockAndPublish();
      }

      // Publish tf /odom frame
      if (enable_odom_tf_ && tf_odom_pub_->trylock())
      {
        geometry_msgs::TransformStamped& odom_frame = tf_odom_pub_->msg_.transforms[0];
        odom_frame.header.stamp = time;
        odom_frame.transform.translation.x = odometry_.getX();
        odom_frame.transform.translation.y = odometry_.getY();
        odom_frame.transform.rotation = orientation;
        tf_odom_pub_->unlockAndPublish();
      }
    }

    // MOVE ROBOT
    // Retreive current velocity command and time step:
    Commands curr_cmd = *(command_.readFromRT());
    const double dt = (time - curr_cmd.stamp).toSec();

    // Brake if cmd_vel has timeout:
    if (dt > cmd_vel_timeout_)
    {
      curr_cmd.lin = 0.0;
      curr_cmd.ang = 0.0;
    }

    // Limit velocities and accelerations:
    const double cmd_dt(period.toSec());

    limiter_lin_.limit(curr_cmd.lin, last0_cmd_.lin, last1_cmd_.lin, cmd_dt);
    limiter_ang_.limit(curr_cmd.ang, last0_cmd_.ang, last1_cmd_.ang, cmd_dt);

    last1_cmd_ = last0_cmd_;
    last0_cmd_ = curr_cmd;

    // Apply multipliers:
    /*
    const double ws_w = wheel_separation_multiplier_ * wheel_separation_w_;
    const double ws_h = wheel_separation_multiplier_ * wheel_separation_h_;
    const double wr = wheel_radius_multiplier_     * wheel_radius_;
    */

    // Set Command
    const double wheel_vel = curr_cmd.lin/wheel_radius_*linear_vel_multiplier_; // omega = linear_vel / radius
    wheel_joint_.setCommand(wheel_vel);
    steer_joint_.setCommand(curr_cmd.ang*angle_vel_multiplier_);

  }

  void SteerDriveController::starting(const ros::Time& time)
  {
    brake();

    // Register starting time used to keep fixed rate
    last_state_publish_time_ = time;

    odometry_.init(time);
  }

  void SteerDriveController::stopping(const ros::Time& /*time*/)
  {
    brake();
  }

  void SteerDriveController::brake()
  {
    const double steer_pos = 0.0;
    const double wheel_vel = 0.0;

    wheel_joint_.setCommand(steer_pos);
    steer_joint_.setCommand(wheel_vel);
  }

  void SteerDriveController::cmdVelCallback(const geometry_msgs::Twist& command)
  {
#ifdef GUI_DEBUG
    if(true)
#else
    if (isRunning())
#endif
    {
      // check that we don't have multiple publishers on the command topic
      if (!allow_multiple_cmd_vel_publishers_ && sub_command_.getNumPublishers() > 1)
      {
        ROS_ERROR_STREAM_THROTTLE_NAMED(1.0, name_, "Detected " << sub_command_.getNumPublishers()
            << " publishers. Only 1 publisher is allowed. Going to brake.");
        brake();
        return;
      }

#ifdef GUI_DEBUG
      if(command.angular.z != 0)
      {
          isCmdVelImput_ = true;
      }
      else if(command.linear.x > 0.3)
      {
          isCmdVelImput_ = true;
      }
      else
      {
          isCmdVelImput_ = false;
      }
#endif
      command_struct_.ang   = command.angular.z;
      command_struct_.lin   = command.linear.x;
      command_struct_.stamp = ros::Time::now();
      command_.writeFromNonRT (command_struct_);
      ROS_DEBUG_STREAM_NAMED(name_,
                             "Added values to command. "
                             << "Ang: "   << command_struct_.ang << ", "
                             << "Lin: "   << command_struct_.lin << ", "
                             << "Stamp: " << command_struct_.stamp);
    }
    else
    {
      ROS_ERROR_NAMED(name_, "Can't accept new commands. Controller is not running.");
    }
  }

  bool SteerDriveController::getWheelNames(ros::NodeHandle& controller_nh,
                              const std::string& wheel_param,
                              std::vector<std::string>& wheel_names)
  {
      XmlRpc::XmlRpcValue wheel_list;
      if (!controller_nh.getParam(wheel_param, wheel_list))
      {
        ROS_ERROR_STREAM_NAMED(name_,
            "Couldn't retrieve wheel param '" << wheel_param << "'.");
        return false;
      }

      if (wheel_list.getType() == XmlRpc::XmlRpcValue::TypeArray)
      {
        if (wheel_list.size() == 0)
        {
          ROS_ERROR_STREAM_NAMED(name_,
              "Wheel param '" << wheel_param << "' is an empty list");
          return false;
        }

        for (int i = 0; i < wheel_list.size(); ++i)
        {
          if (wheel_list[i].getType() != XmlRpc::XmlRpcValue::TypeString)
          {
            ROS_ERROR_STREAM_NAMED(name_,
                "Wheel param '" << wheel_param << "' #" << i <<
                " isn't a string.");
            return false;
          }
        }

        wheel_names.resize(wheel_list.size());
        for (int i = 0; i < wheel_list.size(); ++i)
        {
          wheel_names[i] = static_cast<std::string>(wheel_list[i]);
        }
      }
      else if (wheel_list.getType() == XmlRpc::XmlRpcValue::TypeString)
      {
        wheel_names.push_back(wheel_list);
      }
      else
      {
        ROS_ERROR_STREAM_NAMED(name_,
            "Wheel param '" << wheel_param <<
            "' is neither a list of strings nor a string.");
        return false;
      }

      return true;
  }

  bool SteerDriveController::getSteerNames(ros::NodeHandle& controller_nh,
                              const std::string& steer_param,
                              std::vector<std::string>& steer_names)
  {
      XmlRpc::XmlRpcValue steer_list;
      if (!controller_nh.getParam(steer_param, steer_list))
      {
        ROS_ERROR_STREAM_NAMED(name_,
            "Couldn't retrieve steer param '" << steer_param << "'.");
        return false;
      }

      if (steer_list.getType() == XmlRpc::XmlRpcValue::TypeArray)
      {
        if (steer_list.size() == 0)
        {
          ROS_ERROR_STREAM_NAMED(name_,
              "Steer param '" << steer_param << "' is an empty list");
          return false;
        }

        for (int i = 0; i < steer_list.size(); ++i)
        {
          if (steer_list[i].getType() != XmlRpc::XmlRpcValue::TypeString)
          {
            ROS_ERROR_STREAM_NAMED(name_,
                "Steer param '" << steer_param << "' #" << i <<
                " isn't a string.");
            return false;
          }
        }

        steer_names.resize(steer_list.size());
        for (int i = 0; i < steer_list.size(); ++i)
        {
          steer_names[i] = static_cast<std::string>(steer_list[i]);
        }
      }
      else if (steer_list.getType() == XmlRpc::XmlRpcValue::TypeString)
      {
        steer_names.push_back(steer_list);
      }
      else
      {
        ROS_ERROR_STREAM_NAMED(name_,
            "Steer param '" << steer_param <<
            "' is neither a list of strings nor a string.");
        return false;
      }

      return true;
  }


  bool SteerDriveController::setOdomParamsFromUrdf(ros::NodeHandle& root_nh,
                             const std::vector<std::string>& rear_wheel_names,
                             const std::vector<std::string>& front_wheel_names,
                             const std::vector<std::string>& front_steer_names,
                             bool lookup_wheel_separation_w,
                             bool lookup_wheel_separation_h,
                             bool lookup_wheel_radius)
  {
    if (!(lookup_wheel_separation_w || lookup_wheel_separation_h || lookup_wheel_radius))
    {
      // Short-circuit in case we don't need to look up anything, so we don't have to parse the URDF
      return true;
    }

    // Parse robot description
    const std::string model_param_name = "robot_description";
    bool res = root_nh.hasParam(model_param_name);
    std::string robot_model_str="";
    if (!res || !root_nh.getParam(model_param_name,robot_model_str))
    {
      ROS_ERROR_NAMED(name_, "Robot descripion couldn't be retrieved from param server.");
      return false;
    }

    boost::shared_ptr<urdf::ModelInterface> model(urdf::parseURDF(robot_model_str));

    boost::shared_ptr<const urdf::Joint> rear_left_wheel_joint(model->getJoint(rear_wheel_names[INDEX_LEFT]));
    boost::shared_ptr<const urdf::Joint> rear_right_wheel_joint(model->getJoint(rear_wheel_names[INDEX_RIGHT]));

    boost::shared_ptr<const urdf::Joint> front_left_steer_joint(model->getJoint(front_steer_names[INDEX_LEFT]));
    boost::shared_ptr<const urdf::Joint> front_left_wheel_joint(model->getJoint(front_wheel_names[INDEX_LEFT]));
    //boost::shared_ptr<const urdf::Joint> front_right_wheel_joint(model->getJoint(front_wheel_names[INDEX_RIGHT]));

    if (lookup_wheel_separation_w)
    {
      // Get wheel separation
      if (!rear_left_wheel_joint)
      {
        ROS_ERROR_STREAM_NAMED(name_, rear_wheel_names[INDEX_LEFT]
                               << " couldn't be retrieved from model description");
        return false;
      }

      if (!rear_right_wheel_joint)
      {
        ROS_ERROR_STREAM_NAMED(name_, rear_wheel_names[INDEX_RIGHT]
                               << " couldn't be retrieved from model description");
        return false;
      }

      ROS_INFO_STREAM("rear left wheel to origin: " << rear_left_wheel_joint->parent_to_joint_origin_transform.position.x << ","
                      << rear_left_wheel_joint->parent_to_joint_origin_transform.position.y << ", "
                      << rear_left_wheel_joint->parent_to_joint_origin_transform.position.z);
      ROS_INFO_STREAM("rear right wheel to origin: " << rear_right_wheel_joint->parent_to_joint_origin_transform.position.x << ","
                      << rear_right_wheel_joint->parent_to_joint_origin_transform.position.y << ", "
                      << rear_right_wheel_joint->parent_to_joint_origin_transform.position.z);

      wheel_separation_w_ = euclideanOfVectors(rear_left_wheel_joint->parent_to_joint_origin_transform.position,
                                               rear_right_wheel_joint->parent_to_joint_origin_transform.position);

    }

    if (lookup_wheel_separation_h)
    {
      // Get wheel separation
      if (!rear_left_wheel_joint)
      {
        ROS_ERROR_STREAM_NAMED(name_, rear_wheel_names[INDEX_LEFT]
                               << " couldn't be retrieved from model description");
        return false;
      }

      if (!front_left_wheel_joint)
      {
        ROS_ERROR_STREAM_NAMED(name_, front_wheel_names[INDEX_LEFT]
                               << " couldn't be retrieved from model description");
        return false;
      }

      ROS_INFO_STREAM("rear left wheel to origin: " << rear_left_wheel_joint->parent_to_joint_origin_transform.position.x << ","
                      << rear_left_wheel_joint->parent_to_joint_origin_transform.position.y << ", "
                      << rear_left_wheel_joint->parent_to_joint_origin_transform.position.z);

      urdf::Vector3 front_left_wheel_to_origin;
      front_left_wheel_to_origin.x = front_left_steer_joint->parent_to_joint_origin_transform.position.x +
              front_left_wheel_joint->parent_to_joint_origin_transform.position.x;
      front_left_wheel_to_origin.y = front_left_steer_joint->parent_to_joint_origin_transform.position.y +
              front_left_wheel_joint->parent_to_joint_origin_transform.position.y;
      front_left_wheel_to_origin.z = front_left_steer_joint->parent_to_joint_origin_transform.position.z +
              front_left_wheel_joint->parent_to_joint_origin_transform.position.z;
      ROS_INFO_STREAM("front left wheel to origin: " << front_left_wheel_to_origin.x << ","
                      << front_left_wheel_to_origin.y << ", "
                      << front_left_wheel_to_origin.z);

      wheel_separation_h_ = euclideanOfVectors(rear_left_wheel_joint->parent_to_joint_origin_transform.position,
                                               front_left_wheel_to_origin);

    }

    if (lookup_wheel_radius)
    {
      // Get wheel radius
      if (!getWheelRadius(model->getLink(front_left_wheel_joint->child_link_name), wheel_radius_))
      {
        ROS_ERROR_STREAM_NAMED(name_, "Couldn't retrieve " << rear_wheel_names[INDEX_LEFT] << " wheel radius");
        return false;
      }
    }

    return true;
  }

  void SteerDriveController::setOdomPubFields(ros::NodeHandle& root_nh, ros::NodeHandle& controller_nh)
  {
    // Get and check params for covariances
    XmlRpc::XmlRpcValue pose_cov_list;
    controller_nh.getParam(ns_ + "pose_covariance_diagonal", pose_cov_list);
    ROS_ASSERT(pose_cov_list.getType() == XmlRpc::XmlRpcValue::TypeArray);
    ROS_ASSERT(pose_cov_list.size() == 6);
    for (int i = 0; i < pose_cov_list.size(); ++i)
      ROS_ASSERT(pose_cov_list[i].getType() == XmlRpc::XmlRpcValue::TypeDouble);

    XmlRpc::XmlRpcValue twist_cov_list;
    controller_nh.getParam(ns_ + "twist_covariance_diagonal", twist_cov_list);
    ROS_ASSERT(twist_cov_list.getType() == XmlRpc::XmlRpcValue::TypeArray);
    ROS_ASSERT(twist_cov_list.size() == 6);
    for (int i = 0; i < twist_cov_list.size(); ++i)
      ROS_ASSERT(twist_cov_list[i].getType() == XmlRpc::XmlRpcValue::TypeDouble);

    // Setup odometry realtime publisher + odom message constant fields
    odom_pub_.reset(new realtime_tools::RealtimePublisher<nav_msgs::Odometry>(controller_nh, ns_ + "odom", 100));
    odom_pub_->msg_.header.frame_id = odom_frame_id_;
    odom_pub_->msg_.child_frame_id = base_frame_id_;
    odom_pub_->msg_.pose.pose.position.z = 0;
    odom_pub_->msg_.pose.covariance = boost::assign::list_of
        (static_cast<double>(pose_cov_list[0])) (0)  (0)  (0)  (0)  (0)
        (0)  (static_cast<double>(pose_cov_list[1])) (0)  (0)  (0)  (0)
        (0)  (0)  (static_cast<double>(pose_cov_list[2])) (0)  (0)  (0)
        (0)  (0)  (0)  (static_cast<double>(pose_cov_list[3])) (0)  (0)
        (0)  (0)  (0)  (0)  (static_cast<double>(pose_cov_list[4])) (0)
        (0)  (0)  (0)  (0)  (0)  (static_cast<double>(pose_cov_list[5]));
    odom_pub_->msg_.twist.twist.linear.y  = 0;
    odom_pub_->msg_.twist.twist.linear.z  = 0;
    odom_pub_->msg_.twist.twist.angular.x = 0;
    odom_pub_->msg_.twist.twist.angular.y = 0;
    odom_pub_->msg_.twist.covariance = boost::assign::list_of
        (static_cast<double>(twist_cov_list[0])) (0)  (0)  (0)  (0)  (0)
        (0)  (static_cast<double>(twist_cov_list[1])) (0)  (0)  (0)  (0)
        (0)  (0)  (static_cast<double>(twist_cov_list[2])) (0)  (0)  (0)
        (0)  (0)  (0)  (static_cast<double>(twist_cov_list[3])) (0)  (0)
        (0)  (0)  (0)  (0)  (static_cast<double>(twist_cov_list[4])) (0)
        (0)  (0)  (0)  (0)  (0)  (static_cast<double>(twist_cov_list[5]));
    tf_odom_pub_.reset(new realtime_tools::RealtimePublisher<tf::tfMessage>(root_nh, "/tf", 100));
    tf_odom_pub_->msg_.transforms.resize(1);
    tf_odom_pub_->msg_.transforms[0].transform.translation.z = 0.0;
    tf_odom_pub_->msg_.transforms[0].child_frame_id = base_frame_id_;
    tf_odom_pub_->msg_.transforms[0].header.frame_id = odom_frame_id_;
  }

} // namespace steer_drive_controller
