/**
 * @brief VisionPoseDelta plugin
 * @file vision_position_delta.cpp
 * @author M.H.Kabir <mhkabir98@gmail.com>
 * @author Vladimir Ermakov <vooon341@gmail.com>
 *
 * @addtogroup plugin
 * @{
 */
/*
 * Copyright 2014 M.H.Kabir.
 *
 * This file is part of the mavros package and subject to the license terms
 * in the top-level LICENSE file of the mavros repository.
 * https://github.com/mavlink/mavros/tree/master/LICENSE.md
 */

#include <mavros/mavros_plugin.h>
#include <mavros/setpoint_mixin.h>
#include <eigen_conversions/eigen_msg.h>
#include <std_msgs/Float64.h>
#include <math.h>
#include <geometry_msgs/PoseStamped.h>
#include <geometry_msgs/PoseWithCovarianceStamped.h>

namespace mavros {
namespace extra_plugins {
/**
 * @brief Vision pose delta plugin
 *
 * Send pose estimation from various vision estimators
 * to FCU position and attitude estimators.
 *
 */
class VisionPoseDeltaPlugin : public plugin::PluginBase,
	private plugin::TF2ListenerMixin<VisionPoseDeltaPlugin> {
public:
	VisionPoseDeltaPlugin() : PluginBase(),
		sp_nh("~vision_delta"),
		tf_rate(10.0)
	{ }

	void initialize(UAS &uas_) override
	{
		PluginBase::initialize(uas_);

		bool tf_listen;

		// tf params
		sp_nh.param("tf/listen", tf_listen, false);
		sp_nh.param<std::string>("tf/frame_id", tf_frame_id, "map");
		sp_nh.param<std::string>("tf/child_frame_id", tf_child_frame_id, "vision_delta");
		sp_nh.param("tf/rate_limit", tf_rate, 10.0);

		if (tf_listen) {
			ROS_INFO_STREAM_NAMED("vision_pose", "Listen to vision transform " << tf_frame_id
											   << " -> " << tf_child_frame_id);
			tf2_start("VisionPoseTF", &VisionPoseDeltaPlugin::transform_cb);
		}
		else {
			vision_sub = sp_nh.subscribe("pose", 10, &VisionPoseDeltaPlugin::vision_cb, this);
			vision_cov_sub = sp_nh.subscribe("pose_cov", 10, &VisionPoseDeltaPlugin::vision_cov_cb, this);
		}
		confidence_sub = sp_nh.subscribe("confidence", 10, &VisionPoseDeltaPlugin::confidence_cb, this);
		_cached_conf = 0.0 ;

	}

	Subscriptions get_subscriptions() override
	{
		return { /* Rx disabled */ };
	}

private:
	friend class TF2ListenerMixin;
	ros::NodeHandle sp_nh;

	ros::Subscriber vision_sub;
	ros::Subscriber vision_cov_sub;
	ros::Subscriber confidence_sub ; 

	std::string tf_frame_id;
	std::string tf_child_frame_id;
	float _cached_conf ; 
	double tf_rate;
	ros::Time last_transform_stamp;

	/* -*- low-level send -*- */
	/**
	 * @brief Send vision estimate transform to FCU position controller
	 */
	void send_vision_delta(const ros::Time &stamp, const Eigen::Affine3d &tr, const geometry_msgs::PoseWithCovariance::_covariance_type &cov)
	{
		/**
		 * @warning Issue #60.
		 * This now affects pose callbacks too.
		 */
		if (last_transform_stamp == stamp) {
			ROS_DEBUG_THROTTLE_NAMED(10, "vision_pose", "Vision: Same transform as last one, dropped.");
			return;
		}
		last_transform_stamp = stamp;

		auto position = ftf::transform_frame_enu_ned(Eigen::Vector3d(tr.translation()));
		auto rpy = ftf::quaternion_to_rpy(
			ftf::transform_orientation_enu_ned(
			ftf::transform_orientation_baselink_aircraft(Eigen::Quaterniond(tr.rotation()))));

		auto cov_ned = ftf::transform_frame_enu_ned(cov);
		ftf::EigenMapConstCovariance6d cov_map(cov_ned.data());

		auto urt_view = Eigen::Matrix<double, 6, 6>(cov_map.triangularView<Eigen::Upper>());
		ROS_DEBUG_STREAM_NAMED("vision_pose", "Vision: Covariance URT: " << std::endl << urt_view);

		// mavlink::common::msg::VISION_POSITION_ESTIMATE vp{};
		mavlink::ardupilotmega::msg::VISION_POSITION_DELTA vp{} ; 
		vp.time_usec = stamp.toNSec() / 1000;
		// [[[cog:
		// for f in "xyz":
		//     cog.outl("vp.%s = position.%s();" % (f, f))
		// for a, b in zip("xyz", ('roll', 'pitch', 'yaw')):
		//     cog.outl("vp.%s = rpy.%s();" % (b, a))
		// ]]]
		vp.position_delta[0] = position.x();
		vp.position_delta[1] = position.y();
		vp.position_delta[2] = position.z();
		vp.angle_delta[0] = rpy.x();
		vp.angle_delta[1] = rpy.y();
		vp.angle_delta[2] = rpy.z();
		vp.confidence = _cached_conf ; 
		// [[[end]]] (checksum: 2048daf411780847e77f08fe5a0b9dd3)
		UAS_FCU(m_uas)->send_message_ignore_drop(vp);
	}

	/* -*- callbacks -*- */

	/* common TF listener moved to mixin */

	void transform_cb(const geometry_msgs::TransformStamped &transform)
	{
		Eigen::Affine3d tr;
		tf::transformMsgToEigen(transform.transform, tr);
		ftf::Covariance6d cov {};	// zero initialized

		send_vision_delta(transform.header.stamp, tr, cov);
	}

	void vision_cb(const geometry_msgs::PoseStamped::ConstPtr &req)
	{
		Eigen::Affine3d tr;
		tf::poseMsgToEigen(req->pose, tr);
		ftf::Covariance6d cov {};	// zero initialized

		send_vision_delta(req->header.stamp, tr, cov);
	}

	void vision_cov_cb(const geometry_msgs::PoseWithCovarianceStamped::ConstPtr &req)
	{
		Eigen::Affine3d tr;
		tf::poseMsgToEigen(req->pose.pose, tr);

		send_vision_delta(req->header.stamp, tr, req->pose.covariance);
	}

	void confidence_cb(const std_msgs::Float64::ConstPtr &conf)
	{
		_cached_conf = std::min(100.0,std::max(0.0,conf->data)) ; 
	}

};
}	// namespace extra_plugins
}	// namespace mavros

#include <pluginlib/class_list_macros.hpp>
PLUGINLIB_EXPORT_CLASS(mavros::extra_plugins::VisionPoseDeltaPlugin, mavros::plugin::PluginBase)