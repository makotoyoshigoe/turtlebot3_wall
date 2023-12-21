// SPDX-FileCopyrightText: 2023 Makoto Yoshigoe myoshigo0127@gmail.com
// SPDX-License-Identifier: Apache-2.0

#include "wall_tracking/wall_tracking.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <rclcpp/rclcpp.hpp>
#include <thread>

using namespace std::chrono_literals;

namespace WallTracking {
WallTracking::WallTracking() : Node("wall_tracking_node") 
{
    set_param();
    get_param();
    init_variable();
    init_sub();
    init_pub();
    init_action();
}
WallTracking::~WallTracking()
{
}

void WallTracking::set_param() 
{
    declare_parameter("max_linear_vel", 0.0);
    declare_parameter("max_angular_vel", 0.0);
    declare_parameter("min_angular_vel", 0.0);
    declare_parameter("distance_from_wall", 0.0);
    declare_parameter("distance_to_stop", 0.0);
    declare_parameter("sampling_rate", 0.0);
    declare_parameter("kp", 0.0);
    declare_parameter("ki", 0.0);
    declare_parameter("kd", 0.0);
    declare_parameter("start_deg_lateral", 0);
    declare_parameter("end_deg_lateral", 0);
    declare_parameter("stop_ray_th", 0.0);
    declare_parameter("wheel_separation", 0.0);
    declare_parameter("distance_to_skip", 0.0);
    declare_parameter("cmd_vel_topic_name", "");
    declare_parameter("open_place_distance", 0.0);
}

void WallTracking::get_param() 
{
    max_linear_vel_ = get_parameter("max_linear_vel").as_double();
    max_angular_vel_ = get_parameter("max_angular_vel").as_double();
    min_angular_vel_ = get_parameter("min_angular_vel").as_double();
    distance_from_wall_ = get_parameter("distance_from_wall").as_double();
    distance_to_stop_ = get_parameter("distance_to_stop").as_double();
    sampling_rate_ = get_parameter("sampling_rate").as_double();
    kp_ = get_parameter("kp").as_double();
    ki_ = get_parameter("ki").as_double();
    kd_ = get_parameter("kd").as_double();
    start_deg_lateral_ = get_parameter("start_deg_lateral").as_int();
    end_deg_lateral_ = get_parameter("end_deg_lateral").as_int();
    stop_ray_th_ = get_parameter("stop_ray_th").as_double();
    wheel_separation_ = get_parameter("wheel_separation").as_double();
    distance_to_skip_ = get_parameter("distance_to_skip").as_double();
    cmd_vel_topic_name_ = get_parameter("cmd_vel_topic_name").as_string();
    open_place_distance_ = get_parameter("open_place_distance").as_double();
}

void WallTracking::init_sub() 
{
    scan_sub_ = this->create_subscription<sensor_msgs::msg::LaserScan>(
        "scan", rclcpp::QoS(10),
        std::bind(&WallTracking::scan_callback, this, std::placeholders::_1));
    gnss_sub_ = this->create_subscription<sensor_msgs::msg::NavSatFix>(
        "gnss/fix", rclcpp::QoS(10),
        std::bind(&WallTracking::gnss_callback, this, std::placeholders::_1));
}

void WallTracking::init_pub() 
{
    cmd_vel_pub_ = this->create_publisher<geometry_msgs::msg::Twist>(cmd_vel_topic_name_, rclcpp::QoS(10));
    open_place_arrived_pub_ = this->create_publisher<std_msgs::msg::Bool>("open_place_arrived", rclcpp::QoS(10));
    open_place_detection_pub_ = this->create_publisher<std_msgs::msg::String>("open_place_detection", rclcpp::QoS(10));
}

void WallTracking::init_action() 
{
    wall_tracking_action_srv_ = rclcpp_action::create_server<WallTrackingAction>(
        this, "wall_tracking",
        std::bind(&WallTracking::handle_goal, this, std::placeholders::_1, std::placeholders::_2),
        std::bind(&WallTracking::handle_cancel, this, std::placeholders::_1),
        std::bind(&WallTracking::handle_accepted, this, std::placeholders::_1));
}

void WallTracking::init_variable() 
{
    ei_ = 0.0;
    fwc_deg_ = RAD2DEG(atan2f(-wheel_separation_ / 2, distance_to_stop_));
    float y = distance_from_wall_, x = distance_to_skip_;
    x += distance_from_wall_ / tan(DEG2RAD(start_deg_lateral_));
    flw_deg_ = RAD2DEG(atan2(y, x));
    open_place_ = false;
    outdoor_ = false;
    init_scan_data_ = false;
}

void WallTracking::pub_cmd_vel(float linear_x, float angular_z) 
{
    cmd_vel_msg_.linear.x = std::min(linear_x, max_linear_vel_);
    cmd_vel_msg_.angular.z = std::max(std::min(angular_z, max_angular_vel_), min_angular_vel_);
    cmd_vel_pub_->publish(cmd_vel_msg_);
}

void WallTracking::scan_callback(sensor_msgs::msg::LaserScan::ConstSharedPtr msg) 
{
    if (!init_scan_data_) {
        scan_data_.reset(new ScanData(msg));
        init_scan_data_ = true;
        RCLCPP_INFO(this->get_logger(), "initialized scan data");
    }
    scan_data_->dataUpdate(msg->ranges);
    switch (outdoor_)
    {
    case false:
        open_place_ = false;
        break;
    
    case true:
        float open_place_arrived_check = scan_data_->openPlaceCheck(-90., 90., open_place_distance_);
        if(!open_place_) open_place_ = open_place_arrived_check >= 0.7;
        else open_place_ = open_place_arrived_check >= 0.4;
    }
    pub_open_place_arrived(open_place_);
    // wallTracking();
    // RCLCPP_INFO(this->get_logger(), "update scan data");
}

void WallTracking::gnss_callback(sensor_msgs::msg::NavSatFix::ConstSharedPtr msg)
{
    outdoor_ = msg->position_covariance_type == msg->COVARIANCE_TYPE_UNKNOWN ? false : true;
    // RCLCPP_INFO(this->get_logger(), "outdoor: %d", outdoor_);
}

float WallTracking::lateral_pid_control(float input) 
{
    float e = input - distance_from_wall_;
    ei_ += e * sampling_rate_;
    float ed = e / sampling_rate_;
    return e * kp_ + ei_ * ki_ + ed * kd_;
}

void WallTracking::wallTracking() 
{
    float gap_th = distance_from_wall_ * 2.0;
    bool gap_start = scan_data_->conflictCheck(start_deg_lateral_, gap_th);
    bool gap_end = scan_data_->conflictCheck(90., gap_th);
    bool front_left_wall = scan_data_->thresholdCheck(flw_deg_, 1.87);
    float front_wall_check = scan_data_->frontWallCheck(fwc_deg_, distance_to_stop_);
    std::string detection_res = "Indoor";
    switch (outdoor_)
    {
        case false:
            if (front_wall_check >= stop_ray_th_) {
                // RCLCPP_INFO(get_logger(), "fw_ray num: %d", fw_ray);
                pub_cmd_vel(max_linear_vel_ / 4, DEG2RAD(-45));
                rclcpp::sleep_for(2000ms);
            } else if ((gap_start || gap_end) && !front_left_wall &&
                scan_data_->noiseCheck(flw_deg_)) {
                pub_cmd_vel(max_linear_vel_, 0.0);
                // RCLCPP_INFO(get_logger(), "skip");
            } else {
                double lateral_mean = scan_data_->leftWallCheck(start_deg_lateral_, end_deg_lateral_);
                double angular_z = lateral_pid_control(lateral_mean);
                pub_cmd_vel(max_linear_vel_, angular_z);
                // RCLCPP_INFO(get_logger(), "range: %lf", lateral_mean);
            }
        break;
        case true:
            if (front_wall_check >= stop_ray_th_) {
                pub_cmd_vel(max_linear_vel_ / 4, DEG2RAD(-45));
                rclcpp::sleep_for(2000ms);
            } else {
                std::vector<float> evals(4, 0);
                float div_num = 3;
                float degs[3][2] = 
                {
                    {-15., 15.}, 
                    {15., 45.}, 
                    {-45., -15.}
                };
                for(int i=0; i<div_num; ++i){
                    float res = scan_data_->openPlaceCheck(degs[i][0], degs[i][1], open_place_distance_);
                    evals[i] = res < 0.7 ? -1. : res;
                }
                auto max_iter = std::max_element(evals.begin(), evals.end());
                int max_index = std::distance(evals.begin(), max_iter);
                // RCLCPP_INFO(this->get_logger(), "1: %f 2: %f, 3:%f, 4: %f, max i: %d", evals[0], evals[1], evals[2], evals[3], max_index);
                switch (max_index)
                {
                    case 0:
                        detection_res = "Front";
                        pub_cmd_vel(max_linear_vel_, 0.);
                    break;
                    case 1:
                        detection_res = "Left";
                        pub_cmd_vel(max_linear_vel_, max_angular_vel_);
                    break;
                    case 2:
                        detection_res = "Right";
                        pub_cmd_vel(max_linear_vel_, min_angular_vel_);
                    break;
                    case 3:
                        detection_res = "Not open place";
                        if((gap_start || gap_end) && !front_left_wall && scan_data_->noiseCheck(flw_deg_)){
                            pub_cmd_vel(max_linear_vel_, 0.0);
                            // RCLCPP_INFO(get_logger(), "skip");
                        } else {
                            double lateral_mean = scan_data_->leftWallCheck(start_deg_lateral_, end_deg_lateral_);
                            double angular_z = lateral_pid_control(lateral_mean);
                            pub_cmd_vel(max_linear_vel_, angular_z);
                        }
                    break;
                }
            }
        break;
    }
    pub_open_place_detection(detection_res);
}

void WallTracking::pub_open_place_arrived(bool open_place_arrived)
{
    open_place_arrived_msg_.data = open_place_arrived;
    open_place_arrived_pub_->publish(open_place_arrived_msg_);
}

void WallTracking::pub_open_place_detection(std::string open_place_detection)
{
    open_place_detection_msg_.data = open_place_detection;
    open_place_detection_pub_->publish(open_place_detection_msg_);
}

rclcpp_action::GoalResponse WallTracking::handle_goal(
    const rclcpp_action::GoalUUID &uuid,
    std::shared_ptr<const WallTrackingAction::Goal> goal) 
{
        return rclcpp_action::GoalResponse::ACCEPT_AND_EXECUTE;
}

rclcpp_action::CancelResponse WallTracking::handle_cancel(
    const std::shared_ptr<GoalHandleWallTracking> goal_handle) 
{
    RCLCPP_INFO(this->get_logger(), "Received request to cancel goal");
    (void)goal_handle;
    return rclcpp_action::CancelResponse::ACCEPT;
}

void WallTracking::handle_accepted(
    const std::shared_ptr<GoalHandleWallTracking> goal_handle)
{
    std::thread{
    std::bind(&WallTracking::execute, this, std::placeholders::_1),
                goal_handle
    }.detach();
}

void WallTracking::execute(
    const std::shared_ptr<GoalHandleWallTracking> goal_handle) 
{
    RCLCPP_INFO(this->get_logger(), "EXECUTE");
    const auto goal = goal_handle->get_goal();
    auto feedback = std::make_shared<WallTrackingAction::Feedback>();
    auto result = std::make_shared<WallTrackingAction::Result>();
    feedback->end = false;
    while (rclcpp::ok()) {
        if (goal_handle->is_canceling()) {
            result->get = false;
            goal_handle->canceled(result);
            pub_cmd_vel(0.0, 0.0);
            RCLCPP_INFO(this->get_logger(), "Goal Canceled");
            return;
        }
        feedback->end = open_place_;
        goal_handle->publish_feedback(feedback);
        wallTracking();
    }
    if (rclcpp::ok()) {
        result->get = true;
        goal_handle->succeed(result);
        RCLCPP_INFO(this->get_logger(), "Goal Succeded");
    }
}
} // namespace WallTracking
