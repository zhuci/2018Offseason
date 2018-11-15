#pragma once

#include <controller_interface/controller.h>
#include <realtime_tools/realtime_publisher.h>
#include <pcm_state_interface/pcm_state_interface.h>
#include <pcm_state_controller/PCMState.h>

namespace pcm_state_controller
{

/**
 * \brief Controller that publishes the state of all PCMs in a robot.
 *
 * This controller publishes the state of all resources registered to a \c hardware_interface::PCMStateInterface to a
 * topic of type \c pcm_state_controller/PCMState. The following is a basic configuration of the controller.
 *
 * \code
 * pcm_state_controller:
 *   type: pcm_state_controller/PCMStateController
 *   publish_rate: 50
 * \endcode
 *
 */
class PCMStateController: public controller_interface::Controller<hardware_interface::PCMStateInterface>
{
	public:
		PCMStateController() : publish_rate_(0.0) {}

		virtual bool init(hardware_interface::PCMStateInterface *hw,
						  ros::NodeHandle                       &root_nh,
						  ros::NodeHandle                       &controller_nh);
		virtual void starting(const ros::Time &time);
		virtual void update(const ros::Time &time, const ros::Duration & /*period*/);
		virtual void stopping(const ros::Time & /*time*/);

	private:
		std::vector<hardware_interface::PCMStateHandle> pcm_state_;
		std::shared_ptr<realtime_tools::RealtimePublisher<pcm_state_controller::PCMState> > realtime_pub_;
		ros::Time last_publish_time_;
		double publish_rate_;
		size_t num_pcms_;
};

}
