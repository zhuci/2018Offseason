/*********************************************************************
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2015, University of Colorado, Boulder
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
 *   * Neither the name of the Univ of CO, Boulder nor the names of its
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

/* Original Author: Dave Coleman
   Desc:   Example ros_control hardware interface blank template for the FRCRobot
           For a more detailed simulation example, see sim_hw_interface.cpp

	The hardware interface code reads and writes directly from/to hardware
	connected to the RoboRIO. This include DIO, Analog In, pneumatics,
	and CAN Talons, among other things.

	The two main methods are read() and write().

	read() is responsible for reading hardware state and filling in
	a buffered copy of it. This buffered copy of the hardware state
	can be accessed by various controllers to figure out what to do next.

	write() does the opposite. It takes commands that have been buffered
	by various controllers and sends them to the hardware.  The design goal
	here is to minimize redundant writes to the HW.  Previous values written
	are cached, and subsequent writes of the same value are skipped.

	The main read loop actually reads from all hardware except CAN Talons.
	The CAN talon status reads are double buffered. A thread is kicked
	off for each CAN talon.  That thread updates a buffer which is shared
	by the main read loop. The only thing the main read loop does is
	consolidate the data from each thread into a separate state buffer,
	this one externally visible to controllers.  Since reads are the slowest
	part of the process, this decouples hardware read speed from the
	control loop update rate.

	The PDP data also works in a similar way.  There is a thread running
	at a constant rate polling PDP data, and read() picks up the latest
	copy of that data each time through the read/update/write loop
*/

#include <cmath>
#include <iostream>
#include <math.h>
#include <thread>

#include <tf2/LinearMath/Matrix3x3.h>
#include "ros_control_boilerplate/frcrobot_hw_interface.h"

//HAL / wpilib includes
#include <HALInitializer.h>
#include <networktables/NetworkTable.h>
#include <hal/CAN.h>
#include <hal/Compressor.h>
#include <hal/PDP.h>
#include <hal/Power.h>
#include <hal/Solenoid.h>
#include <frc/Joystick.h>

#include <ctre/phoenix/motorcontrol/SensorCollection.h>
#include <ctre/phoenix/platform/Platform.h>

