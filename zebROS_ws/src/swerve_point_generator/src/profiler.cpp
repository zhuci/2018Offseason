#include <swerve_point_generator/profiler.h>
#include <ros/console.h>
#include <iostream>

namespace swerve_profile
{
swerve_profiler::swerve_profiler(double max_wheel_dist, double max_wheel_mid_accel, double max_wheel_vel,
								 double max_steering_accel, double max_steering_vel, double dt, double ang_accel_conv, double max_wheel_brake_accel)
{
	max_wheel_dist_ = max_wheel_dist;
	max_wheel_mid_accel_ = max_wheel_mid_accel;
	max_wheel_brake_accel_ = max_wheel_brake_accel;
	max_wheel_vel_ = max_wheel_vel;
	max_steering_accel_ = max_steering_accel;
	max_steering_vel_ = max_steering_vel;
	dt_ = dt;
	ang_accel_conv_ = ang_accel_conv;
	t_shift_;
}

bool swerve_profiler::generate_profile(std::vector<spline_coefs> x_splines, std::vector<spline_coefs> y_splines, std::vector<spline_coefs> orient_splines, const double initial_v, const double final_v, swerve_point_generator::GenerateSwerveProfile::Response &out_msg, const std::vector<double> &end_points, double t_shift, bool flip_dirc)
{
	t_shift_ = t_shift;
	flip_dirc_ = flip_dirc;
	// Bounds checking. Not safe to proceed if end_points is empty.
	if (end_points.empty()) {
		ROS_ERROR("Endpoints should never be empty!");
		return false;
	}
	if  (x_splines.empty()) {
		ROS_ERROR("x-splines should also not be empty");
		return false;
	}
	t_total_ = end_points[end_points.size()-1] - end_points[0];

	tk::spline spline;
	double total_arc;

	//ROS_WARN("called");

	double curr_v = final_v;
	std::vector<double> velocities;
	velocities.reserve(155 / dt_); //For full auto :)
	std::vector<double> positions;
	positions.reserve(155 / dt_); //For full auto :)

	path_point holder_point;
	spline_coefs holder_spline;

	fow = true;

	std::vector<spline_coefs> x_splines_first_deriv, y_splines_first_deriv, orient_splines_first_deriv ;
	std::vector<spline_coefs> x_splines_second_deriv, y_splines_second_deriv, orient_splines_second_deriv ;
	for (size_t i = 0; i < x_splines.size(); i++)
	{
		holder_spline.a = 0;
		holder_spline.b = 5 * x_splines[i].a;
		holder_spline.c = 4 * x_splines[i].b;
		holder_spline.d = 3 * x_splines[i].c;
		holder_spline.e = 2 * x_splines[i].d;
		holder_spline.f = 1 * x_splines[i].e;

		x_splines_first_deriv.push_back(holder_spline);


		holder_spline.f = 1 * holder_spline.e;
		holder_spline.e = 2 * holder_spline.d;
		holder_spline.d = 3 * holder_spline.c;
		holder_spline.c = 4 * holder_spline.b;
		holder_spline.b = 0;
		holder_spline.a = 0;

		x_splines_second_deriv.push_back(holder_spline);

		holder_spline.a = 0;
		holder_spline.b = 5 * y_splines[i].a;
		holder_spline.c = 4 * y_splines[i].b;
		holder_spline.d = 3 * y_splines[i].c;
		holder_spline.e = 2 * y_splines[i].d;
		holder_spline.f = 1 * y_splines[i].e;

		y_splines_first_deriv.push_back(holder_spline);


		holder_spline.f = 1 * holder_spline.e;
		holder_spline.e = 2 * holder_spline.d;
		holder_spline.d = 3 * holder_spline.c;
		holder_spline.c = 4 * holder_spline.b;
		holder_spline.b = 0;
		holder_spline.a = 0;

		y_splines_second_deriv.push_back(holder_spline);

		holder_spline.a = 0;
		holder_spline.b = 5 * orient_splines[i].a;
		holder_spline.c = 4 * orient_splines[i].b;
		holder_spline.d = 3 * orient_splines[i].c;
		holder_spline.e = 2 * orient_splines[i].d;
		holder_spline.f = 1 * orient_splines[i].e;

		orient_splines_first_deriv.push_back(holder_spline);


		holder_spline.f = 1 * holder_spline.e;
		holder_spline.e = 2 * holder_spline.d;
		holder_spline.d = 3 * holder_spline.c;
		holder_spline.c = 4 * holder_spline.b;
		holder_spline.b = 0;
		holder_spline.a = 0;

		orient_splines_second_deriv.push_back(holder_spline);
	}
	if(flip_dirc_)
	{	
		//ROS_ERROR_STREAM("flipping");
		std::reverse(x_splines.begin(), x_splines.end());
		std::reverse(y_splines.begin(), y_splines.end());
		std::reverse(orient_splines.begin(), orient_splines.end());
		std::reverse(x_splines_first_deriv.begin(), x_splines_first_deriv.end());
		std::reverse(y_splines_first_deriv.begin(), y_splines_first_deriv.end());
		std::reverse(orient_splines_first_deriv.begin(), orient_splines_first_deriv.end());
		std::reverse(x_splines_second_deriv.begin(), x_splines_second_deriv.end());
		std::reverse(y_splines_second_deriv.begin(), y_splines_second_deriv.end());
		std::reverse(orient_splines_second_deriv.begin(), orient_splines_second_deriv.end());
	}
	std::vector<double> dtds_for_spline;
	double t_raw2;
	spline = parametrize_spline(x_splines_first_deriv, y_splines_first_deriv, end_points, total_arc, dtds_for_spline);
	//ROS_WARN("still running");
	int point_count = 0;
	//back pass
	//ROS_INFO_STREAM("total arc: " <<total_arc);
	for (double i = total_arc /*- .1*/ ; i > 0;)
	{

		i -= curr_v * dt_;
		//ROS_WARN("pre - even_now");

		velocities.push_back(curr_v);
		positions.push_back(i);
		point_count++;
		//if (point_count % 100 == 0)
			//ROS_INFO_STREAM("num points: " << point_count );

		//ROS_WARN("pre - even_now");
		t_raw2 = spline(i);

		//ROS_INFO_STREAM("i val: " << i << " t val: " << t_raw2 << " also: " << spline(i));
		//ROS_WARN("even_now");
		comp_point_characteristics(x_splines, y_splines, x_splines_first_deriv, y_splines_first_deriv, x_splines_second_deriv, y_splines_second_deriv, orient_splines, orient_splines_first_deriv, orient_splines_second_deriv, holder_point, end_points, dtds_for_spline, t_raw2);




		if (!solve_for_next_V(holder_point, total_arc, curr_v, i, max_wheel_brake_accel_))
		{
			return false;
		}
		//ROS_INFO_STREAM("V: " << curr_v);
	}
	//ROS_INFO_STREAM("passed loop 1");
	velocities.erase(velocities.end() - 1);
	positions.erase(positions.end() - 1);
	
	point_count = out_msg.points.size();
	if(out_msg.points.size() == 0)
		out_msg.points.resize(155 / dt_); //For full auto :)  TODO: optimize
	curr_v = initial_v;
	double starting_point = positions.size() - 1;
	double vel_cap;
	double t_raw3;
	ros::Duration now(0);
	ros::Duration period(dt_);
	fow = false;
	for (double i = 0; i < total_arc /* - .1*/;)
	{
		i += curr_v * dt_;

		t_raw3 = spline(i);
		//ROS_INFO_STREAM("i val: " << i << " t val: " << t_raw3);

		comp_point_characteristics(x_splines, y_splines, x_splines_first_deriv, y_splines_first_deriv, x_splines_second_deriv, y_splines_second_deriv, orient_splines, orient_splines_first_deriv, orient_splines_second_deriv, holder_point, end_points, dtds_for_spline, t_raw3);

		//TODO: CHECK CONVERSIONS

		//if (point_count % 100 == 0)
			//ROS_INFO_STREAM("num points: " << point_count );
		//Check these conversions
		out_msg.points[point_count].positions.push_back(holder_point.pos[0]);
		out_msg.points[point_count].positions.push_back(holder_point.pos[1]);
		out_msg.points[point_count].positions.push_back(holder_point.orientation);
		out_msg.points[point_count].velocities.push_back(cos(holder_point.path_angle) * curr_v );
		out_msg.points[point_count].velocities.push_back(sin(holder_point.path_angle) * curr_v );
		out_msg.points[point_count].velocities.push_back(holder_point.angular_velocity * curr_v / max_wheel_dist_);
		//out_msg.points[point_count].velocities.push_back(holder_point.path_angle_deriv * (current_v));
		out_msg.points[point_count].time_from_start = now;
		//ROS_INFO_STREAM(now);
		now += period;
		point_count++;
		if (!solve_for_next_V(holder_point, total_arc, curr_v, i, max_wheel_mid_accel_))
		{
			return false;
		}
		for (size_t k = 0; k < positions.size(); k++)
		{
			if (starting_point - k < 0 || positions[starting_point - k] > i)
			{
				starting_point -= k;
				break;
			}
			//Find point
		}
		coerce(starting_point, 1, 1000000000000);
		//Linear interpolation

		vel_cap = i * (velocities[starting_point] - velocities[starting_point + 1]) /
				  (positions[starting_point] - positions[starting_point + 1]) - positions[starting_point] *
				  (velocities[starting_point] - velocities[starting_point + 1]) /
				  (positions[starting_point] - positions[starting_point + 1]) + velocities[starting_point];
		//Keep below back pass
		//ROS_INFO_STREAM("pre cut max: " << curr_v);
		//if(curr_v > vel_cap)
		//{
		//    ROS_INFO_STREAM("cut by previous vel max: " << vel_cap << " curr_v: " << curr_v);
		//}
		coerce(curr_v, -100000000000, vel_cap);

		//ROS_INFO_STREAM("post cut max: " << curr_v);
	}
	//ROS_ERROR("finished raw generation");
	ROS_INFO_STREAM("time: " << point_count * dt_);
	ROS_INFO_STREAM("total_arc: " << total_arc);
	out_msg.points.erase(out_msg.points.begin() + point_count - 1, out_msg.points.end());
	ROS_ERROR_STREAM("p: " << out_msg.points.size());
	return true;
}
// TODO :: is return code needed here?
bool swerve_profiler::coerce(double &val, const double min, const double max)
{
	if (val > max)
	{
		val = max;
		return true;
	}
	else if (val < min)
	{
		val = min;
		return true;
	}
	else
	{
		return false;
	}
}
bool swerve_profiler::solve_for_next_V(const path_point &path, const double path_length, double &current_v, const double current_pos, double &accel_defined)
{
	//TODO: CHECK CONVERSIONS
	if (current_pos >= 0 && current_pos <= path_length)
	{
		const double max_wheel_orientation_accel = path.angular_accel * current_v * current_v;
		const double max_wheel_orientation_vel = fabs(path.angular_velocity) * current_v;
		const double theta = fabs(fmod(path.path_angle - path.orientation, M_PI / 8));
		const double cos_t = cos(theta);
		const double sin_t = sin(theta);
		const double path_induced_a = current_v * current_v / path.radius;

		//ROS_INFO_STREAM("max_a: " << accel_defined);

		// TODO : check return code here
		double accel;
		poly_solve(1, 4 * cos_t * sin_t * path_induced_a + sqrt(2)*cos_t * max_wheel_orientation_accel + sqrt(2)*sin_t * max_wheel_orientation_accel, path_induced_a * path_induced_a + sqrt(2)*sin_t * path_induced_a * max_wheel_orientation_accel + sqrt(2)*cos_t * path_induced_a * max_wheel_orientation_accel + max_wheel_orientation_accel * max_wheel_orientation_accel - accel_defined * accel_defined, accel);


		//ROS_INFO_STREAM("curr_v: " << current_v << " added accel: " << accel * dt_);

		current_v += accel * dt_;

		double v_general_max =  sqrt(max_wheel_vel_ * max_wheel_vel_ / (path.angular_velocity * fabs(path.angular_velocity) / 2 + 1 + sqrt(2) * fabs(path.angular_velocity) * cos_t + sqrt(2) * fabs(path.angular_velocity) * sin_t));

		//Note: assumption is that angular velocity doesn't change much over timestep
		//if(current_v > v_general_max)
		//{
		//	ROS_INFO_STREAM("cut by general max: " << v_general_max);
		//
		//}
		coerce(current_v, -v_general_max + .01, v_general_max - .01);
		//consider using above coerce in a if statement for optimization
		//ROS_INFO_STREAM("general max: " << current_v);

		double eff_max_a = accel_defined * 2 * ((max_wheel_vel_ - sqrt(current_v * current_v + sqrt(2) * max_wheel_orientation_vel * current_v * (sin_t + cos_t) + max_wheel_orientation_vel * max_wheel_orientation_vel)) / max_wheel_vel_);

		coerce(eff_max_a, .1, 2 * accel_defined); //Consider disabling this coerce
		double v_curve_max = sqrt(sqrt(eff_max_a * eff_max_a / (1 / (path.radius * path.radius) + sqrt(2) * sin_t * path.angular_accel / path.radius + sqrt(2) * cos_t * path.angular_accel / path.radius + path.angular_accel * path.angular_accel)));

		//if(current_v > v_curve_max)
		//{
		//	ROS_INFO_STREAM("cut by curve max: " << v_curve_max << " radius: " << path.radius << " eff_max_a: " << eff_max_a);
		//
		//}
		//ROS_INFO_STREAM("eff_max_a: " << eff_max_a << " under: " << (1/(path.radius * path.radius) + sqrt(2) * sin_t * path.angular_accel / path.radius + sqrt(2) * cos_t * path.angular_accel/path.radius + path.angular_accel * path.angular_accel)
		//<< " angular_accel: " << path.angular_accel
		//);

		coerce(current_v, -v_curve_max, v_curve_max);
		//ROS_INFO_STREAM("curve max: " << current_v);
	}
	else
	{
		current_v += accel_defined * dt_;
		//ROS_INFO_STREAM("off path +" << current_v);
		coerce(current_v, -max_wheel_vel_, max_wheel_vel_);
		//ROS_INFO_STREAM("off path coerce" << current_v);
	}
	return true;
}
tk::spline swerve_profiler::parametrize_spline(const std::vector<spline_coefs> &x_splines_first_deriv, const std::vector<spline_coefs> &y_splines_first_deriv, std::vector<double> end_points, double &total_arc_length, std::vector<double> &dtds_by_spline)
{
	//for( int i = 0; i < end_points.size(); i++)
	//{
		//ROS_WARN_STREAM("end: " << end_points[i]);
	//}
	total_arc_length = 0;
	double period_t = (end_points[0] - 0) / 100;
	double start = 0;
	double a_val;
	double b_val;
	double x_at_a;
	double x_at_b;
	double y_at_a;
	double y_at_b;
	double x_at_avg;
	double y_at_avg;
	double arc_before = 0;
	std::vector<double> t_vals;
	std::vector<double> s_vals;
	t_vals.reserve(x_splines_first_deriv.size() * 101);
	s_vals.reserve(x_splines_first_deriv.size() * 101);
	//ROS_INFO_STREAM("Running parametrize");

	//ROS_WARN_STREAM(x_splines_first_deriv.size());

	for (size_t i = 0; i < x_splines_first_deriv.size(); i++)
	{
		
		//ROS_INFO_STREAM("endpoints: " << end_points[i]);

		if (i != 0)
		{
			period_t = (end_points[i] - end_points[i - 1]) / 100.0; //100 is super arbitrary
			start = end_points[i - 1];
		}
		if (i > 1)
		{

			dtds_by_spline.push_back((end_points[i - 1] - end_points[i - 2]) /  (total_arc_length - arc_before));

		}
		else if (i == 1)
		{

			dtds_by_spline.push_back((end_points[0] - 0) /  (total_arc_length - arc_before));

		}
		arc_before = total_arc_length;
		for (size_t k = 0; k < 100; k++)
		{
			a_val = k * period_t + start;
			b_val = (k + 1) * period_t + start;
			t_vals.push_back(a_val);
			s_vals.push_back(total_arc_length);
			calc_point(x_splines_first_deriv[i], a_val, x_at_a);
			calc_point(x_splines_first_deriv[i], b_val, x_at_b);
			calc_point(y_splines_first_deriv[i], a_val, y_at_a);
			calc_point(y_splines_first_deriv[i], b_val, y_at_b);
			calc_point(x_splines_first_deriv[i], (a_val + b_val) / 2, x_at_avg);
			calc_point(y_splines_first_deriv[i], (a_val + b_val) / 2, y_at_avg);

			//f(t) = sqrt((dx/dt)^2 + (dy/dt)^2)

			total_arc_length += period_t / 6 * (sqrt(x_at_a * x_at_a + y_at_a * y_at_a) + 4 *
												sqrt(x_at_avg * x_at_avg + y_at_avg * y_at_avg) + sqrt(x_at_b * x_at_b + y_at_b * y_at_b));
			//ROS_INFO_STREAM("Spline: " << i << " t_val: " << a_val <<"  arc_length: " << total_arc_length);
		}
	}
	if(x_splines_first_deriv.size() == 1)
		dtds_by_spline.push_back((end_points[x_splines_first_deriv.size() - 1] - 0) /  (total_arc_length - arc_before));
	else
		dtds_by_spline.push_back((end_points[x_splines_first_deriv.size() - 1] - end_points[x_splines_first_deriv.size() - 2]) /  (total_arc_length - arc_before));
		
	
	t_vals.push_back(b_val);
	s_vals.push_back(total_arc_length);
	//TODO: Loop to generate set of s vals for t vals iterating from 0 to end_time (simpsons rule here)

	//Spline fit of t interms of s (we input a t -> s)
	tk::spline s;

	s.set_points(s_vals, t_vals);
	/*for(int i = 0; i < t_vals.size(); i++)
	{
		ROS_INFO_STREAM("t_val = " << t_vals[i] << " s vals = " << s_vals[i]);
		ROS_INFO_STREAM("s_vale = " << s_vals[i] << " s vals = " << s(s_vals[i]));



	}*/

	return s;
}
bool swerve_profiler::poly_solve(const double a, const double b, const double c, double &x)
{
	const double det = b * b - 4 * a * c;
	if (det < 0)
	{
		x = 0;
		return false;
	}
	else
	{
		x = (-b + sqrt(det)) / (2 * a); //This is just one of the roots, but it should be fine for all
		//cases it is used here
		return true;
	}
}
void swerve_profiler::calc_point(const spline_coefs &spline, double t, double &returner)
{
	if(flip_dirc_)t = t_total_ - t;
	t += t_shift_;
	const double t_squared = t * t;
	const double t_cubed   = t_squared * t;
	const double t_fourth  = t_squared * t_squared;
	const double t_fifth   = t_cubed * t_squared;
	returner = spline.a * t_fifth + spline.b * t_fourth + spline.c * t_cubed + spline.d * t_squared + spline.e * t + spline.f;
}
void swerve_profiler::comp_point_characteristics(const std::vector<spline_coefs> &x_splines, const std::vector<spline_coefs> &y_splines, const std::vector<spline_coefs> &x_splines_first_deriv, const std::vector<spline_coefs> &y_splines_first_deriv, const std::vector<spline_coefs> &x_splines_second_deriv, const std::vector<spline_coefs> &y_splines_second_deriv, const std::vector<spline_coefs> &orient_splines, const std::vector<spline_coefs> &orient_splines_first_deriv, const std::vector<spline_coefs> &orient_splines_second_deriv, path_point &holder_point, const std::vector<double> &end_points, const std::vector<double> &dtds_by_spline, const double t_raw)
{
	double t;
	size_t which_spline;
	which_spline = 0;
	t = t_raw;
	for (; which_spline < x_splines.size() - 1; which_spline++)
	{
		if (t_raw < end_points[which_spline])
		{
			break;
		}

	}

	double first_deriv_orient;
	double second_deriv_orient;
	double first_deriv_x;
	double first_deriv_y;
	double second_deriv_x;
	double second_deriv_y;

	calc_point(x_splines[which_spline], t, holder_point.pos[0]);
	calc_point(y_splines[which_spline], t, holder_point.pos[1]);
	calc_point(orient_splines[which_spline], t, holder_point.orientation);

	calc_point(x_splines_first_deriv[which_spline], t, first_deriv_x);
	calc_point(y_splines_first_deriv[which_spline], t, first_deriv_y);
	calc_point(x_splines_second_deriv[which_spline], t, second_deriv_x);
	calc_point(y_splines_second_deriv[which_spline], t, second_deriv_y);
	calc_point(orient_splines_first_deriv[which_spline], t, first_deriv_orient);
	calc_point(orient_splines_second_deriv[which_spline], t, second_deriv_orient);

	//if(fow)
	//ROS_INFO_STREAM("which spline: " << which_spline << " t_raw: "<< t << " x: " << holder_point.pos[0] << " y: " << holder_point.pos[1] << " a: " << x_splines[which_spline].a <<" b: " << x_splines[which_spline].b <<" c: " << x_splines[which_spline].c <<" d: " << x_splines[which_spline].d <<" e: " << x_splines[which_spline].e <<" f: " << x_splines[which_spline].f);

	//Radius = (x'^2 + y'^2)^(3/2) / (x' * y'' - y' * x'')

	const double denomin = (first_deriv_x * second_deriv_y - first_deriv_y * second_deriv_x);

	if (denomin != 0)
	{
		holder_point.radius = fabs(pow(first_deriv_x * first_deriv_x + first_deriv_y * first_deriv_y, 3.0 / 2.0) /
								   denomin);
	}
	else
	{
		holder_point.radius = 10000000000000000;
	}

	if (fabs(holder_point.pos[0] > 100) || fabs(holder_point.pos[1]) > 100)
	{
		//ROS_ERROR_STREAM("resonableness exceeded with x of: " << holder_point.pos[0] << " and y of: " << holder_point.pos[1] << " t: " << t);
	}

	holder_point.path_angle = atan2(first_deriv_y, first_deriv_x);  //Make sure this is what we want

	holder_point.angular_velocity = first_deriv_orient * dtds_by_spline[which_spline] * max_wheel_dist_;

	holder_point.angular_accel = fabs(second_deriv_orient * dtds_by_spline[which_spline] * dtds_by_spline[which_spline] * max_wheel_dist_ * ang_accel_conv_);
}
}
