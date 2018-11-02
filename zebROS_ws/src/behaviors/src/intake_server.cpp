#include "ros/ros.h"
#include "actionlib/server/simple_action_server.h"
#include "behaviors/IntakeAction.h"
#include "intake_controller/IntakeSrv.h"
#include "std_msgs/Bool.h"
#include <atomic>
#include <ros/console.h>

static double intake_power;
static double intake_hold_power;
double linebreak_debounce_iterations; 

class IntakeAction {
    protected:
        ros::NodeHandle nh_;

        actionlib::SimpleActionServer<behaviors::IntakeAction> as_;
        std::string action_name_;
        ros::ServiceClient intake_srv_;
	std::atomic<int> cube_state_true;
	behaviors::IntakeResult result_;
	ros::Subscriber cube_state_;
	ros::Subscriber proceed_;
	bool proceed;

    public:
        IntakeAction(const std::string &name) :
            as_(nh_, name, boost::bind(&IntakeAction::executeCB, this, _1), false),
            action_name_(name)
        {
            as_.start();
            std::map<std::string, std::string> service_connection_header;
            service_connection_header["tcp_nodelay"] = "1";
            intake_srv_ = nh_.serviceClient<intake_controller::IntakeSrv>("/frcrobot/intake_controller/intake_command", false, service_connection_header); 
            cube_state_ = nh_.subscribe("/frcrobot/intake_controller/cube_state", 1, &IntakeAction::cubeCallback, this); 
            //proceed_ = nh_.subscribe("/frcrobot/auto_interpreter_server/proceed", 1, &IntakeAction::proceedCallback, this);
	}

        ~IntakeAction(void) 
        {
        }

        void executeCB(const behaviors::IntakeGoalConstPtr &goal) {
            ros::Rate r(10);
            double startTime = ros::Time::now().toSec();
            bool timed_out = false;
            bool aborted = false;
            if(goal->intake_cube) {
                cube_state_true = 0;
                intake_controller::IntakeSrv srv;
                srv.request.power = intake_power;
                srv.request.intake_in = false;
                if(!intake_srv_.call(srv)) 
                    ROS_ERROR("Srv intake call failed in auto interpreter server intake");
                ros::spinOnce();

                bool success = false;
                while(!success && !timed_out && !aborted) {
                    success = cube_state_true > linebreak_debounce_iterations;
                    if(as_.isPreemptRequested() || !ros::ok()) {
                        ROS_WARN("%s: Preempted", action_name_.c_str());
                        as_.setPreempted();
                        aborted = true;
                        return;
                    }
                    if (!aborted) {
                        r.sleep();
                        ros::spinOnce();
                        timed_out = (ros::Time::now().toSec()-startTime) > goal->intake_timeout;
                    }
                }

                srv.request.power = success ? 0.15 : 0;
                srv.request.intake_in = true; //soft in
                if(!intake_srv_.call(srv)) 
                    ROS_ERROR("Srv intake call failed in auto interpreter server intake");
            }
            else
            {
                cube_state_true = 0;
                intake_controller::IntakeSrv srv;
                srv.request.power = -1;
                srv.request.intake_in = true; //soft in
                if(!intake_srv_.call(srv)) 
                    ROS_ERROR("Srv intake call failed in auto interpreter server intake");
                ros::spinOnce();

                bool success = false;
                while(!success && !timed_out && !aborted) {
                    success = cube_state_true > linebreak_debounce_iterations;
                    if(as_.isPreemptRequested())
                        ROS_WARN("preempt requested");
                    if (!ros::ok())
                        ROS_WARN("ROS is not okay");
                        /*ROS_WARN("%s: Preempted", action_name_.c_str());
                        as_.setPreempted();
                        aborted = true;
                        return;
                    }*/
                    if (!aborted) {
                        r.sleep();
                        ros::spinOnce();
                        timed_out = (ros::Time::now().toSec()-startTime) > goal->intake_timeout;
                    }
                }

                srv.request.power = success ? 0.15 : 0;
                srv.request.intake_in = true; //soft in
                if(!intake_srv_.call(srv)) 
                    ROS_ERROR("Srv intake call failed in auto interpreter server intake");
            }
            if(timed_out)
            {
                ROS_INFO("%s: Timed Out", action_name_.c_str());
            }
            else if(!aborted)
            {
                ROS_INFO("%s: Succeeded", action_name_.c_str());
            }
            else
            {
                ROS_INFO("%s: Aborted", action_name_.c_str());
            }

            result_.timed_out = timed_out;
            as_.setSucceeded(result_);
            return;
        }

        void cubeCallback(const std_msgs::Bool &msg) {
            if(msg.data) 
                cube_state_true += 1;
            else
                cube_state_true = 0;
	}
};

int main(int argc, char** argv) {
    ros::init(argc, argv, "intake_server");
    IntakeAction intake_action("intake_server");
    
    ros::NodeHandle n;
    ros::NodeHandle n_params(n, "teleop_params");
    ros::NodeHandle n_auto_interpreter_server_intake_params(n, "auto_interpreter_server_intake_params");

    if (!n_params.getParam("intake_power", intake_power))
		ROS_ERROR("Could not read intake_power in intake_server");

    if (!n_params.getParam("intake_hold_power", intake_hold_power))
		ROS_ERROR("Could not read intake_hold_power in intake_server");

    if (!n_auto_interpreter_server_intake_params.getParam("linebreak_debounce_iterations", linebreak_debounce_iterations))
		ROS_ERROR("Could not read linebreak_debounce_iterations in intake_sever");
    ros::spin();
    return 0;
}