//
// digital output, PWM, Pneumatics, compressor, nidec, talons
//    controller on jetson  (local update = true, local hardware = false
//        don't do anything in read
//        random controller updates command in controller
//        set output state var from command in write() on jetson - this will be reflected in joint_states
//          but do not call Set since hardware doesn't exist (local write)
//
//      on rio (local update = false, local hardware = true
//         don't do anything in read
//         update loop needs to read joint_states using joint state listener
//             this writes values from the jetson to each local joint command on the Rio
//         write() sets hardware from those joint commands, and also sets state
//            write needs to set value as - is, don't apply invert,
//            since it was already applied on the remote side
//
//	local_update = true, local hardware = true -> no listener
//
//		This would be for hardware on the Rio which is also modified by controllers running on the Rio
//
//	local_update = false, local hardware = true -> listener to transfer cmd from remote to local
//
//		E.g. config on the Rio if a controller on the Jetson wanted to update hardware on the Rio
//
//	local_update = true, local_hardare = false -> no listener, update local state but don't write to hw
//
//		e.g. config on the Jetson if a controller on the Jetson wanted to update hardware on the rio
//
//	local_update = false, local_hardare = false -> listener to mirror updated state from local?
//
//		nothing is happening on the controller wrt the hardware other than wanting to keep current on status
//		not sure how useful this might be, except in cases like digital in where update==hardware
//		by definition
//
//	So !local_update implies add to remote Interface to run a listener
//
// For analog & digital input and state like PDP, match, joystick, etc, there's only 1 local flag.
// The only cases which make sense are local_update = local_hardware, since the value can only be
// updated by reading the hardware itself.  There, just use a "local" flag.
//
namespace frcrobot_control
{
// Dummy vars are used to create joints which are accessed via variable name
// in the low level control code. So far this is only used for sending data
// to the driver station and back via network tables.

const int pidIdx = 0; //0 for primary closed-loop, 1 for cascaded closed-loop
const int timeoutMs = 0; //If nonzero, function will wait for config success and report an error if it times out. If zero, no blocking or checking is performed

// Constructor. Pass appropriate params to base class constructor,
// initialze robot_ pointer to NULL
FRCRobotHWInterface::FRCRobotHWInterface(ros::NodeHandle &nh, urdf::Model *urdf_model)
	: ros_control_boilerplate::FRCRobotInterface(nh, urdf_model)
	, robot_(nullptr)
{
}

// Clean up whatever we've created in init()
FRCRobotHWInterface::~FRCRobotHWInterface()
{
	motion_profile_thread_.join();

	for (size_t i = 0; i < num_can_talon_srxs_; i++)
	{
		if (can_talon_srx_local_hardwares_[i])
		{
			custom_profile_threads_[i].join();
			talon_read_threads_[i].join();
		}
	}

	for (size_t i = 0; i < num_solenoids_; i++)
		HAL_FreeSolenoidPort(solenoids_[i]);
	for (size_t i = 0; i < num_double_solenoids_; i++)
	{
		HAL_FreeSolenoidPort(double_solenoids_[i].forward_);
		HAL_FreeSolenoidPort(double_solenoids_[i].reverse_);
	}

	for (size_t i = 0; i < num_compressors_; i++)
		pcm_thread_[i].join();
	for (size_t i = 0; i < num_pdps_; i++)
		pdp_thread_[i].join();
}

/*
 * Thread to feed talon motion profile data from
 * software buffers into the hardware
 * Previous attempts acted weird - different
 * talons would start and stop profiles at different
 * times.  This code has since been updated to lock
 * access to motion profile config to insure only
 * one thread is working with it at a time - perhaps
 * that will help? Need to test
 * also, experiment with 1 thread per talon rather that
 * 1 thread for all of them
 */
void FRCRobotHWInterface::process_motion_profile_buffer_thread(double hz)
{
	return;
#if 0
	ros::Duration(3).sleep();
	bool set_frame_period[num_can_talon_srxs_];
	for (size_t i = 0; i < num_can_talon_srxs_; i++)
		set_frame_period[i] = false;

	ros::Rate rate(hz);
	while (ros::ok())
	{
		bool writing_points = false;
		for (size_t i = 0; i < num_can_talon_srxs_; i++)
		{
			// TODO : see if we can eliminate this.  It is used to
			// skip accesses if motion profiles were never written
			// but we can also test using mp_status below...
			if (can_talons_mp_written_[i]->load(std::memory_order_relaxed))
			{
				// Avoid accessing motion profile data
				// while the write() loop is also writing it.
				std::lock_guard<std::mutex> l(*motion_profile_mutexes_[i]);

				const hardware_interface::TalonMode talon_mode = talon_state_[i].getTalonMode();
				if (talon_mode == hardware_interface::TalonMode_Follower)
					continue;
				const hardware_interface::MotionProfileStatus mp_status = talon_state_[i].getMotionProfileStatus();
				// Only write to non-follow, non-disabled talons that
				// have points to write from their top-level buffer
				//ROS_INFO_STREAM("top count: " << can_talons_[i]->GetMotionProfileTopLevelBufferCount());
				//ROS_WARN_STREAM("id: " << i << " top size: " << mp_status.topBufferCnt << " running: " << (*can_talons_mp_running_)[i].load(std::memory_order_relaxed));
				if ((mp_status.topBufferCnt && mp_status.btmBufferCnt < 127) ||
					can_talons_mp_running_[i]->load(std::memory_order_relaxed))
				{
					if (!set_frame_period[i])
					{
						can_talons_[i]->ChangeMotionControlFramePeriod(1000./hz); // 1000 to convert from sec to mSec
						talon_state_[i].setMotionControlFramePeriod(1000./hz);
						set_frame_period[i] = true;
					}
					// Only write if SW buffer has entries in it
					//ROS_INFO("needs to send points");
					writing_points = true;
					can_talons_[i]->ProcessMotionProfileBuffer();
				}
			}
		}
		writing_points_.store(writing_points, std:memory_order_relaxed);
		rate.sleep();
	}
#endif
}

// Stuff to support generalized custom profile code
void FRCRobotHWInterface::customProfileSetSensorPosition(int joint_id, double position)
{
	can_talons_[joint_id]->SetSelectedSensorPosition(position, pidIdx, timeoutMs);
}

// Maybe find a way to make use of this in write() as well?
void FRCRobotHWInterface::customProfileSetMode(int joint_id,
		hardware_interface::TalonMode mode,
		double setpoint,
		hardware_interface::DemandType demandtype,
		double demandvalue)
{
	ctre::phoenix::motorcontrol::ControlMode out_mode;

	if (!convertControlMode(mode, out_mode))
		return;

	const hardware_interface::FeedbackDevice encoder_feedback = talon_state_[joint_id].getEncoderFeedback();
	const int encoder_ticks_per_rotation = talon_state_[joint_id].getEncoderTicksPerRotation();
	const double conversion_factor = talon_state_[joint_id].getConversionFactor();

	const double radians_scale = getConversionFactor(encoder_ticks_per_rotation, encoder_feedback, hardware_interface::TalonMode_Position) * conversion_factor;
	const double radians_per_second_scale = getConversionFactor(encoder_ticks_per_rotation, encoder_feedback, hardware_interface::TalonMode_Velocity)* conversion_factor;
	switch (out_mode)
	{
		case ctre::phoenix::motorcontrol::ControlMode::Velocity:
			setpoint /= radians_per_second_scale;
			break;
		case ctre::phoenix::motorcontrol::ControlMode::Position:
			setpoint /= radians_scale;
			break;
		case ctre::phoenix::motorcontrol::ControlMode::MotionMagic:
			setpoint /= radians_scale;
			break;
	}

	ctre::phoenix::motorcontrol::DemandType out_demandtype;
	if (!convertDemand1Type(demandtype, out_demandtype))
	{
		ROS_ERROR("Invalid demand type in hw_interface :: customProfileSetMode");
		return;
	}
	can_talons_[joint_id]->Set(out_mode, setpoint, out_demandtype, demandvalue); //TODO: unit conversion
}

void FRCRobotHWInterface::customProfileSetPIDF(int    joint_id,
											   int    pid_slot,
											   double p,
											   double i,
											   double d,
											   double f,
											   int    iz,
											   int    allowable_closed_loop_error,
											   double max_integral_accumulator,
											   double closed_loop_peak_output,
											   int    closed_loop_period)
{
	can_talons_[joint_id]->Config_kP(pid_slot, p, timeoutMs);
	can_talons_[joint_id]->Config_kI(pid_slot, i, timeoutMs);
	can_talons_[joint_id]->Config_kD(pid_slot, d, timeoutMs);
	can_talons_[joint_id]->Config_kF(pid_slot, f, timeoutMs);
	can_talons_[joint_id]->Config_IntegralZone(pid_slot, iz, timeoutMs);
	// TODO : Scale these two?
	can_talons_[joint_id]->ConfigAllowableClosedloopError(pid_slot, allowable_closed_loop_error, timeoutMs);
	can_talons_[joint_id]->ConfigMaxIntegralAccumulator(pid_slot, max_integral_accumulator, timeoutMs);
	can_talons_[joint_id]->ConfigClosedLoopPeakOutput(pid_slot, closed_loop_peak_output, timeoutMs);
	can_talons_[joint_id]->ConfigClosedLoopPeriod(pid_slot, closed_loop_period, timeoutMs);

	can_talons_[joint_id]->SelectProfileSlot(pid_slot, pidIdx);
}

// TODO : Think some more on how this will work.  Previous idea of making them
// definable joints was good as well, but required some hard coding to
// convert from name to an actual variable. This requires hard-coding here
// but not in the read or write code.  Not sure which is better
std::vector<ros_control_boilerplate::DummyJoint> FRCRobotHWInterface::getDummyJoints(void)
{
	std::vector<ros_control_boilerplate::DummyJoint> dummy_joints;
	dummy_joints.push_back(Dumify(cube_state_));
	dummy_joints.push_back(Dumify(auto_state_0_));
	dummy_joints.push_back(Dumify(auto_state_1_));
	dummy_joints.push_back(Dumify(auto_state_2_));
	dummy_joints.push_back(Dumify(auto_state_3_));
	dummy_joints.push_back(Dumify(stop_arm_));
	dummy_joints.push_back(Dumify(override_arm_limits_));
	dummy_joints.push_back(Dumify(disable_compressor_));
	dummy_joints.push_back(Dumify(starting_config_));
	dummy_joints.push_back(Dumify(navX_zero_));
	return dummy_joints;
}

void FRCRobotHWInterface::init(void)
{
	// Do base class init. This loads common interface info
	// used by both the real and sim interfaces
	FRCRobotInterface::init();

	if (run_hal_robot_)
	{
		// Make sure to initialize WPIlib code before creating
		// a CAN Talon object to avoid NIFPGA: Resource not initialized
		// errors? See https://www.chiefdelphi.com/forums/showpost.php?p=1640943&postcount=3
		robot_.reset(new ROSIterativeRobot());
		realtime_pub_nt_.reset(new realtime_tools::RealtimePublisher<ros_control_boilerplate::AutoMode>(nh_, "autonomous_mode", 1));
		realtime_pub_nt_->msg_.mode.resize(4);
		realtime_pub_nt_->msg_.delays.resize(4);
		realtime_pub_error_.reset(new realtime_tools::RealtimePublisher<std_msgs::Float64>(nh_, "error_times", 4));
		last_nt_publish_time_ = ros::Time::now();

		error_msg_last_received_ = false;
		error_pub_start_time_ = last_nt_publish_time_.toSec();
	}
	else
	{
		// This is for non Rio-based robots.  Call init for the wpilib HAL code
		// we've "borrowed" before using them
		//hal::InitializeCAN();
		hal::init::InitializeCANAPI();
		hal::init::InitializeCompressor();
		hal::init::InitializePCMInternal();
		hal::init::InitializePDP();
		hal::init::InitializeSolenoid();

		ctre::phoenix::platform::can::SetCANInterface(can_interface_.c_str());
	}

	custom_profile_threads_.resize(num_can_talon_srxs_);
#ifdef USE_TALON_MOTION_PROFILE
	profile_is_live_.store(false, std::memory_order_relaxed);
#endif

	for (size_t i = 0; i < num_can_talon_srxs_; i++)
	{
		ROS_INFO_STREAM_NAMED("frcrobot_hw_interface",
							  "Loading joint " << i << "=" << can_talon_srx_names_[i] <<
							  (can_talon_srx_local_updates_[i] ? " local" : " remote") << " update, " <<
							  (can_talon_srx_local_hardwares_[i] ? "local" : "remote") << " hardware" <<
							  " as CAN id " << can_talon_srx_can_ids_[i]);

		can_talons_mp_written_.push_back(std::make_shared<std::atomic<bool>>(false));
		can_talons_mp_running_.push_back(std::make_shared<std::atomic<bool>>(false));
		if (can_talon_srx_local_hardwares_[i])
		{
			can_talons_.push_back(std::make_shared<ctre::phoenix::motorcontrol::can::TalonSRX>(can_talon_srx_can_ids_[i]));
			can_talons_[i]->Set(ctre::phoenix::motorcontrol::ControlMode::Disabled, 0, 20);

			// Clear sticky faults
			//safeTalonCall(can_talons_[i]->ClearStickyFaults(timeoutMs), "ClearStickyFaults()");


			// TODO : if the talon doesn't initialize - maybe known
			// by -1 from firmware version read - somehow tag
			// the entry in can_talons_[] as uninitialized.
			// This probably should be a fatal error
			ROS_INFO_STREAM_NAMED("frcrobot_hw_interface",
								  "\tTalon SRX firmware version " << can_talons_[i]->GetFirmwareVersion());

			custom_profile_threads_[i] = std::thread(&FRCRobotHWInterface::custom_profile_thread, this, i);

			// Create a thread for each talon that is responsible for reading
			// status data from that controller.
			talon_read_state_mutexes_.push_back(std::make_shared<std::mutex>());
			talon_read_thread_states_.push_back(std::make_shared<hardware_interface::TalonHWState>(can_talon_srx_can_ids_[i]));
			talon_read_threads_.push_back(std::thread(&FRCRobotHWInterface::talon_read_thread, this,
										  can_talons_[i], talon_read_thread_states_[i],
										  can_talons_mp_written_[i], talon_read_state_mutexes_[i]));
		}
		else
		{
			// Need to have a CAN talon object created on the Rio
			// for that talon to be enabled.  Don't want to do anything with
			// them, though, so the local flags should be set to false
			// which means both reads and writes will be skipped
			if (run_hal_robot_)
				can_talons_.push_back(std::make_shared<ctre::phoenix::motorcontrol::can::TalonSRX>(can_talon_srx_can_ids_[i]));
			else
				// Add a null pointer as the can talon for this index - no
				// actual local hardware identified for it so nothing to create.
				// Just keep the indexes of all the various can_talon arrays in sync
				can_talons_.push_back(nullptr);
			talon_read_state_mutexes_.push_back(nullptr);
			talon_read_thread_states_.push_back(nullptr);
		}
	}
	for (size_t i = 0; i < num_nidec_brushlesses_; i++)
	{
		ROS_INFO_STREAM_NAMED("frcrobot_hw_interface",
							  "Loading joint " << i << "=" << nidec_brushless_names_[i] <<
							  (nidec_brushless_local_updates_[i] ? " local" : " remote") << " update, " <<
							  (nidec_brushless_local_hardwares_[i] ? "local" : "remote") << " hardware" <<
							  " as PWM channel " << nidec_brushless_pwm_channels_[i] <<
							  " / DIO channel " << nidec_brushless_dio_channels_[i] <<
							  " invert " << nidec_brushless_inverts_[i]);

		if (nidec_brushless_local_hardwares_[i])
		{
			nidec_brushlesses_.push_back(std::make_shared<frc::NidecBrushless>(nidec_brushless_pwm_channels_[i], nidec_brushless_dio_channels_[i]));
			nidec_brushlesses_[i]->SetInverted(nidec_brushless_inverts_[i]);
		}
		else
			nidec_brushlesses_.push_back(nullptr);
	}
	for (size_t i = 0; i < num_digital_inputs_; i++)
	{
		ROS_INFO_STREAM_NAMED("frcrobot_hw_interface",
							  "Loading joint " << i << "=" << digital_input_names_[i] <<
							  " local = " << digital_input_locals_[i] <<
							  " as Digital Input " << digital_input_dio_channels_[i] <<
							  " invert " << digital_input_inverts_[i]);

		if (digital_input_locals_[i])
			digital_inputs_.push_back(std::make_shared<frc::DigitalInput>(digital_input_dio_channels_[i]));
		else
			digital_inputs_.push_back(nullptr);
	}
	for (size_t i = 0; i < num_digital_outputs_; i++)
	{
		ROS_INFO_STREAM_NAMED("frcrobot_hw_interface",
							  "Loading joint " << i << "=" << digital_output_names_[i] <<
							  (digital_output_local_updates_[i] ? " local" : " remote") << " update, " <<
							  (digital_output_local_hardwares_[i] ? "local" : "remote") << " hardware" <<
							  " as Digital Output " << digital_output_dio_channels_[i] <<
							  " invert " << digital_output_inverts_[i]);

		if (digital_output_local_hardwares_[i])
			digital_outputs_.push_back(std::make_shared<frc::DigitalOutput>(digital_output_dio_channels_[i]));
		else
			digital_outputs_.push_back(nullptr);
	}
	for (size_t i = 0; i < num_pwm_; i++)
	{
		ROS_INFO_STREAM_NAMED("frcrobot_hw_interface",
							  "Loading joint " << i << "=" << pwm_names_[i] <<
							  (pwm_local_updates_[i] ? " local" : " remote") << " update, " <<
							  (pwm_local_hardwares_[i] ? "local" : "remote") << " hardware" <<
							  " as Digitial Output " << pwm_pwm_channels_[i] <<
							  " invert " << pwm_inverts_[i]);

		if (pwm_local_hardwares_[i])
		{
			PWMs_.push_back(std::make_shared<frc::PWM>(pwm_pwm_channels_[i]));
			PWMs_[i]->SetSafetyEnabled(true);
		}
		else
			PWMs_.push_back(nullptr);
	}
	for (size_t i = 0; i < num_solenoids_; i++)
	{
		ROS_INFO_STREAM_NAMED("frcrobot_hw_interface",
							  "Loading joint " << i << "=" << solenoid_names_[i] <<
							  (solenoid_local_updates_[i] ? " local" : " remote") << " update, " <<
							  (solenoid_local_hardwares_[i] ? "local" : "remote") << " hardware" <<
							  " as Solenoid " << solenoid_ids_[i]
							  << " with pcm " << solenoid_pcms_[i]);

		if (solenoid_local_hardwares_[i])
		{
			int32_t status = 0;
			solenoids_.push_back(HAL_InitializeSolenoidPort(HAL_GetPortWithModule(solenoid_pcms_[i], solenoid_ids_[i]), &status));
			if (solenoids_.back() == HAL_kInvalidHandle)
				ROS_ERROR_STREAM("Error intializing solenoid : status=" << status);
			else
				HAL_Report(HALUsageReporting::kResourceType_Solenoid,
						solenoid_ids_[i], solenoid_pcms_[i]);
		}
		else
			solenoids_.push_back(HAL_kInvalidHandle);
	}
	for (size_t i = 0; i < num_double_solenoids_; i++)
	{
		ROS_INFO_STREAM_NAMED("frcrobot_hw_interface",
							  "Loading joint " << i << "=" << double_solenoid_names_[i] <<
							  (double_solenoid_local_updates_[i] ? " local" : " remote") << " update, " <<
							  (double_solenoid_local_hardwares_[i] ? "local" : "remote") << " hardware" <<
							  " as Double Solenoid forward " << double_solenoid_forward_ids_[i] <<
							  " reverse " << double_solenoid_reverse_ids_[i]
							  << " with pcm " << double_solenoid_pcms_[i]);

		if (double_solenoid_local_hardwares_[i])
		{
			int32_t forward_status = 0;
			int32_t reverse_status = 0;
			auto forward_handle = HAL_InitializeSolenoidPort(
					HAL_GetPortWithModule(double_solenoid_pcms_[i], double_solenoid_forward_ids_[i]),
					&forward_status);
			auto reverse_handle = HAL_InitializeSolenoidPort(
					HAL_GetPortWithModule(double_solenoid_pcms_[i], double_solenoid_reverse_ids_[i]),
					&reverse_status);
			if ((forward_handle != HAL_kInvalidHandle) &&
			    (reverse_handle != HAL_kInvalidHandle) )
			{
				double_solenoids_.push_back(DoubleSolenoidHandle(forward_handle, reverse_handle));
				HAL_Report(HALUsageReporting::kResourceType_Solenoid,
						double_solenoid_forward_ids_[i], solenoid_pcms_[i]);
				HAL_Report(HALUsageReporting::kResourceType_Solenoid,
						double_solenoid_reverse_ids_[i], solenoid_pcms_[i]);
			}
			else
			{
				ROS_ERROR_STREAM("Error intializing double solenoid : status=" << forward_status << " : " << reverse_status);
				double_solenoids_.push_back(DoubleSolenoidHandle(HAL_kInvalidHandle, HAL_kInvalidHandle));
				HAL_FreeSolenoidPort(forward_handle);
				HAL_FreeSolenoidPort(reverse_handle);
			}
		}
		else
			double_solenoids_.push_back(DoubleSolenoidHandle(HAL_kInvalidHandle, HAL_kInvalidHandle));
	}

	//RIGHT NOW THIS WILL ONLY WORK IF THERE IS ONLY ONE NAVX INSTANTIATED
	for(size_t i = 0; i < num_navX_; i++)
	{
		ROS_INFO_STREAM_NAMED("frcrobot_hw_interface",
				"Loading joint " << i << "=" << navX_names_[i] <<
				" as navX id " << navX_ids_[i] <<
				" local = " << navX_locals_[i]);
		//TODO: fix how we use ids

		if (navX_locals_[i])
			navXs_.push_back(std::make_shared<AHRS>(SPI::Port::kMXP));
		else
			navXs_.push_back(nullptr);

		// This is a guess so TODO : get better estimates
		imu_orientation_covariances_[i] = {0.0015, 0.0, 0.0, 0.0, 0.0015, 0.0, 0.0, 0.0, 0.0015};
		imu_angular_velocity_covariances_[i] = {0.0015, 0.0, 0.0, 0.0, 0.0015, 0.0, 0.0, 0.0, 0.0015};
		imu_linear_acceleration_covariances_[i] ={0.0015, 0.0, 0.0, 0.0, 0.0015, 0.0, 0.0, 0.0, 0.0015};
		break; // TODO : only support 1 for now - if we need more, need to define
		       // the interface in config files somehow
	}
	for (size_t i = 0; i < num_analog_inputs_; i++)
	{
		ROS_INFO_STREAM_NAMED("frcrobot_hw_interface",
							  "Loading joint " << i << "=" << analog_input_names_[i] <<
							  " local = " << analog_input_locals_[i] <<
							  " as Analog Input " << analog_input_analog_channels_[i]);
		if (analog_input_locals_[i])
			analog_inputs_.push_back(std::make_shared<frc::AnalogInput>(analog_input_analog_channels_[i]));
		else
			analog_inputs_.push_back(nullptr);
	}
	for (size_t i = 0; i < num_compressors_; i++)
	{
		ROS_INFO_STREAM_NAMED("frcrobot_hw_interface",
							  "Loading joint " << i << "=" << compressor_names_[i] <<
							  (compressor_local_updates_[i] ? " local" : " remote") << " update, " <<
							  (compressor_local_hardwares_[i] ? "local" : "remote") << " hardware" <<
							  " as Compressor with pcm " << compressor_pcm_ids_[i]);

		pcm_read_thread_state_.push_back(std::make_shared<hardware_interface::PCMState>(compressor_pcm_ids_[i]));
		if (compressor_local_hardwares_[i])
		{
			if (!HAL_CheckCompressorModule(compressor_pcm_ids_[i]))
			{
				ROS_ERROR("Invalid Compressor PDM ID");
				compressors_.push_back(HAL_kInvalidHandle);
			}
			else
			{
				int32_t status = 0;
				compressors_.push_back(HAL_InitializeCompressor(compressor_pcm_ids_[i], &status));
				if (compressors_[i] != HAL_kInvalidHandle)
				{
					pcm_read_thread_mutexes_.push_back(std::make_shared<std::mutex>());
					pcm_thread_.push_back(std::thread(&FRCRobotHWInterface::pcm_read_thread, this,
								compressors_[i], compressor_pcm_ids_[i], pcm_read_thread_state_[i],
								pcm_read_thread_mutexes_[i]));
					HAL_Report(HALUsageReporting::kResourceType_Compressor, compressor_pcm_ids_[i]);
				}
			}
		}
		else
			compressors_.push_back(HAL_kInvalidHandle);
	}

	// No real init needed here, just report the config loaded for them
	for (size_t i = 0; i < num_rumbles_; i++)
		ROS_INFO_STREAM_NAMED("frcrobot_hw_interface",
							  "Loading joint " << i << "=" << rumble_names_[i] <<
							  (rumble_local_updates_[i] ? " local" : " remote") << " update, " <<
							  (rumble_local_hardwares_[i] ? "local" : "remote") << " hardware" <<
							  " as Rumble with port" << rumble_ports_[i]);

	for (size_t i = 0; i < num_pdps_; i++)
	{
		ROS_INFO_STREAM_NAMED("frcrobot_hw_interface",
							  "Loading joint " << i << "=" << pdp_names_[i] <<
							  " local = " << pdp_locals_[i] <<
							  " as PDP");

		if (pdp_locals_[i])
		{
			if (!HAL_CheckPDPModule(pdp_modules_[i]))
			{
				ROS_ERROR("Invalid PDP module number");
				pdps_.push_back(HAL_kInvalidHandle);
			}
			else
			{
				int32_t status = 0;
				pdps_.push_back(HAL_InitializePDP(pdp_modules_[i], &status));
				pdp_read_thread_state_.push_back(std::make_shared<hardware_interface::PDPHWState>());
				if (pdps_[i] == HAL_kInvalidHandle)
				{
					ROS_ERROR_STREAM("Could not initialize PDP module, status = " << status);
				}
				else
				{
					pdp_read_thread_mutexes_.push_back(std::make_shared<std::mutex>());
					pdp_thread_.push_back(std::thread(&FRCRobotHWInterface::pdp_read_thread, this,
										  pdps_[i], pdp_read_thread_state_[i], pdp_read_thread_mutexes_[i]));
					HAL_Report(HALUsageReporting::kResourceType_PDP, pdp_modules_[i]);
				}
			}
		}
		else
			pdps_.push_back(HAL_kInvalidHandle);
	}

	// TODO : better support for multiple joysticks?
	bool started_pub = false;
	for (size_t i = 0; i < num_joysticks_; i++)
	{
		ROS_INFO_STREAM_NAMED("frcrobot_hw_interface",
							  "Loading joint " << i << "=" << joystick_names_[i] <<
							  " local = " << joystick_locals_[i] <<
							  " as joystick with ID " << joystick_ids_[i]);
		if (joystick_locals_[i])
		{
			joysticks_.push_back(std::make_shared<Joystick>(joystick_ids_[i]));
			if (!started_pub)
			{
				realtime_pub_joystick_ = std::make_shared<realtime_tools::RealtimePublisher<ros_control_boilerplate::JoystickState>>(nh_, "joystick_states", 1);
				started_pub = true;
			}
		}
		else
			joysticks_.push_back(nullptr);

		joystick_up_last_.push_back(false);
		joystick_down_last_.push_back(false);
		joystick_right_last_.push_back(false);
		joystick_left_last_.push_back(false);
	}

	navX_angle_ = 0;
	pressure_ = 0;
	navX_zero_ = -10000;

	for (size_t i = 0; i < num_can_talon_srxs_; i++)
		motion_profile_mutexes_.push_back(std::make_shared<std::mutex>());
	motion_profile_thread_ = std::thread(&FRCRobotHWInterface::process_motion_profile_buffer_thread, this, 100.);

	ROS_INFO_NAMED("frcrobot_hw_interface", "FRCRobotHWInterface Ready.");
}

// Each talon gets their own read thread. The thread loops at a fixed rate
// reading all state from that talon. The state is copied to a shared buffer
// at the end of each iteration of the loop.
// The code tries to only read status when we expect there to be new
// data given the update rate of various CAN messages.
void FRCRobotHWInterface::talon_read_thread(std::shared_ptr<ctre::phoenix::motorcontrol::can::TalonSRX> talon,
											std::shared_ptr<hardware_interface::TalonHWState> state,
											std::shared_ptr<std::atomic<bool>> mp_written,
											std::shared_ptr<std::mutex> mutex)
{
	ros::Rate rate(100); // TODO : configure me from a file or
						 // be smart enough to run at the rate of the fastest status update?

	ros::Time last_status_1_time = ros::Time::now();
	ros::Duration status_1_period;

	ros::Time last_status_2_time = ros::Time::now();
	ros::Duration status_2_period;

	ros::Time last_status_4_time = ros::Time::now();
	ros::Duration status_4_period;

	ros::Time last_status_9_time = ros::Time::now();
	ros::Duration status_9_period;

	ros::Time last_status_10_time = ros::Time::now();
	ros::Duration status_10_period;

	ros::Time last_status_13_time = ros::Time::now();
	ros::Duration status_13_period;

	// TODO = not sure about this timing
	ros::Time last_sensor_collection_time = ros::Time::now();
	ros::Duration sensor_collection_period;

	double time_sum = 0.;
	unsigned iteration_count = 0;

	// This never changes so read it once when the thread is started
	int can_id;
	{
		std::lock_guard<std::mutex> l(*mutex);
		can_id = state->getCANID();
	}

	while(ros::ok())
	{
		struct timespec start_time;
		clock_gettime(CLOCK_MONOTONIC, &start_time);

#if 0
		if(mp_written->load(std::memory_order_relaxed))
			ROS_INFO_STREAM("written");
#endif
		hardware_interface::TalonMode talon_mode;
		hardware_interface::FeedbackDevice encoder_feedback;
		int encoder_ticks_per_rotation;
		double conversion_factor;

		// Update local status with relevant global config
		// values set by write(). This way, items configured
		// by controllers will be reflected in the state here
		// used when reading from talons.
		// Realistically they won't change much (except maybe mode)
		// but unless it causes performance problems reading them
		// each time through the loop is easier than waiting until
		// they've been correctly set by write() before using them
		// here.
		// Note that this isn't a complete list - only the values
		// used by the read thread are copied over.  Update
		// as needed when more are read
		{
			std::lock_guard<std::mutex> l(*mutex);
			talon_mode = state->getTalonMode();
			encoder_feedback = state->getEncoderFeedback();
			encoder_ticks_per_rotation = state->getEncoderTicksPerRotation();
			conversion_factor = state->getConversionFactor();
			ros::Duration status_1_period = ros::Duration(state->getStatusFramePeriod(hardware_interface::Status_1_General));
			ros::Duration status_2_period = ros::Duration(state->getStatusFramePeriod(hardware_interface::Status_2_Feedback0));
			ros::Duration status_4_period = ros::Duration(state->getStatusFramePeriod(hardware_interface::Status_4_AinTempVbat));
			ros::Duration status_9_period = ros::Duration(state->getStatusFramePeriod(hardware_interface::Status_9_MotProfBuffer));
			ros::Duration status_10_period = ros::Duration(state->getStatusFramePeriod(hardware_interface::Status_10_MotionMagic));
			ros::Duration status_13_period = ros::Duration(state->getStatusFramePeriod(hardware_interface::Status_13_Base_PIDF0));
			ros::Duration sensor_collection_period = ros::Duration(.1); // TODO : fix me
			if (!state->getEnableReadThread())
				return;
		}

		// TODO : in main read() loop copy status from talon being followed
		// into follower talon state?
		if (talon_mode == hardware_interface::TalonMode_Follower)
			return;

		const double radians_scale = getConversionFactor(encoder_ticks_per_rotation, encoder_feedback, hardware_interface::TalonMode_Position) * conversion_factor;
		const double radians_per_second_scale = getConversionFactor(encoder_ticks_per_rotation, encoder_feedback, hardware_interface::TalonMode_Velocity) * conversion_factor;

		bool update_mp_status = false;
		hardware_interface::MotionProfileStatus internal_status;

#ifdef USE_TALON_MOTION_PROFILE
		if (profile_is_live_.load(std::memory_order_relaxed))
		{
			// TODO - this should be if (!drivebase)
			// Don't bother reading status while running
			// drive base motion profile code
			if (can_id == 51 || can_id == 41) //All we care about are the arm and lift
			{
				const double position = talon->GetSelectedSensorPosition(pidIdx) * radians_scale;
				safeTalonCall(talon->GetLastError(), "GetSelectedSensorPosition");
				std::lock_guard<std::mutex> l(*mutex);
				state->setPosition(position);
			}
			rate.sleep();
			continue;
		}

		// Vastly reduce the stuff being read while
		// buffering motion profile poinstate-> This lets CAN
		// bus bandwidth be used for writing points as
		// quickly as possible
		if (writing_points_.load(std::memory_order_relaxed))
		{
			// TODO : get rid of this hard-coded canID stuff
			if (can_id == 51 || can_id == 41) //All we care about are the arm and lift
			{
				const double position = talon->GetSelectedSensorPosition(pidIdx) * radians_scale;
				safeTalonCall(talon->GetLastError(), "GetSelectedSensorPosition");
				std::lock_guard<std::mutex> l(*mutex);
				state->setPosition(position);
			}
			// TODO - don't hard code
			// This is a check to see if the talon is a drive base one
			else if (can_id <= 30)
			{
				ctre::phoenix::motion::MotionProfileStatus talon_status;
				safeTalonCall(talon->GetMotionProfileStatus(talon_status), "GetMotionProfileStatus");

				internal_status.topBufferRem = talon_status.topBufferRem;
				internal_status.topBufferCnt = talon_status.topBufferCnt;
				internal_status.btmBufferCnt = talon_status.btmBufferCnt;
				internal_status.hasUnderrun = talon_status.hasUnderrun;
				internal_status.isUnderrun = talon_status.isUnderrun;
				internal_status.activePointValid = talon_status.activePointValid;
				internal_status.isLast = talon_status.isLast;
				internal_status.profileSlotSelect0 = talon_status.profileSlotSelect0;
				internal_status.profileSlotSelect1 = talon_status.profileSlotSelect1;
				internal_status.outputEnable = static_cast<hardware_interface::SetValueMotionProfile>(talon_status.outputEnable);
				internal_status.timeDurMs = talon_status.timeDurMs;

				std::lock_guard<std::mutex> l(*mutex);
				state->setMotionProfileStatus(internal_status);
			}

			rate.sleep();
			continue;
		}
		// TODO : don't hard-code this
		// Code to handle status read for drive base motion
		// profile mode
		else if (can_id < 30 && mp_written->load(std::memory_order_relaxed))
		{
			ctre::phoenix::motion::MotionProfileStatus talon_status;
			safeTalonCall(talon->GetMotionProfileStatus(talon_status), "GetMotionProfileStatus");

			internal_status.topBufferRem = talon_status.topBufferRem;
			internal_status.topBufferCnt = talon_status.topBufferCnt;
			internal_status.btmBufferCnt = talon_status.btmBufferCnt;
			internal_status.hasUnderrun = talon_status.hasUnderrun;
			internal_status.isUnderrun = talon_status.isUnderrun;
			internal_status.activePointValid = talon_status.activePointValid;
			internal_status.isLast = talon_status.isLast;
			internal_status.profileSlotSelect0 = talon_status.profileSlotSelect0;
			internal_status.profileSlotSelect1 = talon_status.profileSlotSelect1;
			internal_status.outputEnable = static_cast<hardware_interface::SetValueMotionProfile>(talon_status.outputEnable);
			internal_status.timeDurMs = talon_status.timeDurMs;
			update_mp_status = true;
		}
#endif

		bool update_status_1 = false;
		double motor_output_percent;
		ctre::phoenix::motorcontrol::Faults faults;
		ros::Time ros_time_now = ros::Time::now();
		// General status 1 signals = default 10msec
		if ((last_status_1_time + status_1_period) < ros_time_now)
		{
			motor_output_percent = talon->GetMotorOutputPercent();
			safeTalonCall(talon->GetLastError(), "GetMotorOutputPercent");

			// TODO : Check this
			safeTalonCall(talon->GetFaults(faults), "GetFaults");

			// Supposedly limit switch pin state

			// applied control mode - cached
			// soft limit and limit switch override - cached
			update_status_1 = true;
			last_status_1_time = ros_time_now;
		}

		// status 2 = 20 msec default
		bool update_status_2 = false;
		double position;
		double velocity;
		double output_current;
		ctre::phoenix::motorcontrol::StickyFaults sticky_faults;

		if ((last_status_2_time + status_2_period) < ros_time_now)
		{
			position = talon->GetSelectedSensorPosition(pidIdx) * radians_scale;
			safeTalonCall(talon->GetLastError(), "GetSelectedSensorPosition");

			velocity = talon->GetSelectedSensorVelocity(pidIdx) * radians_per_second_scale;
			safeTalonCall(talon->GetLastError(), "GetSelectedSensorVelocity");

			output_current = talon->GetOutputCurrent();
			safeTalonCall(talon->GetLastError(), "GetOutputCurrent");

			safeTalonCall(talon->GetStickyFaults(sticky_faults), "GetStickyFault");

			update_status_2 = true;
			last_status_2_time = ros_time_now;
		}

		// Temp / Voltage status 4 == 160 mSec default
		bool update_status_4 = false;
		double temperature;
		double bus_voltage;
		double output_voltage;
		if ((last_status_4_time + status_4_period) < ros_time_now)
		{
			bus_voltage = talon->GetBusVoltage();
			safeTalonCall(talon->GetLastError(), "GetBusVoltage");

			temperature = talon->GetTemperature(); //returns in Celsius
			safeTalonCall(talon->GetLastError(), "GetTemperature");

			// TODO : not sure about this one being in status 4
			output_voltage = talon->GetMotorOutputVoltage();
			safeTalonCall(talon->GetLastError(), "GetMotorOutputVoltage");

			update_status_4 = true;
			last_status_4_time = ros_time_now;
		}

		//closed-loop
		bool update_status_13 = false;
		double closed_loop_error;
		double integral_accumulator;
		double error_derivative;
		double closed_loop_target;

		if ((talon_mode == hardware_interface::TalonMode_Position) ||
			(talon_mode == hardware_interface::TalonMode_Velocity) ||
			(talon_mode == hardware_interface::TalonMode_Current ) ||
			(talon_mode == hardware_interface::TalonMode_MotionProfile) ||
			(talon_mode == hardware_interface::TalonMode_MotionMagic))
		{
			// PIDF0 Status 13 - 160 mSec default
			if ((last_status_13_time + status_13_period) < ros_time_now)
			{
				const double closed_loop_scale = getConversionFactor(encoder_ticks_per_rotation, encoder_feedback, talon_mode) * conversion_factor;

				closed_loop_error = talon->GetClosedLoopError(pidIdx) * closed_loop_scale;
				safeTalonCall(talon->GetLastError(), "GetClosedLoopError");

				integral_accumulator = talon->GetIntegralAccumulator(pidIdx) * closed_loop_scale;
				safeTalonCall(talon->GetLastError(), "GetIntegralAccumulator");

				error_derivative = talon->GetErrorDerivative(pidIdx) * closed_loop_scale;
				safeTalonCall(talon->GetLastError(), "GetErrorDerivative");

				// Not sure of timing on this?
				const double closed_loop_target = talon->GetClosedLoopTarget(pidIdx) * closed_loop_scale;
				safeTalonCall(talon->GetLastError(), "GetClosedLoopTarget");
				state->setClosedLoopTarget(closed_loop_target);

				// Reverse engineer the individual P,I,D,F components used
				// to generate closed-loop control signals to the motor
				// This is just for debugging PIDF tuning
				const int pidf_slot = state->getSlot();
				const double kp = state->getPidfP(pidf_slot);
				const double ki = state->getPidfI(pidf_slot);
				const double kd = state->getPidfD(pidf_slot);
				const double kf = state->getPidfF(pidf_slot);

				const double native_closed_loop_error = closed_loop_error / closed_loop_scale;
				state->setPTerm(native_closed_loop_error * kp);
				state->setITerm(integral_accumulator * ki);
				state->setDTerm(error_derivative * kd);
				state->setFTerm(closed_loop_target / closed_loop_scale * kf);

				update_status_13 = true;
				last_status_13_time = ros_time_now;
			}
		}

		bool update_status_10 = false;
		double active_trajectory_position;
		double active_trajectory_velocity;
		double active_trajectory_heading;
		// Targets Status 10 - 160 mSec default
		if (((talon_mode == hardware_interface::TalonMode_MotionProfile) ||
			 (talon_mode == hardware_interface::TalonMode_MotionMagic))  &&
			((last_status_10_time + status_10_period) < ros_time_now) )
		{
			active_trajectory_position = talon->GetActiveTrajectoryPosition() * radians_scale;
			safeTalonCall(talon->GetLastError(), "GetActiveTrajectoryPosition");

			active_trajectory_velocity = talon->GetActiveTrajectoryVelocity() * radians_per_second_scale;
			safeTalonCall(talon->GetLastError(), "GetActiveTrajectoryVelocity");

			active_trajectory_heading = talon->GetActiveTrajectoryHeading() * 2. * M_PI / 360.; //returns in degrees
			safeTalonCall(talon->GetLastError(), "GetActiveTrajectoryHeading");

			update_status_10 = true;
			last_status_10_time = ros_time_now;
		}

		bool update_status_9 = false;
		int  mp_top_level_buffer_count;
		if ((talon_mode == hardware_interface::TalonMode_MotionProfile) &&
			(last_status_9_time + status_9_period) < ros_time_now)
		{
			mp_top_level_buffer_count = talon->GetMotionProfileTopLevelBufferCount();
			ctre::phoenix::motion::MotionProfileStatus talon_status;
			safeTalonCall(talon->GetMotionProfileStatus(talon_status), "GetMotionProfileStatus");

			internal_status.topBufferRem = talon_status.topBufferRem;
			internal_status.topBufferCnt = talon_status.topBufferCnt;
			internal_status.btmBufferCnt = talon_status.btmBufferCnt;
			internal_status.hasUnderrun = talon_status.hasUnderrun;
			internal_status.isUnderrun = talon_status.isUnderrun;
			internal_status.activePointValid = talon_status.activePointValid;
			internal_status.isLast = talon_status.isLast;
			internal_status.profileSlotSelect0 = talon_status.profileSlotSelect0;
			internal_status.profileSlotSelect1 = talon_status.profileSlotSelect1;
			internal_status.outputEnable = static_cast<hardware_interface::SetValueMotionProfile>(talon_status.outputEnable);
			internal_status.timeDurMs = talon_status.timeDurMs;
			update_status_9 = true;
			last_status_9_time = ros_time_now;
		}

		// SensorCollection - 100msec default
		bool update_sensor_collection = false;
		bool forward_limit_switch;
		bool reverse_limit_switch;
		if ((last_sensor_collection_time + sensor_collection_period) < ros_time_now)
		{
			auto sensor_collection = talon->GetSensorCollection();
			forward_limit_switch = sensor_collection.IsFwdLimitSwitchClosed();
			reverse_limit_switch = sensor_collection.IsRevLimitSwitchClosed();

			update_sensor_collection = true;
			last_sensor_collection_time = ros_time_now;
		}

		// Actually update the TalonHWState shared between
		// this thread and read()
		// Do this all at once so the code minimizes the amount
		// of time with mutex locked
		{
			// Lock the state entry to make sure writes
			// are atomic - reads won't grab data in
			// the middle of a write
			std::lock_guard<std::mutex> l(*mutex);

			if (update_mp_status || update_status_9)
			{
				state->setMotionProfileStatus(internal_status);
				state->setMotionProfileTopLevelBufferCount(mp_top_level_buffer_count);
			}

			if (update_status_1)
			{
				state->setMotorOutputPercent(motor_output_percent);
				state->setFaults(faults.ToBitfield());

				state->setForwardSoftlimitHit(faults.ForwardSoftLimit);
				state->setReverseSoftlimitHit(faults.ReverseSoftLimit);
			}

			if (update_status_2)
			{
				state->setPosition(position);
				state->setSpeed(velocity);
				state->setOutputCurrent(output_current);
				state->setStickyFaults(sticky_faults.ToBitfield());
			}

			if (update_status_4)
			{
				state->setBusVoltage(bus_voltage);
				state->setTemperature(temperature);
			}

			if ((talon_mode == hardware_interface::TalonMode_Position) ||
				(talon_mode == hardware_interface::TalonMode_Velocity) ||
				(talon_mode == hardware_interface::TalonMode_Current ) ||
				(talon_mode == hardware_interface::TalonMode_MotionProfile) ||
				(talon_mode == hardware_interface::TalonMode_MotionMagic))
			{
				if (update_status_13)
				{
					state->setClosedLoopError(closed_loop_error);
					state->setIntegralAccumulator(integral_accumulator);
					state->setErrorDerivative(error_derivative);
					if ((talon_mode != hardware_interface::TalonMode_MotionProfile) &&
						(talon_mode != hardware_interface::TalonMode_MotionMagic))
					{
						state->setClosedLoopTarget(closed_loop_target);
					}
				}
			}

			if ((talon_mode == hardware_interface::TalonMode_MotionProfile) ||
				(talon_mode == hardware_interface::TalonMode_MotionMagic))
			{
				if (update_status_10)
				{
					state->setActiveTrajectoryPosition(active_trajectory_position);
					state->setActiveTrajectoryVelocity(active_trajectory_velocity);
					state->setActiveTrajectoryHeading(active_trajectory_heading);
				}
			}

			state->setFaults(faults.ToBitfield());

			if (update_sensor_collection)
			{
				state->setForwardLimitSwitch(forward_limit_switch);
				state->setReverseLimitSwitch(reverse_limit_switch);
			}
		}
		struct timespec end_time;
		clock_gettime(CLOCK_MONOTONIC, &end_time);
		time_sum +=
			((double)end_time.tv_sec -  (double)start_time.tv_sec) +
			((double)end_time.tv_nsec - (double)start_time.tv_nsec) / 1000000000.;
		iteration_count += 1;
		ROS_INFO_STREAM_THROTTLE(2, "Read thread " << can_id << " = " << time_sum / iteration_count);
		rate.sleep();
	}
}

// The PDP reads happen in their own thread. This thread
// loops at 20Hz to match the update rate of PDP CAN
// status messages.  Each iteration, data read from the
// PDP is copied to a state buffer shared with the main read
// thread.
void FRCRobotHWInterface::pdp_read_thread(int32_t pdp,
		std::shared_ptr<hardware_interface::PDPHWState> state,
		std::shared_ptr<std::mutex> mutex)
{
	ros::Rate r(20); // TODO : Tune me?
	int32_t status = 0;
	double time_sum = 0.;
	unsigned iteration_count = 0;
	HAL_ClearPDPStickyFaults(pdp, &status);
	HAL_ResetPDPTotalEnergy(pdp, &status);
	if (status)
		ROS_ERROR_STREAM("pdp_read_thread error clearing sticky faults : status = " << status);
	while (ros::ok())
	{
		struct timespec start_time;
		clock_gettime(CLOCK_MONOTONIC, &start_time);
#ifdef USE_TALON_MOTION_PROFILE
		if (!profile_is_live_.load(std::memory_order_relaxed) &&
			!writing_points_.load(std::memory_order_relaxed))
#endif
		{
			//read info from the PDP hardware
			status = 0;
			hardware_interface::PDPHWState pdp_state;
			pdp_state.setVoltage(HAL_GetPDPVoltage(pdp, &status));
			pdp_state.setTemperature(HAL_GetPDPTemperature(pdp, &status));
			pdp_state.setTotalCurrent(HAL_GetPDPTotalCurrent(pdp, &status));
			pdp_state.setTotalPower(HAL_GetPDPTotalPower(pdp, &status));
			pdp_state.setTotalEnergy(HAL_GetPDPTotalEnergy(pdp, &status));
			for (int channel = 0; channel <= 15; channel++)
			{
				pdp_state.setCurrent(HAL_GetPDPChannelCurrent(pdp, channel, &status), channel);
			}
			if (status)
				ROS_ERROR_STREAM("pdp_read_thread error : status = " << status);
			else
			{
				// Copy to state shared with read() thread
				std::lock_guard<std::mutex> l(*mutex);
				*state = pdp_state;
			}
		}
		struct timespec end_time;
		clock_gettime(CLOCK_MONOTONIC, &end_time);
		time_sum +=
			((double)end_time.tv_sec -  (double)start_time.tv_sec) +
			((double)end_time.tv_nsec - (double)start_time.tv_nsec) / 1000000000.;
		iteration_count += 1;
		ROS_INFO_STREAM_THROTTLE(2, "pdp_read = " << time_sum / iteration_count);
		r.sleep();
	}
}

// The PCM state reads happen in their own thread. This thread
// loops at 20Hz to match the update rate of PCM CAN
// status messages.  Each iteration, data read from the
// PCM is copied to a state buffer shared with the main read
// thread.
void FRCRobotHWInterface::pcm_read_thread(HAL_CompressorHandle pcm, int32_t pcm_id,
										  std::shared_ptr<hardware_interface::PCMState> state,
										  std::shared_ptr<std::mutex> mutex)
{
	ros::Rate r(20); // TODO : Tune me?
	int32_t status = 0;
	double time_sum = 0.;
	unsigned iteration_count = 0;
	HAL_ClearAllPCMStickyFaults(pcm, &status);
	if (status)
		ROS_ERROR_STREAM("pcm_read_thread error clearing sticky faults : status = " << status);
	while (ros::ok())
	{
		struct timespec start_time;
		clock_gettime(CLOCK_MONOTONIC, &start_time);
#ifdef USE_TALON_MOTION_PROFILE
		if (!profile_is_live_.load(std::memory_order_relaxed) &&
			!writing_points_.load(std::memory_order_relaxed))
#endif
		{
			// TODO : error checking?
			hardware_interface::PCMState pcm_state(pcm_id);
			status = 0;
			pcm_state.setEnabled(HAL_GetCompressor(pcm, &status));
			pcm_state.setPressureSwitch(HAL_GetCompressorPressureSwitch(pcm, &status));
			pcm_state.setCompressorCurrent(HAL_GetCompressorCurrent(pcm, &status));
			pcm_state.setClosedLoopControl(HAL_GetCompressorClosedLoopControl(pcm, &status));
			pcm_state.setCurrentTooHigh(HAL_GetCompressorCurrentTooHighFault(pcm, &status));
			pcm_state.setCurrentTooHighSticky(HAL_GetCompressorCurrentTooHighStickyFault(pcm, &status));

			pcm_state.setShorted(HAL_GetCompressorShortedFault(pcm, &status));
			pcm_state.setShortedSticky(HAL_GetCompressorShortedStickyFault(pcm, &status));
			pcm_state.setNotConntected(HAL_GetCompressorNotConnectedFault(pcm, &status));
			pcm_state.setNotConnecteSticky(HAL_GetCompressorNotConnectedStickyFault(pcm, &status));
			pcm_state.setVoltageFault(HAL_GetPCMSolenoidVoltageFault(pcm, &status));
			pcm_state.setVoltageStickFault(HAL_GetPCMSolenoidVoltageStickyFault(pcm, &status));
			pcm_state.setSolenoidBlacklist(HAL_GetPCMSolenoidBlackList(pcm, &status));

			if (status)
			{
				ROS_ERROR_STREAM("pcm_read_thread error : status = " << status);
			}
			else
			{
				// Copy to state shared with read() thread
				std::lock_guard<std::mutex> l(*mutex);
				*state = pcm_state;
			}
		}
		struct timespec end_time;
		clock_gettime(CLOCK_MONOTONIC, &end_time);
		time_sum +=
			((double)end_time.tv_sec -  (double)start_time.tv_sec) +
			((double)end_time.tv_nsec - (double)start_time.tv_nsec) / 1000000000.;
		iteration_count += 1;
		ROS_INFO_STREAM_THROTTLE(2, "pcm_read = " << time_sum / iteration_count);
		r.sleep();
	}
}

void FRCRobotHWInterface::read(ros::Duration &/*elapsed_time*/)
{
	static double time_sum = 0;
	static int iteration_count = 0;

	struct timespec start_time;
	clock_gettime(CLOCK_MONOTONIC, &start_time);

	if (run_hal_robot_ && !robot_code_ready_)
	{
		bool ready = true;
		// This will be written by the last controller to be
		// spawned - waiting here prevents the robot from
		// reporting robot code ready to the field until
		// all other controllers are started
		for (auto r : robot_ready_signals_)
			ready &= (r != 0);
		if (ready)
		{
			robot_->StartCompetition();
			robot_code_ready_ = true;
		}
	}

	if (robot_code_ready_)
	{
		robot_->OneIteration();

		static double time_sum_nt = 0.;
		static double time_sum_joystick = 0.;
		static unsigned iteration_count_nt = 0;
		static unsigned iteration_count_joystick = 0;

		const ros::Time time_now_t = ros::Time::now();
		const double nt_publish_rate = 10;

		struct timespec start_timespec;
		clock_gettime(CLOCK_MONOTONIC, &start_timespec);

		// Throttle NT updates since these are mainly for human
		// UI and don't have to run at crazy speeds
		if ((last_nt_publish_time_ + ros::Duration(1.0 / nt_publish_rate)) < time_now_t)
		{
			// SmartDashboard works!
			frc::SmartDashboard::PutNumber("navX_angle", navX_angle_);
			frc::SmartDashboard::PutNumber("Pressure", pressure_);
			frc::SmartDashboard::PutBoolean("cube_state", cube_state_ != 0);
			frc::SmartDashboard::PutBoolean("death_0", auto_state_0_ != 0);
			frc::SmartDashboard::PutBoolean("death_1", auto_state_1_ != 0);
			frc::SmartDashboard::PutBoolean("death_2", auto_state_2_ != 0);
			frc::SmartDashboard::PutBoolean("death_3", auto_state_3_ != 0);

			std::shared_ptr<nt::NetworkTable> driveTable = NetworkTable::GetTable("SmartDashboard");  //Access Smart Dashboard Variables
			if (driveTable && realtime_pub_nt_->trylock())
			{
				auto &m = realtime_pub_nt_->msg_;
				m.mode[0] = (int)driveTable->GetNumber("auto_mode_0", 0);
				m.mode[1] = (int)driveTable->GetNumber("auto_mode_1", 0);
				m.mode[2] = (int)driveTable->GetNumber("auto_mode_2", 0);
				m.mode[3] = (int)driveTable->GetNumber("auto_mode_3", 0);
				m.delays[0] = (int)driveTable->GetNumber("delay_0", 0);
				m.delays[1] = (int)driveTable->GetNumber("delay_1", 0);
				m.delays[2] = (int)driveTable->GetNumber("delay_2", 0);
				m.delays[3] = (int)driveTable->GetNumber("delay_3", 0);
				m.position = (int)driveTable->GetNumber("robot_start_position", 0);

				frc::SmartDashboard::PutNumber("auto_mode_0_ret", m.mode[0]);
				frc::SmartDashboard::PutNumber("auto_mode_1_ret", m.mode[1]);
				frc::SmartDashboard::PutNumber("auto_mode_2_ret", m.mode[2]);
				frc::SmartDashboard::PutNumber("auto_mode_3_ret", m.mode[3]);
				frc::SmartDashboard::PutNumber("delay_0_ret", m.delays[0]);
				frc::SmartDashboard::PutNumber("delay_1_ret", m.delays[1]);
				frc::SmartDashboard::PutNumber("delay_2_ret", m.delays[2]);
				frc::SmartDashboard::PutNumber("delay_3_ret", m.delays[3]);
				frc::SmartDashboard::PutNumber("robot_start_position_ret", m.position);

				m.header.stamp = time_now_t;
				realtime_pub_nt_->unlockAndPublish();
			}
			if (driveTable)
			{
				disable_compressor_ = driveTable->GetBoolean("disable_reg", 0);
				frc::SmartDashboard::PutBoolean("disable_reg_ret", disable_compressor_ != 0);
				starting_config_ = driveTable->GetBoolean("starting_config", 0);

				override_arm_limits_ = driveTable->GetBoolean("disable_arm_limits", 0);
				frc::SmartDashboard::PutBoolean("disable_arm_limits_ret", override_arm_limits_ != 0);

				stop_arm_ = driveTable->GetBoolean("stop_arm", 0);

				if(driveTable->GetBoolean("zero_navX", 0) != 0)
					navX_zero_ = (double)driveTable->GetNumber("zero_angle", 0);
				else
					navX_zero_ = -10000;

				if(driveTable->GetBoolean("record_time", 0) != 0 )
				{
					if (!error_msg_last_received_ && realtime_pub_error_->trylock())
					{
						realtime_pub_error_->msg_.data = time_now_t.toSec() - error_pub_start_time_;
						realtime_pub_error_->unlockAndPublish();
						error_msg_last_received_ = true;
					}
				}
				else
					error_msg_last_received_ = false;
			}

			last_nt_publish_time_ += ros::Duration(1.0 / nt_publish_rate);
		}

		struct timespec end_time;
		clock_gettime(CLOCK_MONOTONIC, &end_time);
		time_sum_nt +=
			((double)end_time.tv_sec -  (double)start_timespec.tv_sec) +
			((double)end_time.tv_nsec - (double)start_timespec.tv_nsec) / 1000000000.;
		iteration_count_nt += 1;

		start_timespec = end_time;

		// Update joystick state as often as possible
		if ((joysticks_.size() > 0) && realtime_pub_joystick_->trylock())
		{
			auto &m = realtime_pub_joystick_->msg_;
			m.header.stamp = time_now_t;

			m.rightStickY = joysticks_[0]->GetRawAxis(5);
			m.rightStickX = joysticks_[0]->GetRawAxis(4);
			m.leftStickY = joysticks_[0]->GetRawAxis(1);
			m.leftStickX = joysticks_[0]->GetRawAxis(0);

			m.leftTrigger = joysticks_[0]->GetRawAxis(2);
			m.rightTrigger = joysticks_[0]->GetRawAxis(3);
			m.buttonXButton = joysticks_[0]->GetRawButton(3);
			m.buttonXPress = joysticks_[0]->GetRawButtonPressed(3);
			m.buttonXRelease = joysticks_[0]->GetRawButtonReleased(3);
			m.buttonYButton = joysticks_[0]->GetRawButton(4);
			m.buttonYPress = joysticks_[0]->GetRawButtonPressed(4);
			m.buttonYRelease = joysticks_[0]->GetRawButtonReleased(4);

			m.bumperLeftButton = joysticks_[0]->GetRawButton(5);
			m.bumperLeftPress = joysticks_[0]->GetRawButtonPressed(5);
			m.bumperLeftRelease = joysticks_[0]->GetRawButtonReleased(5);

			m.bumperRightButton = joysticks_[0]->GetRawButton(6);
			m.bumperRightPress = joysticks_[0]->GetRawButtonPressed(6);
			m.bumperRightRelease = joysticks_[0]->GetRawButtonReleased(6);

			m.stickLeftButton = joysticks_[0]->GetRawButton(9);
			m.stickLeftPress = joysticks_[0]->GetRawButtonPressed(9);
			m.stickLeftRelease = joysticks_[0]->GetRawButtonReleased(9);

			m.stickRightButton = joysticks_[0]->GetRawButton(10);
			m.stickRightPress = joysticks_[0]->GetRawButtonPressed(10);
			m.stickRightRelease = joysticks_[0]->GetRawButtonReleased(10);

			m.buttonAButton = joysticks_[0]->GetRawButton(1);
			m.buttonAPress = joysticks_[0]->GetRawButtonPressed(1);
			m.buttonARelease = joysticks_[0]->GetRawButtonReleased(1);
			m.buttonBButton = joysticks_[0]->GetRawButton(2);
			m.buttonBPress = joysticks_[0]->GetRawButtonPressed(2);
			m.buttonBRelease = joysticks_[0]->GetRawButtonReleased(2);
			m.buttonBackButton = joysticks_[0]->GetRawButton(7);
			m.buttonBackPress = joysticks_[0]->GetRawButtonPressed(7);
			m.buttonBackRelease = joysticks_[0]->GetRawButtonReleased(7);

			m.buttonStartButton = joysticks_[0]->GetRawButton(8);
			m.buttonStartPress = joysticks_[0]->GetRawButtonPressed(8);
			m.buttonStartRelease = joysticks_[0]->GetRawButtonReleased(8);

			bool joystick_up = false;
			bool joystick_down = false;
			bool joystick_left = false;
			bool joystick_right = false;
			switch (joysticks_[0]->GetPOV(0))
			{
				case 0 :
						joystick_up = true;
						break;
				case 45:
						joystick_up = true;
						joystick_right = true;
						break;
				case 90:
						joystick_right = true;
						break;
				case 135:
						joystick_down = true;
						joystick_right = true;
						break;
				case 180:
						joystick_down = true;
						break;
				case 225:
						joystick_down = true;
						joystick_left = true;
						break;
				case 270:
						joystick_left = true;
						break;
				case 315:
						joystick_up = true;
						joystick_left = true;
						break;
			}

			m.directionUpButton = joystick_up;
			m.directionUpPress = joystick_up && !joystick_up_last_[0];
			m.directionUpRelease = !joystick_up && joystick_up_last_[0];

			m.directionDownButton = joystick_down;
			m.directionDownPress = joystick_down && !joystick_down_last_[0];
			m.directionDownRelease = !joystick_down && joystick_down_last_[0];

			m.directionLeftButton = joystick_left;
			m.directionLeftPress = joystick_left && !joystick_left_last_[0];
			m.directionLeftRelease = !joystick_left && joystick_left_last_[0];

			m.directionRightButton = joystick_right;
			m.directionRightPress = joystick_right && !joystick_right_last_[0];
			m.directionRightRelease = !joystick_right && joystick_right_last_[0];

			joystick_up_last_[0] = joystick_up;
			joystick_down_last_[0] = joystick_down;
			joystick_left_last_[0] = joystick_left;
			joystick_right_last_[0] = joystick_right;

			realtime_pub_joystick_->unlockAndPublish();
		}
		clock_gettime(CLOCK_MONOTONIC, &end_time);
		time_sum_joystick +=
			((double)end_time.tv_sec -  (double)start_timespec.tv_sec) +
			((double)end_time.tv_nsec - (double)start_timespec.tv_nsec) / 1000000000.;
		iteration_count_joystick += 1;

		ROS_INFO_STREAM_THROTTLE(2, "hw_keepalive nt = " << time_sum_nt / iteration_count_nt
				<< " joystick = " << time_sum_joystick / iteration_count_joystick);

		int32_t status = 0;
		match_data_.setMatchTimeRemaining(HAL_GetMatchTime(&status));
		HAL_MatchInfo info;
		HAL_GetMatchInfo(&info);

		match_data_.setGameSpecificData(std::string(reinterpret_cast<char*>(info.gameSpecificMessage),
                     info.gameSpecificMessageSize));
		match_data_.setEventName(info.eventName);

		status = 0;
		auto allianceStationID = HAL_GetAllianceStation(&status);
		DriverStation::Alliance color;
		switch (allianceStationID) {
			case HAL_AllianceStationID_kRed1:
			case HAL_AllianceStationID_kRed2:
			case HAL_AllianceStationID_kRed3:
				color = DriverStation::kRed;
				break;
			case HAL_AllianceStationID_kBlue1:
			case HAL_AllianceStationID_kBlue2:
			case HAL_AllianceStationID_kBlue3:
				color = DriverStation::kBlue;
				break;
			default:
				color = DriverStation::kInvalid;
		}
		match_data_.setAllianceColor(color);

		match_data_.setMatchType(static_cast<DriverStation::MatchType>(info.matchType));

		int station_location;
		switch (allianceStationID) {
			case HAL_AllianceStationID_kRed1:
			case HAL_AllianceStationID_kBlue1:
				station_location = 1;
				break;
			case HAL_AllianceStationID_kRed2:
			case HAL_AllianceStationID_kBlue2:
				station_location = 2;
				break;
			case HAL_AllianceStationID_kRed3:
			case HAL_AllianceStationID_kBlue3:
				station_location = 3;
				break;
			default:
				station_location = 0;
		}
		match_data_.setDriverStationLocation(station_location);

		match_data_.setMatchNumber(info.matchNumber);
		match_data_.setReplayNumber(info.replayNumber);

		HAL_ControlWord controlWord;
		HAL_GetControlWord(&controlWord);
		match_data_.setEnabled(controlWord.enabled && controlWord.dsAttached);
		match_data_.setDisabled(!(controlWord.enabled && controlWord.dsAttached));
		match_data_.setAutonomous(controlWord.autonomous);
		match_data_.setOperatorControl(!(controlWord.autonomous || controlWord.test));
		match_data_.setTest(controlWord.test);
		match_data_.setDSAttached(controlWord.dsAttached);
		match_data_.setFMSAttached(controlWord.fmsAttached);
		status = 0;
		match_data_.setBatteryVoltage(HAL_GetVinVoltage(&status));

		status = 0;
		robot_controller_state_.SetFPGAVersion(HAL_GetFPGAVersion(&status));
		robot_controller_state_.SetFPGARevision(HAL_GetFPGARevision(&status));
		robot_controller_state_.SetFPGATime(HAL_GetFPGATime(&status));
		robot_controller_state_.SetUserButton(HAL_GetFPGAButton(&status));
		robot_controller_state_.SetIsSysActive(HAL_GetSystemActive(&status));
		robot_controller_state_.SetIsBrownedOut(HAL_GetBrownedOut(&status));
		robot_controller_state_.SetInputVoltage(HAL_GetVinVoltage(&status));
		robot_controller_state_.SetInputCurrent(HAL_GetVinCurrent(&status));
		robot_controller_state_.SetVoltage3V3(HAL_GetUserVoltage3V3(&status));
		robot_controller_state_.SetCurrent3V3(HAL_GetUserCurrent3V3(&status));
		robot_controller_state_.SetEnabled3V3(HAL_GetUserActive3V3(&status));
		robot_controller_state_.SetFaultCount3V3(HAL_GetUserCurrentFaults3V3(&status));
		robot_controller_state_.SetVoltage5V(HAL_GetUserVoltage5V(&status));
		robot_controller_state_.SetCurrent5V(HAL_GetUserCurrent5V(&status));
		robot_controller_state_.SetEnabled5V(HAL_GetUserActive5V(&status));
		robot_controller_state_.SetFaultCount5V(HAL_GetUserCurrentFaults5V(&status));
		robot_controller_state_.SetVoltage6V(HAL_GetUserVoltage6V(&status));
		robot_controller_state_.SetCurrent6V(HAL_GetUserCurrent6V(&status));
		robot_controller_state_.SetEnabled6V(HAL_GetUserActive6V(&status));
		robot_controller_state_.SetFaultCount6V(HAL_GetUserCurrentFaults6V(&status));
		float percent_bus_utilization;
		uint32_t bus_off_count;
		uint32_t tx_full_count;
		uint32_t receive_error_count;
		uint32_t transmit_error_count;
		HAL_CAN_GetCANStatus(&percent_bus_utilization, &bus_off_count,
				&tx_full_count, &receive_error_count,
				&transmit_error_count, &status);

		robot_controller_state_.SetCANPercentBusUtilization(percent_bus_utilization);
		robot_controller_state_.SetCANBusOffCount(bus_off_count);
		robot_controller_state_.SetCANTxFullCount(tx_full_count);
		robot_controller_state_.SetCANReceiveErrorCount(receive_error_count);
		robot_controller_state_.SetCANTransmitErrorCount(transmit_error_count);
	}

	for (std::size_t joint_id = 0; joint_id < num_can_talon_srxs_; ++joint_id)
	{
		if (can_talon_srx_local_hardwares_[joint_id])
		{
			std::lock_guard<std::mutex> l(*talon_read_state_mutexes_[joint_id]);
			auto &ts   = talon_state_[joint_id];
			auto &trts = talon_read_thread_states_[joint_id];

			// Copy config items from talon state to talon_read_thread_state
			// This makes sure config items set by controllers is
			// eventually reflected in the state unique to the
			// talon_read_thread code
			trts->setTalonMode(ts.getTalonMode());
			trts->setEncoderFeedback(ts.getEncoderFeedback());
			trts->setEncoderTicksPerRotation(ts.getEncoderTicksPerRotation());
			trts->setConversionFactor(ts.getConversionFactor());
			for (int i = hardware_interface::Status_1_General; i < hardware_interface::Status_Last; i++)
			{
				const hardware_interface::StatusFrame status_frame = static_cast<hardware_interface::StatusFrame>(i);
				trts->setStatusFramePeriod(status_frame, ts.getStatusFramePeriod(status_frame));
			}
			trts->setEnableReadThread(ts.getEnableReadThread());

			// Copy talon state values read in the read thread into the
			// talon state shared globally with the rest of the hardware
			// interface code
			ts.setPosition(trts->getPosition());
			ts.setSpeed(trts->getSpeed());
			ts.setOutputCurrent(trts->getOutputCurrent());
			ts.setBusVoltage(trts->getBusVoltage());
			ts.setMotorOutputPercent(trts->getMotorOutputPercent());
			ts.setOutputVoltage(trts->getOutputVoltage());
			ts.setTemperature(trts->getTemperature());
			ts.setClosedLoopError(trts->getClosedLoopError());
			ts.setIntegralAccumulator(trts->getIntegralAccumulator());
			ts.setErrorDerivative(trts->getErrorDerivative());
			ts.setClosedLoopTarget(trts->getClosedLoopTarget());
			ts.setActiveTrajectoryPosition(trts->getActiveTrajectoryPosition());
			ts.setActiveTrajectoryVelocity(trts->getActiveTrajectoryVelocity());
			ts.setActiveTrajectoryHeading(trts->getActiveTrajectoryHeading());
			ts.setMotionProfileTopLevelBufferCount(trts->getMotionProfileTopLevelBufferCount());
			ts.setMotionProfileStatus(trts->getMotionProfileStatus());
			ts.setFaults(trts->getFaults());
			ts.setForwardLimitSwitch(trts->getForwardLimitSwitch());
			ts.setReverseLimitSwitch(trts->getReverseLimitSwitch());
			ts.setForwardSoftlimitHit(trts->getForwardSoftlimitHit());
			ts.setReverseSoftlimitHit(trts->getReverseSoftlimitHit());
			ts.setStickyFaults(trts->getStickyFaults());
		}
	}

	for (size_t i = 0; i < num_nidec_brushlesses_; i++)
	{
		if (nidec_brushless_local_updates_[i])
			brushless_vel_[i] = nidec_brushlesses_[i]->Get();
	}
	for (size_t i = 0; i < num_digital_inputs_; i++)
	{
		//State should really be a bool - but we're stuck using
		//ROS control code which thinks everything to and from
		//hardware are doubles
		if (digital_input_locals_[i])
			digital_input_state_[i] = (digital_inputs_[i]->Get()^digital_input_inverts_[i]) ? 1 : 0;
	}
#if 0
	for (size_t i = 0; i < num_digital_outputs_; i++)
	{
		if (!digital_output_local_updates_[i])
			digital_output_state_[i] = digital_output_command_[i];
	}
	for (size_t i = 0; i < num_pwm_; i++)
	{
		// Just reflect state of output in status
		if (!pwm_local_updates_[i])
			pwm_state_[i] = pwm_command_[i];
	}
	for (size_t i = 0; i < num_solenoids_; i++)
	{
		if (!solenoid_local_updates_[i])
			solenoid_state_[i] = solenoid_command_[i];
	}
	for (size_t i = 0; i < num_double_solenoids_; i++)
	{
		if (!double_solenoid_local_updates_[i])
			double_solenoid_state_[i] = double_solenoid_command_[i];
	}
#endif
	for (size_t i = 0; i < num_analog_inputs_; i++)
	{
		if (analog_input_locals_[i])
			analog_input_state_[i] = analog_inputs_[i]->GetValue() *analog_input_a_[i] + analog_input_b_[i];

		if (analog_input_names_[i] == "analog_pressure_sensor")
			pressure_ = analog_input_state_[i];
	}
	//navX read here
	for (size_t i = 0; i < num_navX_; i++)
	{
		if (navX_locals_[i])
		{
			// TODO : double check we're reading
			// the correct data

			// navXs_[i]->GetFusedHeading();
			// navXs_[i]->GetPitch();
			// navXs_[i]->GetRoll();

			// TODO : Fill in imu_angular_velocity[i][]

			//navXs_[i]->IsCalibrating();
			//navXs_[i]->IsConnected();
			//navXs_[i]->GetLastSensorTimestamp();
			//
			imu_linear_accelerations_[i][0] = navXs_[i]->GetWorldLinearAccelX();
			imu_linear_accelerations_[i][1] = navXs_[i]->GetWorldLinearAccelY();
			imu_linear_accelerations_[i][2] = navXs_[i]->GetWorldLinearAccelZ();

			//navXs_[i]->IsMoving();
			//navXs_[i]->IsRotating();
			//navXs_[i]->IsMagneticDisturbance();
			//navXs_[i]->IsMagnetometerCalibrated();
			//
			tf2::Quaternion tempQ;
			if(i == 0)
			{
				if(navX_zero_ != -10000)
					offset_navX_[i] = navX_zero_ - navXs_[i]->GetFusedHeading() / 360. * 2. * M_PI;

				// For display on the smartdash
				navX_angle_ = navXs_[i]->GetFusedHeading() / 360. * 2. * M_PI + offset_navX_[i];
			}
			tempQ.setRPY(navXs_[i]->GetRoll() / -360 * 2 * M_PI, navXs_[i]->GetPitch() / -360 * 2 * M_PI, navXs_[i]->GetFusedHeading() / 360 * 2 * M_PI + offset_navX_[i]  );

			imu_orientations_[i][3] = tempQ.w();
			imu_orientations_[i][0] = tempQ.x();
			imu_orientations_[i][1] = tempQ.y();
			imu_orientations_[i][2] = tempQ.z();

			imu_angular_velocities_[i][0] = navXs_[i]->GetVelocityX();
			imu_angular_velocities_[i][1] = navXs_[i]->GetVelocityY();
			imu_angular_velocities_[i][2] = navXs_[i]->GetVelocityZ();

			//navXs_[i]->GetDisplacementX();
			//navXs_[i]->GetDisplacementY();
			//navXs_[i]->GetDisplacementZ();
			//navXs_[i]->GetAngle(); //continous
			//TODO: add setter functions

			navX_state_[i] = offset_navX_[i];
		}
	}

	for (size_t i = 0; i < num_compressors_; i++)
	{
		if (compressor_local_updates_[i])
		{
			std::lock_guard<std::mutex> l(*pcm_read_thread_mutexes_[i]);
			pcm_state_[i] = *pcm_read_thread_state_[i];
		}
	}
	for (size_t i = 0; i < num_pdps_; i++)
	{
		if (pdp_locals_[i])
		{
			std::lock_guard<std::mutex> l(*pdp_read_thread_mutexes_[i]);
			pdp_state_[i] = *pdp_read_thread_state_[i];
		}
	}

	struct timespec end_time;
	clock_gettime(CLOCK_MONOTONIC, &end_time);
	time_sum +=
		((double)end_time.tv_sec -  (double)start_time.tv_sec) +
		((double)end_time.tv_nsec - (double)start_time.tv_nsec) / 1000000000.;
	iteration_count += 1;
	ROS_INFO_STREAM_THROTTLE(2, "read() = " << time_sum / iteration_count);
}


double FRCRobotHWInterface::getConversionFactor(int encoder_ticks_per_rotation,
						hardware_interface::FeedbackDevice encoder_feedback,
						hardware_interface::TalonMode talon_mode)
{
	if((talon_mode == hardware_interface::TalonMode_Position) ||
	   (talon_mode == hardware_interface::TalonMode_MotionMagic)) // TODO - maybe motion profile as well?
	{
		switch (encoder_feedback)
		{
			case hardware_interface::FeedbackDevice_Uninitialized:
				return 1.;
			case hardware_interface::FeedbackDevice_QuadEncoder:
			case hardware_interface::FeedbackDevice_PulseWidthEncodedPosition:
				return 2 * M_PI / encoder_ticks_per_rotation;
			case hardware_interface::FeedbackDevice_Analog:
				return 2 * M_PI / 1024;
			case hardware_interface::FeedbackDevice_Tachometer:
			case hardware_interface::FeedbackDevice_SensorSum:
			case hardware_interface::FeedbackDevice_SensorDifference:
			case hardware_interface::FeedbackDevice_RemoteSensor0:
			case hardware_interface::FeedbackDevice_RemoteSensor1:
			case hardware_interface::FeedbackDevice_SoftwareEmulatedSensor:
				//ROS_WARN_STREAM("Unable to convert units.");
				return 1.;
			default:
				ROS_WARN_STREAM("Invalid encoder feedback device. Unable to convert units.");
				return 1.;
		}
	}
	else if(talon_mode == hardware_interface::TalonMode_Velocity)
	{
		switch (encoder_feedback)
		{
			case hardware_interface::FeedbackDevice_Uninitialized:
				return 1.;
			case hardware_interface::FeedbackDevice_QuadEncoder:
			case hardware_interface::FeedbackDevice_PulseWidthEncodedPosition:
				return 2 * M_PI / encoder_ticks_per_rotation / .1;
			case hardware_interface::FeedbackDevice_Analog:
				return 2 * M_PI / 1024 / .1;
			case hardware_interface::FeedbackDevice_Tachometer:
			case hardware_interface::FeedbackDevice_SensorSum:
			case hardware_interface::FeedbackDevice_SensorDifference:
			case hardware_interface::FeedbackDevice_RemoteSensor0:
			case hardware_interface::FeedbackDevice_RemoteSensor1:
			case hardware_interface::FeedbackDevice_SoftwareEmulatedSensor:
				//ROS_WARN_STREAM("Unable to convert units.");
				return 1.;
			default:
				ROS_WARN_STREAM("Invalid encoder feedback device. Unable to convert units.");
				return 1.;
		}
	}
	else
	{
		//ROS_WARN_STREAM("Unable to convert closed loop units.");
		return 1.;
	}
}

bool FRCRobotHWInterface::safeTalonCall(ctre::phoenix::ErrorCode error_code, const std::string &talon_method_name)
{
	std::string error_name;
	switch (error_code)
	{
		case ctre::phoenix::OK :
			return true; // Yay us!

		case ctre::phoenix::CAN_MSG_STALE :
			error_name = "CAN_MSG_STALE/CAN_TX_FULL/TxFailed";
			break;
		case ctre::phoenix::InvalidParamValue :
			error_name = "InvalidParamValue/CAN_INVALID_PARAM";
			break;

		case ctre::phoenix::RxTimeout :
			error_name = "RxTimeout/CAN_MSG_NOT_FOUND";
			break;
		case ctre::phoenix::TxTimeout :
			error_name = "TxTimeout/CAN_NO_MORE_TX_JOBS";
			break;
		case ctre::phoenix::UnexpectedArbId :
			error_name = "UnexpectedArbId/CAN_NO_SESSIONS_AVAIL";
			break;
		case ctre::phoenix::BufferFull :
			error_name = "BufferFull/CAN_OVERFLOW";
			break;
		case ctre::phoenix::SensorNotPresent :
			error_name = "SensorNotPresent";
			break;
		case ctre::phoenix::FirmwareTooOld :
			error_name = "FirmwareTooOld";
			break;
		case ctre::phoenix::CouldNotChangePeriod :
			error_name = "CouldNotChangePeriod";
			break;

		case ctre::phoenix::GENERAL_ERROR :
			error_name = "GENERAL_ERROR";
			break;

		case ctre::phoenix::SIG_NOT_UPDATED :
			error_name = "SIG_NOT_UPDATED";
			break;
		case ctre::phoenix::NotAllPIDValuesUpdated :
			error_name = "NotAllPIDValuesUpdated";
			break;

		case ctre::phoenix::GEN_PORT_ERROR :
			error_name = "GEN_PORT_ERROR";
			break;
		case ctre::phoenix::PORT_MODULE_TYPE_MISMATCH :
			error_name = "PORT_MODULE_TYPE_MISMATCH";
			break;

		case ctre::phoenix::GEN_MODULE_ERROR :
			error_name = "GEN_MODULE_ERROR";
			break;
		case ctre::phoenix::MODULE_NOT_INIT_SET_ERROR :
			error_name = "MODULE_NOT_INIT_SET_ERROR";
			break;
		case ctre::phoenix::MODULE_NOT_INIT_GET_ERROR :
			error_name = "MODULE_NOT_INIT_GET_ERROR";
			break;

		case ctre::phoenix::WheelRadiusTooSmall :
			error_name = "WheelRadiusTooSmall";
			break;
		case ctre::phoenix::TicksPerRevZero :
			error_name = "TicksPerRevZero";
			break;
		case ctre::phoenix::DistanceBetweenWheelsTooSmall :
			error_name = "DistanceBetweenWheelsTooSmall";
			break;
		case ctre::phoenix::GainsAreNotSet :
			error_name = "GainsAreNotSet";
			break;
		case ctre::phoenix::IncompatibleMode :
			error_name = "IncompatibleMode";
			break;
		case ctre::phoenix::InvalidHandle :
			error_name = "InvalidHandle";
			break;

		case ctre::phoenix::FeatureRequiresHigherFirm:
			error_name = "FeatureRequiresHigherFirm";
			break;
		case ctre::phoenix::TalonFeatureRequiresHigherFirm:
			error_name = "TalonFeatureRequiresHigherFirm";
			break;

		case ctre::phoenix::PulseWidthSensorNotPresent :
			error_name = "PulseWidthSensorNotPresent";
			break;
		case ctre::phoenix::GeneralWarning :
			error_name = "GeneralWarning";
			break;
		case ctre::phoenix::FeatureNotSupported :
			error_name = "FeatureNotSupported";
			break;
		case ctre::phoenix::NotImplemented :
			error_name = "NotImplemented";
			break;
		case ctre::phoenix::FirmVersionCouldNotBeRetrieved :
			error_name = "FirmVersionCouldNotBeRetrieved";
			break;
		case ctre::phoenix::FeaturesNotAvailableYet :
			error_name = "FeaturesNotAvailableYet";
			break;
		case ctre::phoenix::ControlModeNotValid :
			error_name = "ControlModeNotValid";
			break;

		case ctre::phoenix::ControlModeNotSupportedYet :
			error_name = "case";
			break;
		case ctre::phoenix::CascadedPIDNotSupporteYet:
			error_name = "CascadedPIDNotSupporteYet/AuxiliaryPIDNotSupportedYet";
			break;
		case ctre::phoenix::RemoteSensorsNotSupportedYet:
			error_name = "RemoteSensorsNotSupportedYet";
			break;
		case ctre::phoenix::MotProfFirmThreshold:
			error_name = "MotProfFirmThreshold";
			break;
		case ctre::phoenix::MotProfFirmThreshold2:
			error_name = "MotProfFirmThreshold2";
			break;

		default:
			{
				std::stringstream s;
				s << "Unknown Talon error " << error_code;
				error_name = s.str();
				break;
			}

	}
	ROS_ERROR_STREAM("Error calling " << talon_method_name << " : " << error_name);
	return false;
}

#define DEBUG_WRITE
void FRCRobotHWInterface::write(ros::Duration &elapsed_time)
{
	// Was the robot enabled last time write was run?
	static bool last_robot_enabled = false;

#ifdef USE_TALON_MOTION_PROFILE
	bool profile_is_live = false;
#endif

	static std::array<double, 250> time_sum{};
	static std::array<int, 250> iteration_count{};
	int time_idx = 0;

	struct timespec start_time;
	clock_gettime(CLOCK_MONOTONIC, &start_time);
	struct timespec end_time;

	for (std::size_t joint_id = 0; joint_id < num_can_talon_srxs_; ++joint_id)
	{
		if (!can_talon_srx_local_hardwares_[joint_id])
			continue;
		//TODO : skip over most or all of this if the talon is in follower mode
		//       Only do the Set() call and then never do anything else?

		// Save some typing by making references to commonly
		// used variables
		auto &talon = can_talons_[joint_id];

		if (!talon) // skip unintialized Talons
			continue;

		auto &ts = talon_state_[joint_id];
		auto &tc = talon_command_[joint_id];

		bool enable_read_thread;
		if (tc.enableReadThreadChanged(enable_read_thread))
			ts.setEnableReadThread(enable_read_thread);

		if (tc.getCustomProfileRun())
		{
			can_talon_srx_run_profile_stop_time_[joint_id] = ros::Time::now().toSec();

			continue; //Don't mess with talons running in custom profile mode
		}

		hardware_interface::FeedbackDevice internal_feedback_device;
		double feedback_coefficient;

		ctre::phoenix::motorcontrol::FeedbackDevice talon_feedback_device;
		if (tc.encoderFeedbackChanged(internal_feedback_device, feedback_coefficient) &&
			convertFeedbackDevice(internal_feedback_device, talon_feedback_device))
		{
			// Check for errors on Talon writes. If it fails, used the reset() call to
			// set the changed var for the config items to true. This will trigger a re-try
			// the next time through the loop.
			bool rc = true;
			rc &= safeTalonCall(talon->ConfigSelectedFeedbackSensor(talon_feedback_device, pidIdx, timeoutMs),"ConfigSelectedFeedbackSensor");
			rc &= safeTalonCall(talon->ConfigSelectedFeedbackCoefficient(feedback_coefficient, pidIdx, timeoutMs),"ConfigSelectedFeedbackCoefficient");
			if (rc)
			{
				ROS_INFO_STREAM("Updated joint " << joint_id << "=" << can_talon_srx_names_[joint_id] << " feedback");
				ts.setEncoderFeedback(internal_feedback_device);
				ts.setFeedbackCoefficient(feedback_coefficient);
			}
			else
			{
				tc.resetEncoderFeedback();
			}
		}
		// 1
	clock_gettime(CLOCK_MONOTONIC, &end_time);
	time_sum[time_idx] +=
		((double)end_time.tv_sec -  (double)start_time.tv_sec) +
		((double)end_time.tv_nsec - (double)start_time.tv_nsec) / 1000000000.;
	iteration_count[time_idx] += 1;
	time_idx += 1;
	start_time = end_time;

		// Get mode that is about to be commanded
		const hardware_interface::TalonMode talon_mode = tc.getMode();
		const int encoder_ticks_per_rotation = tc.getEncoderTicksPerRotation();
		ts.setEncoderTicksPerRotation(encoder_ticks_per_rotation);

		double conversion_factor;
		if (tc.conversionFactorChanged(conversion_factor))
			ts.setConversionFactor(conversion_factor);

		//2
	clock_gettime(CLOCK_MONOTONIC, &end_time);
	time_sum[time_idx] +=
		((double)end_time.tv_sec -  (double)start_time.tv_sec) +
		((double)end_time.tv_nsec - (double)start_time.tv_nsec) / 1000000000.;
	iteration_count[time_idx] += 1;
	time_idx += 1;
	start_time = end_time;
		const double radians_scale = getConversionFactor(encoder_ticks_per_rotation, internal_feedback_device, hardware_interface::TalonMode_Position) * conversion_factor;
		const double radians_per_second_scale = getConversionFactor(encoder_ticks_per_rotation, internal_feedback_device, hardware_interface::TalonMode_Velocity) * conversion_factor;
		const double closed_loop_scale = getConversionFactor(encoder_ticks_per_rotation, internal_feedback_device, talon_mode) * conversion_factor;

		bool close_loop_mode = false;
		bool motion_profile_mode = false;

		if ((talon_mode == hardware_interface::TalonMode_Position) ||
		    (talon_mode == hardware_interface::TalonMode_Velocity) ||
		    (talon_mode == hardware_interface::TalonMode_Current ))
		{
			close_loop_mode = true;
		}
		else if ((talon_mode == hardware_interface::TalonMode_MotionProfile) ||
			     (talon_mode == hardware_interface::TalonMode_MotionMagic))
		{
			close_loop_mode = true;
			motion_profile_mode = true;
		}

		if (close_loop_mode)
		{
			int slot;
			const bool slot_changed = tc.slotChanged(slot);

			double p;
			double i;
			double d;
			double f;
			int    iz;
			int    allowable_closed_loop_error;
			double max_integral_accumulator;
			double closed_loop_peak_output;
			int    closed_loop_period;

			if (tc.pidfChanged(p, i, d, f, iz, allowable_closed_loop_error, max_integral_accumulator, closed_loop_peak_output, closed_loop_period, slot) || ros::Time::now().toSec() - can_talon_srx_run_profile_stop_time_[joint_id] < .2)
			{
				bool rc = true;
				rc &= safeTalonCall(talon->Config_kP(slot, p, timeoutMs),"Config_kP");
				rc &= safeTalonCall(talon->Config_kI(slot, i, timeoutMs),"Config_kI");
				rc &= safeTalonCall(talon->Config_kD(slot, d, timeoutMs),"Config_kD");
				rc &= safeTalonCall(talon->Config_kF(slot, f, timeoutMs),"Config_kF");
				rc &= safeTalonCall(talon->Config_IntegralZone(slot, iz, timeoutMs),"Config_IntegralZone");
				// TODO : Scale these two?
				rc &= safeTalonCall(talon->ConfigAllowableClosedloopError(slot, allowable_closed_loop_error, timeoutMs),"ConfigAllowableClosedloopError");
				rc &= safeTalonCall(talon->ConfigMaxIntegralAccumulator(slot, max_integral_accumulator, timeoutMs),"ConfigMaxIntegralAccumulator");
				rc &= safeTalonCall(talon->ConfigClosedLoopPeakOutput(slot, closed_loop_peak_output, timeoutMs),"ConfigClosedLoopPeakOutput");
				rc &= safeTalonCall(talon->ConfigClosedLoopPeriod(slot, closed_loop_period, timeoutMs),"ConfigClosedLoopPeriod");

				if (rc)
				{
					ROS_INFO_STREAM("Updated joint " << joint_id << "=" << can_talon_srx_names_[joint_id] <<" PIDF slot " << slot << " config values");
					ts.setPidfP(p, slot);
					ts.setPidfI(i, slot);
					ts.setPidfD(d, slot);
					ts.setPidfF(f, slot);
					ts.setPidfIzone(iz, slot);
					ts.setAllowableClosedLoopError(allowable_closed_loop_error, slot);
					ts.setMaxIntegralAccumulator(max_integral_accumulator, slot);
					ts.setClosedLoopPeakOutput(closed_loop_peak_output, slot);
					ts.setClosedLoopPeriod(closed_loop_period, slot);
				}
				else
				{
					tc.resetPIDF(slot);
				}
			}

			bool aux_pid_polarity;
			if (tc.auxPidPolarityChanged(aux_pid_polarity))
			{
				if (safeTalonCall(talon->ConfigAuxPIDPolarity(aux_pid_polarity, timeoutMs), "ConfigAuxPIDPolarity"))
				{
					ROS_INFO_STREAM("Updated joint " << joint_id << " PIDF polarity to " << aux_pid_polarity << std::endl);
					ts.setAuxPidPolarity(aux_pid_polarity);
				}
				else
				{
					tc.resetAuxPidPolarity();
				}
			}

			if (slot_changed)
			{
				if (safeTalonCall(talon->SelectProfileSlot(slot, pidIdx),"SelectProfileSlot"))
				{
					ROS_INFO_STREAM("Updated joint " << joint_id << " PIDF slot to " << slot << std::endl);
					ts.setSlot(slot);
				}
				else
				{
					tc.resetPidfSlot();
				}
			}
		}

		//3
	clock_gettime(CLOCK_MONOTONIC, &end_time);
	time_sum[time_idx] +=
		((double)end_time.tv_sec -  (double)start_time.tv_sec) +
		((double)end_time.tv_nsec - (double)start_time.tv_nsec) / 1000000000.;
	iteration_count[time_idx] += 1;
	time_idx += 1;
	start_time = end_time;
		bool invert;
		bool sensor_phase;
		if (tc.invertChanged(invert, sensor_phase))
		{
			ROS_INFO_STREAM("Updated joint " << joint_id << "=" << can_talon_srx_names_[joint_id] <<
							" invert = " << invert << " phase = " << sensor_phase);
			// TODO : can these calls fail. If so, what to do if they do?
			talon->SetInverted(invert);
			safeTalonCall(talon->GetLastError(), "SetInverted");
			talon->SetSensorPhase(sensor_phase);
			safeTalonCall(talon->GetLastError(), "SetSensorPhase");
			ts.setInvert(invert);
			ts.setSensorPhase(sensor_phase);
		}
		//4
	clock_gettime(CLOCK_MONOTONIC, &end_time);
	time_sum[time_idx] +=
		((double)end_time.tv_sec -  (double)start_time.tv_sec) +
		((double)end_time.tv_nsec - (double)start_time.tv_nsec) / 1000000000.;
	iteration_count[time_idx] += 1;
	time_idx += 1;
	start_time = end_time;

		hardware_interface::NeutralMode neutral_mode;
		ctre::phoenix::motorcontrol::NeutralMode ctre_neutral_mode;
		if (tc.neutralModeChanged(neutral_mode) &&
			convertNeutralMode(neutral_mode, ctre_neutral_mode))
		{

			ROS_INFO_STREAM("Updated joint " << joint_id << "=" << can_talon_srx_names_[joint_id] <<" neutral mode");
			talon->SetNeutralMode(ctre_neutral_mode);
			safeTalonCall(talon->GetLastError(), "SetNeutralMode");
			ts.setNeutralMode(neutral_mode);
		}
		//5
	clock_gettime(CLOCK_MONOTONIC, &end_time);
	time_sum[time_idx] +=
		((double)end_time.tv_sec -  (double)start_time.tv_sec) +
		((double)end_time.tv_nsec - (double)start_time.tv_nsec) / 1000000000.;
	iteration_count[time_idx] += 1;
	time_idx += 1;
	start_time = end_time;

		if (tc.neutralOutputChanged())
		{
			ROS_INFO_STREAM("Set joint " << joint_id << "=" << can_talon_srx_names_[joint_id] <<" neutral output");
			talon->NeutralOutput();
			safeTalonCall(talon->GetLastError(), "NeutralOutput");
			ts.setNeutralOutput(true);
		}

		//6
	clock_gettime(CLOCK_MONOTONIC, &end_time);
	time_sum[time_idx] +=
		((double)end_time.tv_sec -  (double)start_time.tv_sec) +
		((double)end_time.tv_nsec - (double)start_time.tv_nsec) / 1000000000.;
	iteration_count[time_idx] += 1;
	time_idx += 1;
	start_time = end_time;
		double iaccum;
		if (close_loop_mode && tc.integralAccumulatorChanged(iaccum))
		{
			//The units on this aren't really right?
			if (safeTalonCall(talon->SetIntegralAccumulator(iaccum / closed_loop_scale, pidIdx, timeoutMs), "SetIntegralAccumulator"))
			{
				ROS_INFO_STREAM("Updated joint " << joint_id << "=" << can_talon_srx_names_[joint_id] <<" integral accumulator");
				// Do not set talon state - this changes
				// dynamically so read it in read() above instead
			}
			else
				tc.resetIntegralAccumulator();

		}

		//7
	clock_gettime(CLOCK_MONOTONIC, &end_time);
	time_sum[time_idx] +=
		((double)end_time.tv_sec -  (double)start_time.tv_sec) +
		((double)end_time.tv_nsec - (double)start_time.tv_nsec) / 1000000000.;
	iteration_count[time_idx] += 1;
	time_idx += 1;
	start_time = end_time;
		double closed_loop_ramp;
		double open_loop_ramp;
		double peak_output_forward;
		double peak_output_reverse;
		double nominal_output_forward;
		double nominal_output_reverse;
		double neutral_deadband;
		if (tc.outputShapingChanged(closed_loop_ramp,
									open_loop_ramp,
									peak_output_forward,
									peak_output_reverse,
									nominal_output_forward,
									nominal_output_reverse,
									neutral_deadband))
		{
			bool rc = true;
			rc &= safeTalonCall(talon->ConfigOpenloopRamp(open_loop_ramp, timeoutMs),"ConfigOpenloopRamp");
			rc &= safeTalonCall(talon->ConfigClosedloopRamp(closed_loop_ramp, timeoutMs),"ConfigClosedloopRamp");
			rc &= safeTalonCall(talon->ConfigPeakOutputForward(peak_output_forward, timeoutMs),"ConfigPeakOutputForward");          // 100
			rc &= safeTalonCall(talon->ConfigPeakOutputReverse(peak_output_reverse, timeoutMs),"ConfigPeakOutputReverse");          // -100
			rc &= safeTalonCall(talon->ConfigNominalOutputForward(nominal_output_forward, timeoutMs),"ConfigNominalOutputForward"); // 0
			rc &= safeTalonCall(talon->ConfigNominalOutputReverse(nominal_output_reverse, timeoutMs),"ConfigNominalOutputReverse"); // 0
			rc &= safeTalonCall(talon->ConfigNeutralDeadband(neutral_deadband, timeoutMs),"ConfigNeutralDeadband");                 // 0

			if (rc)
			{
				ts.setOpenloopRamp(open_loop_ramp);
				ts.setClosedloopRamp(closed_loop_ramp);
				ts.setPeakOutputForward(peak_output_forward);
				ts.setPeakOutputReverse(peak_output_reverse);
				ts.setNominalOutputForward(nominal_output_forward);
				ts.setNominalOutputReverse(nominal_output_reverse);
				ts.setNeutralDeadband(neutral_deadband);
				ROS_INFO_STREAM("Updated joint " << joint_id << "=" << can_talon_srx_names_[joint_id] <<" output shaping");
			}
			else
			{
				tc.resetOutputShaping();
			}
		}
		//8
	clock_gettime(CLOCK_MONOTONIC, &end_time);
	time_sum[time_idx] +=
		((double)end_time.tv_sec -  (double)start_time.tv_sec) +
		((double)end_time.tv_nsec - (double)start_time.tv_nsec) / 1000000000.;
	iteration_count[time_idx] += 1;
	time_idx += 1;
	start_time = end_time;
		double v_c_saturation;
		int v_measurement_filter;
		bool v_c_enable;
		if (tc.voltageCompensationChanged(v_c_saturation,
										  v_measurement_filter,
										  v_c_enable))
		{
			bool rc = true;
			rc &= safeTalonCall(talon->ConfigVoltageCompSaturation(v_c_saturation, timeoutMs),"ConfigVoltageCompSaturation");
			rc &= safeTalonCall(talon->ConfigVoltageMeasurementFilter(v_measurement_filter, timeoutMs),"ConfigVoltageMeasurementFilter");

			if (rc)
			{
				// Only enable once settings are correctly written to the Talon
				talon->EnableVoltageCompensation(v_c_enable);
				rc &= safeTalonCall(talon->GetLastError(), "EnableVoltageCompensation");
				ROS_INFO_STREAM("Updated joint " << joint_id << "=" << can_talon_srx_names_[joint_id] <<" voltage compensation");

				ts.setVoltageCompensationSaturation(v_c_saturation);
				ts.setVoltageMeasurementFilter(v_measurement_filter);
				ts.setVoltageCompensationEnable(v_c_enable);
			}
			else
			{
				tc.resetVoltageCompensation();
			}
		}

		//9
	clock_gettime(CLOCK_MONOTONIC, &end_time);
	time_sum[time_idx] +=
		((double)end_time.tv_sec -  (double)start_time.tv_sec) +
		((double)end_time.tv_nsec - (double)start_time.tv_nsec) / 1000000000.;
	iteration_count[time_idx] += 1;
	time_idx += 1;
	start_time = end_time;
		hardware_interface::VelocityMeasurementPeriod internal_v_m_period;
		ctre::phoenix::motorcontrol::VelocityMeasPeriod phoenix_v_m_period;
		int v_m_window;

		if (tc.velocityMeasurementChanged(internal_v_m_period, v_m_window) &&
			convertVelocityMeasurementPeriod(internal_v_m_period, phoenix_v_m_period))
		{
			bool rc = true;
			rc &= safeTalonCall(talon->ConfigVelocityMeasurementPeriod(phoenix_v_m_period, timeoutMs),"ConfigVelocityMeasurementPeriod");
			rc &= safeTalonCall(talon->ConfigVelocityMeasurementWindow(v_m_window, timeoutMs),"ConfigVelocityMeasurementWindow");

			if (rc)
			{
				ROS_INFO_STREAM("Updated joint " << joint_id << "=" << can_talon_srx_names_[joint_id] <<" velocity measurement period / window");
				ts.setVelocityMeasurementPeriod(internal_v_m_period);
				ts.setVelocityMeasurementWindow(v_m_window);
			}
			else
			{
				tc.resetVelocityMeasurement();
			}
		}
		//10
	clock_gettime(CLOCK_MONOTONIC, &end_time);
	time_sum[time_idx] +=
		((double)end_time.tv_sec -  (double)start_time.tv_sec) +
		((double)end_time.tv_nsec - (double)start_time.tv_nsec) / 1000000000.;
	iteration_count[time_idx] += 1;
	time_idx += 1;
	start_time = end_time;

		double sensor_position;
		if (tc.sensorPositionChanged(sensor_position))
		{
			if (safeTalonCall(talon->SetSelectedSensorPosition(sensor_position / radians_scale, pidIdx, timeoutMs),
					"SetSelectedSensorPosition"))
			{
				ROS_INFO_STREAM("Updated joint " << joint_id << "=" << can_talon_srx_names_[joint_id] <<" selected sensor position");
			}
			else
			{
				tc.resetSensorPosition();
			}
		}

		hardware_interface::LimitSwitchSource internal_local_forward_source;
		hardware_interface::LimitSwitchNormal internal_local_forward_normal;
		hardware_interface::LimitSwitchSource internal_local_reverse_source;
		hardware_interface::LimitSwitchNormal internal_local_reverse_normal;
		ctre::phoenix::motorcontrol::LimitSwitchSource talon_local_forward_source;
		ctre::phoenix::motorcontrol::LimitSwitchNormal talon_local_forward_normal;
		ctre::phoenix::motorcontrol::LimitSwitchSource talon_local_reverse_source;
		ctre::phoenix::motorcontrol::LimitSwitchNormal talon_local_reverse_normal;
		if (tc.limitSwitchesSourceChanged(internal_local_forward_source, internal_local_forward_normal,
										  internal_local_reverse_source, internal_local_reverse_normal) &&
				convertLimitSwitchSource(internal_local_forward_source, talon_local_forward_source) &&
				convertLimitSwitchNormal(internal_local_forward_normal, talon_local_forward_normal) &&
				convertLimitSwitchSource(internal_local_reverse_source, talon_local_reverse_source) &&
				convertLimitSwitchNormal(internal_local_reverse_normal, talon_local_reverse_normal) )
		{
			bool rc = true;
			rc &= safeTalonCall(talon->ConfigForwardLimitSwitchSource(talon_local_forward_source, talon_local_forward_normal, timeoutMs),"ConfigForwardLimitSwitchSource");
			rc &= safeTalonCall(talon->ConfigReverseLimitSwitchSource(talon_local_reverse_source, talon_local_reverse_normal, timeoutMs),"ConfigReverseLimitSwitchSource");

			if (rc)
			{
				ROS_INFO_STREAM("Updated joint " << joint_id << "=" << can_talon_srx_names_[joint_id] <<" limit switches");
				ts.setForwardLimitSwitchSource(internal_local_forward_source, internal_local_forward_normal);
				ts.setReverseLimitSwitchSource(internal_local_reverse_source, internal_local_reverse_normal);
			}
			else
			{
				tc.resetLimitSwitchesSource();
			}
		}
		//11
	clock_gettime(CLOCK_MONOTONIC, &end_time);
	time_sum[time_idx] +=
		((double)end_time.tv_sec -  (double)start_time.tv_sec) +
		((double)end_time.tv_nsec - (double)start_time.tv_nsec) / 1000000000.;
	iteration_count[time_idx] += 1;
	time_idx += 1;
	start_time = end_time;

		double softlimit_forward_threshold;
		bool softlimit_forward_enable;
		double softlimit_reverse_threshold;
		bool softlimit_reverse_enable;
		bool softlimit_override_enable;
		if (tc.softLimitChanged(softlimit_forward_threshold,
				softlimit_forward_enable,
				softlimit_reverse_threshold,
				softlimit_reverse_enable,
				softlimit_override_enable))
		{
			double softlimit_forward_threshold_NU = softlimit_forward_threshold / radians_scale; //native units
			double softlimit_reverse_threshold_NU = softlimit_reverse_threshold / radians_scale;
			talon->OverrideSoftLimitsEnable(softlimit_override_enable);
			bool rc = true;
			rc &= safeTalonCall(talon->GetLastError(), "OverrideSoftLimitsEnable");
			rc &= safeTalonCall(talon->ConfigForwardSoftLimitThreshold(softlimit_forward_threshold_NU, timeoutMs),"ConfigForwardSoftLimitThreshold");
			rc &= safeTalonCall(talon->ConfigForwardSoftLimitEnable(softlimit_forward_enable, timeoutMs),"ConfigForwardSoftLimitEnable");
			rc &= safeTalonCall(talon->ConfigReverseSoftLimitThreshold(softlimit_reverse_threshold_NU, timeoutMs),"ConfigReverseSoftLimitThreshold");
			rc &= safeTalonCall(talon->ConfigReverseSoftLimitEnable(softlimit_reverse_enable, timeoutMs),"ConfigReverseSoftLimitEnable");

			if (rc)
			{
				ts.setOverrideSoftLimitsEnable(softlimit_override_enable);
				ts.setForwardSoftLimitThreshold(softlimit_forward_threshold);
				ts.setForwardSoftLimitEnable(softlimit_forward_enable);
				ts.setReverseSoftLimitThreshold(softlimit_reverse_threshold);
				ts.setReverseSoftLimitEnable(softlimit_reverse_enable);
				ROS_INFO_STREAM("Updated joint " << joint_id << "=" << can_talon_srx_names_[joint_id] <<" soft limits " <<
						std::endl << "\tforward enable=" << softlimit_forward_enable << " forward threshold=" << softlimit_forward_threshold <<
						std::endl << "\treverse enable=" << softlimit_reverse_enable << " reverse threshold=" << softlimit_reverse_threshold <<
						std::endl << "\toverride_enable=" << softlimit_override_enable);
			}
			else
			{
				tc.resetSoftLimit();
			}
		}

		//12
	clock_gettime(CLOCK_MONOTONIC, &end_time);
	time_sum[time_idx] +=
		((double)end_time.tv_sec -  (double)start_time.tv_sec) +
		((double)end_time.tv_nsec - (double)start_time.tv_nsec) / 1000000000.;
	iteration_count[time_idx] += 1;
	time_idx += 1;
	start_time = end_time;
		int peak_amps;
		int peak_msec;
		int continuous_amps;
		bool enable;
		if (tc.currentLimitChanged(peak_amps, peak_msec, continuous_amps, enable))
		{
			bool rc = true;
			rc &= safeTalonCall(talon->ConfigPeakCurrentLimit(peak_amps, timeoutMs),"ConfigPeakCurrentLimit");
			rc &= safeTalonCall(talon->ConfigPeakCurrentDuration(peak_msec, timeoutMs),"ConfigPeakCurrentDuration");
			rc &= safeTalonCall(talon->ConfigContinuousCurrentLimit(continuous_amps, timeoutMs),"ConfigContinuousCurrentLimit");
			if (rc)
			{
				talon->EnableCurrentLimit(enable);
				safeTalonCall(talon->GetLastError(), "EnableCurrentLimit");

				ROS_INFO_STREAM("Updated joint " << joint_id << "=" << can_talon_srx_names_[joint_id] <<" peak current");
				ts.setPeakCurrentLimit(peak_amps);
				ts.setPeakCurrentDuration(peak_msec);
				ts.setContinuousCurrentLimit(continuous_amps);
				ts.setCurrentLimitEnable(enable);
			}
			else
			{
				tc.resetCurrentLimit();
			}
		}
		//13
	clock_gettime(CLOCK_MONOTONIC, &end_time);
	time_sum[time_idx] +=
		((double)end_time.tv_sec -  (double)start_time.tv_sec) +
		((double)end_time.tv_nsec - (double)start_time.tv_nsec) / 1000000000.;
	iteration_count[time_idx] += 1;
	time_idx += 1;
	start_time = end_time;

		for (int i = hardware_interface::Status_1_General; i < hardware_interface::Status_Last; i++)
		{
			uint8_t period;
			const hardware_interface::StatusFrame status_frame = static_cast<hardware_interface::StatusFrame>(i);
			if (tc.statusFramePeriodChanged(status_frame, period) && (period != 0))
			{
				ctre::phoenix::motorcontrol::StatusFrameEnhanced status_frame_enhanced;
				if (convertStatusFrame(status_frame, status_frame_enhanced))
				{
					if (safeTalonCall(talon->SetStatusFramePeriod(status_frame_enhanced, period), "SetStatusFramePeriod"))
					{
						ts.setStatusFramePeriod(status_frame, period);
						ROS_INFO_STREAM("Updated joint " << joint_id << "=" << can_talon_srx_names_[joint_id] <<" status_frame " << i << "=" << static_cast<int>(period) << "mSec");
					}
					else
						tc.resetStatusFramePeriod(status_frame);
				}
			}
		}

		for (int i = hardware_interface::Control_3_General; i < hardware_interface::Control_Last; i++)
		{
			uint8_t period;
			const hardware_interface::ControlFrame control_frame = static_cast<hardware_interface::ControlFrame>(i);
			if (tc.controlFramePeriodChanged(control_frame, period) && (period != 0))
			{
				ctre::phoenix::motorcontrol::ControlFrame control_frame_phoenix;
				if (convertControlFrame(control_frame, control_frame_phoenix))
				{
					if (safeTalonCall(talon->SetControlFramePeriod(control_frame_phoenix, period), "SetControlFramePeriod"))
					{
						ts.setControlFramePeriod(control_frame, period);
						ROS_INFO_STREAM("Updated joint " << joint_id << "=" << can_talon_srx_names_[joint_id] <<" control_frame " << i << "=" << static_cast<int>(period) << "mSec");
					}
					else
						tc.setControlFramePeriod(control_frame, period);
				}

			}
		}

		{
#ifdef USE_TALON_MOTION_PROFILE
			// Lock this so that the motion profile update
			// thread doesn't update in the middle of writing
			// motion profile params
			std::lock_guard<std::mutex> l(*motion_profile_mutexes_[joint_id]);
#endif

			if (motion_profile_mode)
			{
				double motion_cruise_velocity;
				double motion_acceleration;
				if (tc.motionCruiseChanged(motion_cruise_velocity, motion_acceleration))
				{
					bool rc = true;
					//converted from rad/sec to native units
					rc &= safeTalonCall(talon->ConfigMotionCruiseVelocity(motion_cruise_velocity / radians_per_second_scale, timeoutMs),"ConfigMotionCruiseVelocity(");
					rc &= safeTalonCall(talon->ConfigMotionAcceleration(motion_acceleration / radians_per_second_scale, timeoutMs),"ConfigMotionAcceleration(");

					if (rc)
					{
						ROS_INFO_STREAM("Updated joint " << joint_id << "=" << can_talon_srx_names_[joint_id] <<" cruise velocity / acceleration");
						ts.setMotionCruiseVelocity(motion_cruise_velocity);
						ts.setMotionAcceleration(motion_acceleration);
					}
					else
					{
						tc.resetMotionCruise();
					}

				}

				int motion_profile_trajectory_period;
				if (tc.motionProfileTrajectoryPeriodChanged(motion_profile_trajectory_period))
				{
					if (safeTalonCall(talon->ConfigMotionProfileTrajectoryPeriod(motion_profile_trajectory_period, timeoutMs),"ConfigMotionProfileTrajectoryPeriod"))
					{
						ts.setMotionProfileTrajectoryPeriod(motion_profile_trajectory_period);
						ROS_INFO_STREAM("Updated joint " << joint_id << "=" << can_talon_srx_names_[joint_id] <<" motion profile trajectory period");
					}
					else
					{
						tc.resetMotionProfileTrajectoryPeriod();
					}
				}

				if (tc.clearMotionProfileTrajectoriesChanged())
				{
					if (safeTalonCall(talon->ClearMotionProfileTrajectories(), "ClearMotionProfileTrajectories"))
					{
						can_talons_mp_written_[joint_id]->store(false, std::memory_order_relaxed);
						ROS_INFO_STREAM("Cleared joint " << joint_id << "=" << can_talon_srx_names_[joint_id] <<" motion profile trajectories");
					}
					else
					{
						tc.setClearMotionProfileTrajectories();
					}
				}

				if (tc.clearMotionProfileHasUnderrunChanged())
				{
					if (safeTalonCall(talon->ClearMotionProfileHasUnderrun(timeoutMs),"ClearMotionProfileHasUnderrun"))
					{
						ROS_INFO_STREAM("Cleared joint " << joint_id << "=" << can_talon_srx_names_[joint_id] <<" motion profile underrun changed");
					}
					else
					{
						tc.setClearMotionProfileHasUnderrun();
					}
				}

				// TODO : check that Talon motion buffer is not full
				// before writing, communicate how many have been written
				// - and thus should be cleared - from the talon_command
				// list of requests.
			}

			std::vector<hardware_interface::TrajectoryPoint> trajectory_points;
			if (tc.motionProfileTrajectoriesChanged(trajectory_points))
			{
				//int i = 0;
				for (auto it = trajectory_points.cbegin(); it != trajectory_points.cend(); ++it)
				{
					ctre::phoenix::motion::TrajectoryPoint pt;
					pt.position = it->position / radians_scale;
					pt.velocity = it->velocity / radians_per_second_scale;
					pt.headingDeg = it->headingRad * 180. / M_PI;
					pt.auxiliaryPos = it->auxiliaryPos; // TODO : unit conversion?
					pt.profileSlotSelect0 = it->profileSlotSelect0;
					pt.profileSlotSelect1 = it->profileSlotSelect1;
					pt.isLastPoint = it->isLastPoint;
					pt.zeroPos = it->zeroPos;
					pt.timeDur = static_cast<ctre::phoenix::motion::TrajectoryDuration>(it->trajectoryDuration);
					safeTalonCall(talon->PushMotionProfileTrajectory(pt),"PushMotionProfileTrajectory");
					// TODO: not sure what to do if this fails?
					//ROS_INFO_STREAM("id: " << joint_id << " pos: " << pt.position << " i: " << i++);
				}
				// Copy the 1st profile trajectory point from
				// the top level buffer to the talon
				// Subsequent points will be copied by
				// the process_motion_profile_buffer_thread code
				//talon->ProcessMotionProfileBuffer();
				can_talons_mp_written_[joint_id]->store(true, std::memory_order_relaxed);

				ROS_INFO_STREAM("Added joint " << joint_id << "=" << can_talon_srx_names_[joint_id] <<" motion profile trajectories");
			}
		}
		//14
	clock_gettime(CLOCK_MONOTONIC, &end_time);
	time_sum[time_idx] +=
		((double)end_time.tv_sec -  (double)start_time.tv_sec) +
		((double)end_time.tv_nsec - (double)start_time.tv_nsec) / 1000000000.;
	iteration_count[time_idx] += 1;
	time_idx += 1;
	start_time = end_time;

		// Set new motor setpoint if either the mode or
		// the setpoint has been changed
		if (match_data_.isEnabled())
		{
			double command;
			hardware_interface::TalonMode in_mode;
			hardware_interface::DemandType demand1_type_internal;
			double demand1_value;

			const bool b1 = tc.newMode(in_mode);
			const bool b2 = tc.commandChanged(command);
			const bool b3 = tc.demand1Changed(demand1_type_internal, demand1_value);

			// TODO : unconditionally use the 4-param version of Set()
			// ROS_INFO_STREAM("b1 = " << b1 << " b2 = " << b2 << " b3 = " << b3);
			if (b1 || b2 || b3 || ros::Time::now().toSec() - can_talon_srx_run_profile_stop_time_[joint_id] < .2)
			{
				ctre::phoenix::motorcontrol::ControlMode out_mode;
				if (convertControlMode(in_mode, out_mode))
				{
					ts.setTalonMode(in_mode);
					ts.setSetpoint(command);

					ts.setNeutralOutput(false); // maybe make this a part of setSetpoint?

					switch (out_mode)
					{
						case ctre::phoenix::motorcontrol::ControlMode::Velocity:
							command /= radians_per_second_scale;
							break;
						case ctre::phoenix::motorcontrol::ControlMode::Position:
							command /= radians_scale;
							break;
						case ctre::phoenix::motorcontrol::ControlMode::MotionMagic:
							command /= radians_scale;
							break;
					}

					ts.setDemand1Type(demand1_type_internal);
					ts.setDemand1Value(demand1_value);

					if (b3)
					{
						ctre::phoenix::motorcontrol::DemandType demand1_type_phoenix;
						if (convertDemand1Type(demand1_type_internal, demand1_type_phoenix))
						{
//#define DEBUG_WRITE
#ifndef DEBUG_WRITE
							ROS_INFO_STREAM("called Set() on " << joint_id << "=" << can_talon_srx_names_[joint_id] <<
									" out_mode = " << static_cast<int>(out_mode) << " command = " << command <<
									" demand1_type_phoenix = " << static_cast<int>(demand1_type_phoenix) << " demand1_value = " << demand1_value);
#endif
							talon->Set(out_mode, command, demand1_type_phoenix, demand1_value);
						}
						else
							ROS_ERROR("Invalid Demand1 Type in hardware_interface write()");
					}
					else
					{
#ifdef DEBUG_WRITE
						ROS_INFO_STREAM("called Set(2) on " << joint_id << "=" << can_talon_srx_names_[joint_id] <<
								" out_mode = " << static_cast<int>(out_mode) << " command = " << command);
#endif
						talon->Set(out_mode, command);
					}
				}

#ifdef USE_TALON_MOTION_PROFILE
				// If any of the talons are set to MotionProfile and
				// command == 1 to start the profile, set
				// profile_is_live_ to true. If this is false
				// for all of them, set profile_is_live_ to false.
				if ((out_mode == ctre::phoenix::motorcontrol::ControlMode::MotionProfile) &&
					(command == 1))
				{
					profile_is_live = true;
					can_talons_mp_running_[joint_id]->store(true, std::memory_order_relaxed);
				}
				else
					can_talons_mp_running_[joint_id]->store(false, std::memory_order_relaxed);
#endif
			}
		}
		else
		{
			// Update talon state with requested setpoints for
			// debugging. Don't actually write them to the physical
			// Talons until the robot is re-enabled, though.
			double command;
			hardware_interface::DemandType demand1_type_internal;
			double demand1_value;

			ts.setSetpoint(tc.get());
			ts.setDemand1Type(tc.getDemand1Type());
			ts.setDemand1Value(tc.getDemand1Value());
			if (last_robot_enabled)
			{
				// On the switch from robot enabled to robot disabled, set Talons to ControlMode::Disabled
				// call resetMode() to queue up a change back to the correct mode / setpoint
				// when the robot switches from disabled back to enabled
				tc.resetMode();
				talon->Set(ctre::phoenix::motorcontrol::ControlMode::Disabled, 0);
				ts.setTalonMode(hardware_interface::TalonMode_Disabled);
				ROS_INFO_STREAM("Robot disabled - called Set(Disabled) on " << joint_id << "=" << can_talon_srx_names_[joint_id]);
			}
		}
		//15
	clock_gettime(CLOCK_MONOTONIC, &end_time);
	time_sum[time_idx] +=
		((double)end_time.tv_sec -  (double)start_time.tv_sec) +
		((double)end_time.tv_nsec - (double)start_time.tv_nsec) / 1000000000.;
	iteration_count[time_idx] += 1;
	time_idx += 1;
	start_time = end_time;

		if (tc.clearStickyFaultsChanged())
		{
			if (safeTalonCall(talon->ClearStickyFaults(timeoutMs), "ClearStickyFaults"))
			{
				ROS_INFO_STREAM("Cleared joint " << joint_id << "=" << can_talon_srx_names_[joint_id] <<" sticky_faults");
			}
			else
			{
				tc.setClearStickyFaults();
			}
		}
		//16
	clock_gettime(CLOCK_MONOTONIC, &end_time);
	time_sum[time_idx] +=
		((double)end_time.tv_sec -  (double)start_time.tv_sec) +
		((double)end_time.tv_nsec - (double)start_time.tv_nsec) / 1000000000.;
	iteration_count[time_idx] += 1;
	time_idx += 1;
	start_time = end_time;
	}
	last_robot_enabled = match_data_.isEnabled();

#ifdef USE_TALON_MOTION_PROFILE
	profile_is_live_.store(profile_is_live, std::memory_order_relaxed);
#endif

	for (size_t i = 0; i < num_nidec_brushlesses_; i++)
	{
		if (nidec_brushless_local_hardwares_[i])
			nidec_brushlesses_[i]->Set(brushless_command_[i]);
	}

	for (size_t i = 0; i < num_digital_outputs_; i++)
	{
		// Only invert the desired output once, on the controller
		// where the update originated
		const bool converted_command = (digital_output_command_[i] > 0) ^ (digital_output_inverts_[i] && digital_output_local_updates_[i]);
		if (converted_command != digital_output_state_[i])
		{
			if (digital_output_local_hardwares_[i])
				digital_outputs_[i]->Set(converted_command);
			digital_output_state_[i] = converted_command;
			ROS_INFO_STREAM("Wrote digital output " << i << "=" << converted_command);
		}
	}

	for (size_t i = 0; i < num_pwm_; i++)
	{
		const int setpoint = pwm_command_[i] * ((pwm_inverts_[i] & pwm_local_updates_[i]) ? -1 : 1);
		if (pwm_state_[i] != setpoint)
		{
			if (pwm_local_hardwares_[i])
				PWMs_[i]->SetSpeed(setpoint);
			pwm_state_[i] = setpoint;
			ROS_INFO_STREAM("PWM " << pwm_names_[i] <<
					" at channel" <<  pwm_pwm_channels_[i] <<
					" set to " << pwm_state_[i]);
		}
	}

	for (size_t i = 0; i < num_solenoids_; i++)
	{
		const bool setpoint = solenoid_command_[i] > 0;
		if (solenoid_state_[i] != setpoint)
		{
			if (solenoid_local_hardwares_[i])
			{
				int32_t status = 0;
				HAL_SetSolenoid(solenoids_[i], setpoint, &status);
				if (status != 0)
					ROS_ERROR_STREAM("Error setting solenoid " << solenoid_names_[i] <<
							" to " << setpoint << " status = " << status);
			}
			solenoid_state_[i] = setpoint;
			ROS_INFO_STREAM("Solenoid " << solenoid_names_[i] <<
							" at id " << solenoid_ids_[i] <<
							" / pcm " << solenoid_pcms_[i] <<
							" = " << setpoint);
		}
	}

	for (size_t i = 0; i < num_double_solenoids_; i++)
	{
		DoubleSolenoid::Value setpoint = DoubleSolenoid::Value::kOff;
		if (double_solenoid_command_[i] >= 1.0)
			setpoint = DoubleSolenoid::Value::kForward;
		else if (double_solenoid_command_[i] <= -1.0)
			setpoint = DoubleSolenoid::Value::kReverse;

		// Not sure if it makes sense to store command values
		// in state or wpilib enum values
		if (double_solenoid_state_[i] != double_solenoid_command_[i])
		{
			if (double_solenoid_local_hardwares_[i])
			{
				bool forward = false;
				bool reverse = false;
				if (setpoint == DoubleSolenoid::Value::kForward)
					forward = true;
				else if (setpoint == DoubleSolenoid::Value::kReverse)
					forward = true;

				int32_t status = 0;
				HAL_SetSolenoid(double_solenoids_[i].forward_, forward, &status);
				if (status != 0)
					ROS_ERROR_STREAM("Error setting double solenoid " << double_solenoid_names_[i] <<
							" forward to " << forward << " status = " << status);
				status = 0;
				HAL_SetSolenoid(double_solenoids_[i].reverse_, reverse, &status);
				if (status != 0)
					ROS_ERROR_STREAM("Error setting double solenoid " << double_solenoid_names_[i] <<
							" reverse to " << reverse << " status = " << status);
			}
			double_solenoid_state_[i] = double_solenoid_command_[i];
			ROS_INFO_STREAM("Double solenoid " << double_solenoid_names_[i] <<
					" at forward id " << double_solenoid_forward_ids_[i] <<
					"/ reverse id " << double_solenoid_reverse_ids_[i] <<
					" / pcm " << double_solenoid_pcms_[i] <<
					" = " << setpoint);
		}
	}

	for (size_t i = 0; i < num_rumbles_; i++)
	{
		if (rumble_state_[i] != rumble_command_[i])
		{
			const unsigned int rumbles = *((unsigned int*)(&rumble_command_[i]));
			const unsigned int left_rumble  = (rumbles >> 16) & 0xFFFF;
			const unsigned int right_rumble = (rumbles      ) & 0xFFFF;
			if (rumble_local_hardwares_[i])
				HAL_SetJoystickOutputs(rumble_ports_[i], 0, left_rumble, right_rumble);
			rumble_state_[i] = rumble_command_[i];
			ROS_INFO_STREAM("Wrote rumble " << i << "=" << rumble_command_[i]);
		}
	}

	for (size_t i = 0; i< num_compressors_; i++)
	{
		if (last_compressor_command_[i] != compressor_command_[i])
		{
			const bool setpoint = compressor_command_[i] > 0;
			if (compressor_local_hardwares_[i])
			{
				int32_t status = 0;
				HAL_SetCompressorClosedLoopControl(compressors_[i], setpoint, &status);
			}
			last_compressor_command_[i] = compressor_command_[i];
			ROS_INFO_STREAM("Wrote compressor " << i << "=" << setpoint);
		}
	}

	// TODO : what to do about this?
	for (size_t i = 0; i < num_dummy_joints_; i++)
	{
		if (dummy_joint_locals_[i])
		{
			// Use dummy joints to communicate info between
			// various controllers and driver station smartdash vars
			{
				dummy_joint_effort_[i] = 0;
				//if (dummy_joint_names_[i].substr(2, std::string::npos) == "_angle")
				{
					// position mode
					dummy_joint_velocity_[i] = (dummy_joint_command_[i] - dummy_joint_position_[i]) / elapsed_time.toSec();
					dummy_joint_position_[i] = dummy_joint_command_[i];
				}
#if 0
				else if (dummy_joint_names_[i].substr(2, std::string::npos) == "_drive")
				{
					// velocity mode
					dummy_joint_position_[i] += dummy_joint_command_[i] * elapsed_time.toSec();
					dummy_joint_velocity_[i] = dummy_joint_command_[i];
				}
#endif
			}
		}
	}

#if 0
	clock_gettime(CLOCK_MONOTONIC, &end_time);
	time_sum[time_idx] +=
		((double)end_time.tv_sec -  (double)start_time.tv_sec) +
		((double)end_time.tv_nsec - (double)start_time.tv_nsec) / 1000000000.;
	iteration_count[time_idx] += 1;
	time_idx += 1;
#endif
	std::stringstream s;
	for (int i = 0; i < time_idx; i++)
		s << time_sum[i]/iteration_count[i] << " ";
	ROS_INFO_STREAM_THROTTLE(2, "write() = " << s.str());
}

// Convert from internal version of hardware mode ID
// to one to write to actual Talon hardware
// Return true if conversion is OK, false if
// an unknown mode is hit.
bool FRCRobotHWInterface::convertControlMode(
	const hardware_interface::TalonMode input_mode,
	ctre::phoenix::motorcontrol::ControlMode &output_mode)
{
	switch (input_mode)
	{
		case hardware_interface::TalonMode_PercentOutput:
			output_mode = ctre::phoenix::motorcontrol::ControlMode::PercentOutput;
			break;
		case hardware_interface::TalonMode_Position:      // CloseLoop
			output_mode = ctre::phoenix::motorcontrol::ControlMode::Position;
			break;
		case hardware_interface::TalonMode_Velocity:      // CloseLoop
			output_mode = ctre::phoenix::motorcontrol::ControlMode::Velocity;
			break;
		case hardware_interface::TalonMode_Current:       // CloseLoop
			output_mode = ctre::phoenix::motorcontrol::ControlMode::Current;
			break;
		case hardware_interface::TalonMode_Follower:
			output_mode = ctre::phoenix::motorcontrol::ControlMode::Follower;
			break;
		case hardware_interface::TalonMode_MotionProfile:
			output_mode = ctre::phoenix::motorcontrol::ControlMode::MotionProfile;
			break;
		case hardware_interface::TalonMode_MotionMagic:
			output_mode = ctre::phoenix::motorcontrol::ControlMode::MotionMagic;
			break;
		case hardware_interface::TalonMode_Disabled:
			output_mode = ctre::phoenix::motorcontrol::ControlMode::Disabled;
			break;
		default:
			output_mode = ctre::phoenix::motorcontrol::ControlMode::Disabled;
			ROS_WARN("Unknown mode seen in HW interface");
			return false;
	}
	return true;
}

bool FRCRobotHWInterface::convertDemand1Type(
	const hardware_interface::DemandType input,
	ctre::phoenix::motorcontrol::DemandType &output)
{
	switch(input)
	{
		case hardware_interface::DemandType::DemandType_Neutral:
			output = ctre::phoenix::motorcontrol::DemandType::DemandType_Neutral;
			break;
		case hardware_interface::DemandType::DemandType_AuxPID:
			output = ctre::phoenix::motorcontrol::DemandType::DemandType_AuxPID;
			break;
		case hardware_interface::DemandType::DemandType_ArbitraryFeedForward:
			output = ctre::phoenix::motorcontrol::DemandType::DemandType_ArbitraryFeedForward;
			break;
		default:
			output = ctre::phoenix::motorcontrol::DemandType::DemandType_Neutral;
			ROS_WARN("Unknown demand1 type seen in HW interface");
			return false;
	}
}

bool FRCRobotHWInterface::convertNeutralMode(
	const hardware_interface::NeutralMode input_mode,
	ctre::phoenix::motorcontrol::NeutralMode &output_mode)
{
	switch (input_mode)
	{
		case hardware_interface::NeutralMode_EEPROM_Setting:
			output_mode = ctre::phoenix::motorcontrol::EEPROMSetting;
			break;
		case hardware_interface::NeutralMode_Coast:
			output_mode = ctre::phoenix::motorcontrol::Coast;
			break;
		case hardware_interface::NeutralMode_Brake:
			output_mode = ctre::phoenix::motorcontrol::Brake;
			break;
		default:
			output_mode = ctre::phoenix::motorcontrol::EEPROMSetting;
			ROS_WARN("Unknown neutral mode seen in HW interface");
			return false;
	}

	return true;
}

bool FRCRobotHWInterface::convertFeedbackDevice(
	const hardware_interface::FeedbackDevice input_fd,
	ctre::phoenix::motorcontrol::FeedbackDevice &output_fd)
{
	switch (input_fd)
	{
		case hardware_interface::FeedbackDevice_QuadEncoder:
			output_fd = ctre::phoenix::motorcontrol::QuadEncoder;
			break;
		case hardware_interface::FeedbackDevice_Analog:
			output_fd = ctre::phoenix::motorcontrol::Analog;
			break;
		case hardware_interface::FeedbackDevice_Tachometer:
			output_fd = ctre::phoenix::motorcontrol::Tachometer;
			break;
		case hardware_interface::FeedbackDevice_PulseWidthEncodedPosition:
			output_fd = ctre::phoenix::motorcontrol::PulseWidthEncodedPosition;
			break;
		case hardware_interface::FeedbackDevice_SensorSum:
			output_fd = ctre::phoenix::motorcontrol::SensorSum;
			break;
		case hardware_interface::FeedbackDevice_SensorDifference:
			output_fd = ctre::phoenix::motorcontrol::SensorDifference;
			break;
		case hardware_interface::FeedbackDevice_RemoteSensor0:
			output_fd = ctre::phoenix::motorcontrol::RemoteSensor0;
			break;
		case hardware_interface::FeedbackDevice_RemoteSensor1:
			output_fd = ctre::phoenix::motorcontrol::RemoteSensor1;
			break;
		case hardware_interface::FeedbackDevice_SoftwareEmulatedSensor:
			output_fd = ctre::phoenix::motorcontrol::SoftwareEmulatedSensor;
			break;
		default:
			ROS_WARN("Unknown feedback device seen in HW interface");
			return false;
	}
	return true;
}

bool FRCRobotHWInterface::convertLimitSwitchSource(
	const hardware_interface::LimitSwitchSource input_ls,
	ctre::phoenix::motorcontrol::LimitSwitchSource &output_ls)
{
	switch (input_ls)
	{
		case hardware_interface::LimitSwitchSource_FeedbackConnector:
			output_ls = ctre::phoenix::motorcontrol::LimitSwitchSource_FeedbackConnector;
			break;
		case hardware_interface::LimitSwitchSource_RemoteTalonSRX:
			output_ls = ctre::phoenix::motorcontrol::LimitSwitchSource_RemoteTalonSRX;
			break;
		case hardware_interface::LimitSwitchSource_RemoteCANifier:
			output_ls = ctre::phoenix::motorcontrol::LimitSwitchSource_RemoteCANifier;
			break;
		case hardware_interface::LimitSwitchSource_Deactivated:
			output_ls = ctre::phoenix::motorcontrol::LimitSwitchSource_Deactivated;
			break;
		default:
			ROS_WARN("Unknown limit switch source seen in HW interface");
			return false;
	}
	return true;
}

bool FRCRobotHWInterface::convertLimitSwitchNormal(
	const hardware_interface::LimitSwitchNormal input_ls,
	ctre::phoenix::motorcontrol::LimitSwitchNormal &output_ls)
{
	switch (input_ls)
	{
		case hardware_interface::LimitSwitchNormal_NormallyOpen:
			output_ls = ctre::phoenix::motorcontrol::LimitSwitchNormal_NormallyOpen;
			break;
		case hardware_interface::LimitSwitchNormal_NormallyClosed:
			output_ls = ctre::phoenix::motorcontrol::LimitSwitchNormal_NormallyClosed;
			break;
		case hardware_interface::LimitSwitchNormal_Disabled:
			output_ls = ctre::phoenix::motorcontrol::LimitSwitchNormal_Disabled;
			break;
		default:
			ROS_WARN("Unknown limit switch normal seen in HW interface");
			return false;
	}
	return true;

}

bool FRCRobotHWInterface::convertVelocityMeasurementPeriod(const hardware_interface::VelocityMeasurementPeriod input_v_m_p, ctre::phoenix::motorcontrol::VelocityMeasPeriod &output_v_m_period)
{
	switch(input_v_m_p)
	{
		case hardware_interface::VelocityMeasurementPeriod::Period_1Ms:
			output_v_m_period = ctre::phoenix::motorcontrol::VelocityMeasPeriod::Period_1Ms;
			break;
		case hardware_interface::VelocityMeasurementPeriod::Period_2Ms:
			output_v_m_period = ctre::phoenix::motorcontrol::VelocityMeasPeriod::Period_2Ms;
			break;
		case hardware_interface::VelocityMeasurementPeriod::Period_5Ms:
			output_v_m_period = ctre::phoenix::motorcontrol::VelocityMeasPeriod::Period_5Ms;
			break;
		case hardware_interface::VelocityMeasurementPeriod::Period_10Ms:
			output_v_m_period = ctre::phoenix::motorcontrol::VelocityMeasPeriod::Period_10Ms;
			break;
		case hardware_interface::VelocityMeasurementPeriod::Period_20Ms:
			output_v_m_period = ctre::phoenix::motorcontrol::VelocityMeasPeriod::Period_20Ms;
			break;
		case hardware_interface::VelocityMeasurementPeriod::Period_25Ms:
			output_v_m_period = ctre::phoenix::motorcontrol::VelocityMeasPeriod::Period_25Ms;
			break;
		case hardware_interface::VelocityMeasurementPeriod::Period_50Ms:
			output_v_m_period = ctre::phoenix::motorcontrol::VelocityMeasPeriod::Period_50Ms;
			break;
		case hardware_interface::VelocityMeasurementPeriod::Period_100Ms:
			output_v_m_period = ctre::phoenix::motorcontrol::VelocityMeasPeriod::Period_100Ms;
			break;
		default:
			ROS_WARN("Unknown velocity measurement period seen in HW interface");
			return false;
	}
	return true;
}

bool FRCRobotHWInterface::convertStatusFrame(const hardware_interface::StatusFrame input, ctre::phoenix::motorcontrol::StatusFrameEnhanced &output)
{
	switch (input)
	{
		case hardware_interface::Status_1_General:
			output = ctre::phoenix::motorcontrol::Status_1_General;
			break;
		case hardware_interface::Status_2_Feedback0:
			output = ctre::phoenix::motorcontrol::Status_2_Feedback0;
			break;
		case hardware_interface::Status_3_Quadrature:
			output = ctre::phoenix::motorcontrol::Status_3_Quadrature;
			break;
		case hardware_interface::Status_4_AinTempVbat:
			output = ctre::phoenix::motorcontrol::Status_4_AinTempVbat;
			break;
		case hardware_interface::Status_6_Misc:
			output = ctre::phoenix::motorcontrol::Status_6_Misc;
			break;
		case hardware_interface::Status_7_CommStatus:
			output = ctre::phoenix::motorcontrol::Status_7_CommStatus;
			break;
		case hardware_interface::Status_8_PulseWidth:
			output = ctre::phoenix::motorcontrol::Status_8_PulseWidth;
			break;
		case hardware_interface::Status_9_MotProfBuffer:
			output = ctre::phoenix::motorcontrol::Status_9_MotProfBuffer;
			break;
		case hardware_interface::Status_10_MotionMagic:
			output = ctre::phoenix::motorcontrol::Status_10_MotionMagic;
			break;
		case hardware_interface::Status_11_UartGadgeteer:
			output = ctre::phoenix::motorcontrol::Status_11_UartGadgeteer;
			break;
		case hardware_interface::Status_12_Feedback1:
			output = ctre::phoenix::motorcontrol::Status_12_Feedback1;
			break;
		case hardware_interface::Status_13_Base_PIDF0:
			output = ctre::phoenix::motorcontrol::Status_13_Base_PIDF0;
			break;
		case hardware_interface::Status_14_Turn_PIDF1:
			output = ctre::phoenix::motorcontrol::Status_14_Turn_PIDF1;
			break;
		case hardware_interface::Status_15_FirmwareApiStatus:
			output = ctre::phoenix::motorcontrol::Status_15_FirmareApiStatus;
			break;
		default:
			ROS_ERROR("Invalid input in convertStatusFrame");
			return false;
	}
	return true;
}

bool FRCRobotHWInterface::convertControlFrame(const hardware_interface::ControlFrame input, ctre::phoenix::motorcontrol::ControlFrame &output)
{
	switch (input)
	{
		case hardware_interface::Control_3_General:
			output = ctre::phoenix::motorcontrol::Control_3_General;
			break;
		case hardware_interface::Control_4_Advanced:
			output = ctre::phoenix::motorcontrol::Control_4_Advanced;
			break;
#if 0 // There's no SetControlFramePeriod which takes an enhanced ControlFrame, so this is out for now
		case hardware_interface::Control_5_FeedbackOutputOverride:
			output = ctre::phoenix::motorcontrol::Control_5_FeedbackOutputOverride_;
			break;
#endif
		case hardware_interface::Control_6_MotProfAddTrajPoint:
			output = ctre::phoenix::motorcontrol::Control_6_MotProfAddTrajPoint;
			break;
		default:
			ROS_ERROR("Invalid input in convertControlFrame");
			return false;
	}
	return true;

}

} // namespace
