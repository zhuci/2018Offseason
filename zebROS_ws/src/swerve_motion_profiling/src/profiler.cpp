#include<swerve_motion_profiling/profiler.h>

namespace swerve_profile
{
	swerve_profiler::swerve_profiler(double max_wheel_dist, double max_wheel_mid_accel, double max_wheel_vel,
	double max_steering_accel, double max_steering_vel, double dt, double index_dist_unit)
	{
		max_wheel_dist_ = max_wheel_dist;
                max_wheel_mid_accel_ = max_wheel_mid_accel;
                max_wheel_vel_ = max_wheel_vel;
                max_steering_accel_ = max_steering_accel;
                max_steering_vel_ = max_steering_vel;
                dt_ = dt;
		index_dist_unit_ = index_dist_unit;
	}
	//TODO :: path should be const vect & to avoid a redundant copy
	// being made each time the function is called
	bool swerve_profiler::generate_profile(const std::vector<path_point> &path, const double &initial_v, const double &final_v)
	{
		double curr_v = initial_v;
		std::vector<double> velocities;
		velocities.reserve(155 / dt_); //For full auto :) 
		std::vector<double> positions;
		positions.reserve(155 / dt_); //For full auto :) 

		//Forward pass
		for(double i = 0; i < path.size();)
		{
			velocities.push_back(curr_v);
			positions.push_back(i);

			i += curr_v*dt_/index_dist_unit_;	

			if(!solve_for_next_V(i, path, curr_v))
			{
				return false;
			}			
		}
		//std::vector<> final_points; //TODO:Some type of struct or something to return
		//final_points.reserve(155 / dt_); //For full auto :) 
		curr_v = final_v;
		double starting_point = positions.size();
		double vel_cap;
		for(double i = path.size(); i > 0;)
		{
			i -= curr_v*dt_/index_dist_unit_;	
			
			if(!solve_for_next_V(i, path, curr_v))
			{
				return false;
			}			
			for(size_t k = 0; k < starting_point; k++)
			{
				if(positions[starting_point-k] < i)
				{
					starting_point -= k;
					break;
				}
				//Find point
			}
			//Linear interpolation
			vel_cap = i * (velocities[starting_point] - velocities[starting_point - 1]) / 
			(positions[starting_point] - positions[starting_point - 1]) - positions[starting_point] *
			(velocities[starting_point] - velocities[starting_point - 1]) / 
			(positions[starting_point] - positions[starting_point - 1]) + velocities[starting_point];	
			//Keep below forward pass	
			coerce(curr_v, -100000000000, vel_cap);
		}
		return true;
	}
	// TODO :: is return code needed here?
	bool swerve_profiler::coerce(double &val, const double &min, const double &max)
	{
		if(val > max)
		{
			val = max;
			return true;
		}
		else if(val < min)
		{
			val = min;
			return true;
		}
		else
		{
			return false;
		}	
	}
	bool swerve_profiler::solve_for_next_V(const double &i, const std::vector<path_point> &path, double &current_v)
	{
		// TODO - double-check that these need to be static
		static double v_general_max;
		static double v_curve_max; 
		static double eff_max_a;
		static double max_wheel_orientation_vel;
		static double max_wheel_orientation_accel;
		static double accel;
		static double theta;
		static double cos_t;
		static double sin_t;
		static double path_induced_a;
		if(i<=0 && i>=path.size() - 1)
		{
			max_wheel_orientation_accel = path[i].angular_accel * max_wheel_dist_;
			max_wheel_orientation_vel = path[i].angular_velocity * max_wheel_dist_;
			theta = fabs(fmod(path[i].path_angle - path[i].orientation, M_PI / 8));
			cos_t = cos(theta);
			sin_t = sin(theta);
			path_induced_a = current_v*current_v/path[i].radius;
			
			eff_max_a = max_wheel_mid_accel_ * 2 * (1 -  (max_wheel_vel_ - sqrt((current_v + max_wheel_orientation_vel * sqrt(2)/2) * (current_v + max_wheel_orientation_vel * sqrt(2)/2) + max_wheel_orientation_vel * max_wheel_orientation_vel / 2)) / max_wheel_vel_);
			coerce(eff_max_a, 0, max_wheel_mid_accel_); //Consider disabling this coerce
			
			// TODO : check return code here
			// TODO : Use explicit multiply rather than pow() for squaring stuff
			poly_solve(1, 4*cos_t*sin_t*path_induced_a + sqrt(2)*cos_t*max_wheel_orientation_accel + sqrt(2)*sin_t*max_wheel_orientation_accel, path_induced_a*path_induced_a + sqrt(2)*sin_t*path_induced_a*max_wheel_orientation_accel + sqrt(2)*cos_t*path_induced_a*max_wheel_orientation_accel + max_wheel_orientation_accel*max_wheel_orientation_accel - max_wheel_mid_accel_ * max_wheel_mid_accel_, accel);

			current_v += accel * dt_;
			if(!poly_solve(1, sqrt(2) *  max_wheel_orientation_vel * cos_t + sqrt(2) *  max_wheel_orientation_vel * sin_t, max_wheel_orientation_vel * max_wheel_orientation_vel- max_wheel_vel_ * max_wheel_vel_, v_general_max))
				return false;
			//Note: assumption is that angular velocity doesn't change much over timestep
			coerce(current_v, -v_general_max, v_general_max); 
			//consider using above coerce in a if statement for optimization

			if(!poly_solve(1, sqrt(2) *  max_wheel_orientation_accel * cos_t + sqrt(2) *  max_wheel_orientation_accel * sin_t, max_wheel_orientation_accel * max_wheel_orientation_accel - eff_max_a * eff_max_a, v_curve_max))
				return false;
			v_curve_max = sqrt(v_curve_max);
			v_curve_max *= path[i].radius;
			coerce(current_v, -v_curve_max, v_curve_max);
		}
		else
		{	
			current_v += max_wheel_mid_accel_;
			coerce(current_v, -max_wheel_vel_, max_wheel_vel_);	
			return true;
		}
	}
	bool swerve_profiler::poly_solve(const double &a, const double &b, const double &c, double &x)
	{
		const double det = b*b - 4 * a * c;
		if(det < 0)
		{
			return false;
		}
		else
		{
			x = (-b + sqrt(det))/(2*a); //This is just one of the roots, but it should be fine for all 
						    //cases it is used here
			return true;
		}
	}
}