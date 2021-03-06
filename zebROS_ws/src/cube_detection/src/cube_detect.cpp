#include <iostream>
#include <opencv2/opencv.hpp>

#include <ros/ros.h>
#include <sensor_msgs/Image.h>
#include <sensor_msgs/image_encodings.h>
#include <message_filters/subscriber.h>
#include <message_filters/synchronizer.h>
#include <message_filters/sync_policies/approximate_time.h>

#include <geometry_msgs/Point32.h>
#include <std_msgs/Header.h>
#include "cube_detection/CubeDetection.h"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/highgui.hpp>

#include <sstream>
#include <vector>

#include "track3d.hpp"
#include "objtype.hpp"
#include "kalman.hpp"
#include "hungarian.hpp"

//#include "vid_reader.cpp"

#include <cv_bridge/cv_bridge.h>

int erosion_size = 1;
const int sliderMax = 255;
bool visualization;

static ros::Publisher pub;

using namespace cv;
using namespace std;
using namespace sensor_msgs;
using namespace message_filters;



//TEST VALUES
int hLo = 25;
int sLo = 102;
int vLo = 45;
int hUp = 47;

int maxTrans = 15900;
int minTrans = 7000;

int pixelError = .06;

//orig: 193695.3745 * .2226^x

//min: 193695.3745 * .2226^x - 9000

//max: 193695.3745 * .2226^x + 9000

static bool down_sample = false;
//This funtion along with the commented out slider code is useful when getting new HSV values for the threshold
//To get the trackbars active, comment out the lines marked "mark1", uncomment the lines marked "mark2",
//multiline comment from "markS" to "markE"

/*
void cubeDivision(Rect boundRect, Rect vRect){
		int numOfCubes;
		switch(abs((float)(boundRect.height)/(float)(boundRect.width)))
			case >= 1.3 and <= 2.3: numofCubes = 2

				Point bl = Point(boundRect.tl() + Point(0, vRect.height));
				Point tr = Point(boundRect.tl() + Point(vRect.width, 0));
		
		drawContours(drawing, contours,i,color,2,8,rank,0,Point());
		rectangle(drawing, boundRect[i].tl(), vRect.br(), rect_color, 2, 8, 0);
		rectangle(drawing, bl, boundRect[i].br(), rect_color, 2, 8, 0);


}
*/
void callback(const ImageConstPtr &frameMsg, const ImageConstPtr &depthMsg)
{
	cv_bridge::CvImageConstPtr cvFrame = cv_bridge::toCvShare(frameMsg, sensor_msgs::image_encodings::BGR8);
	cv_bridge::CvImageConstPtr cvDepth = cv_bridge::toCvShare(depthMsg, sensor_msgs::image_encodings::TYPE_32FC1); 
	
	// Avoid copies by using pointers to RGB and depth info
	// These pointers are either to the original data or to
	// the downsampled data, depending on the down_sample flag
	const Mat *framePtr = &cvFrame->image;
	const Mat *depthPtr = &cvDepth->image;
	

	// To hold downsampled images, if necessary
	Mat frame;
	Mat depth;

 	Mat gray;
	Mat thresh;

	// Downsample for speed purposes
	if (down_sample)
	{
		pyrDown(*framePtr, frame);
		pyrDown(*depthPtr, depth);

		framePtr = &frame;
		depthPtr = &depth;
		
	}
	Mat hsv;
	Mat threshold;
	Mat contour;
	
	// Create a kernel that we will use to sharpen our image
   	Mat kernel = (Mat_<float>(3,3) <<
        	          1,  1, 1,
        	          1, -8, 1,
        	          1,  1, 1); 
    	//Mat imgLaplacian;
    	//filter2D(frame, imgLaplacian, CV_32F, kernel);
    	//Mat sharp;
	//frame.convertTo(sharp, CV_32F);
    	//Mat imgResult = sharp - imgLaplacian;
    	// convert back to 8bits gray scale
    	//imgResult.convertTo(imgResult, CV_8UC3);
    	//imgLaplacian.convertTo(imgLaplacian, CV_8UC3);
   	// imshow( "Laplace Filtered Image", imgLaplacian);
    	//imshow( "New Sharped Image", imgResult);

	cvtColor(*framePtr,hsv,COLOR_BGR2HSV);
	cvtColor(*framePtr, gray, COLOR_BGR2GRAY);
	inRange(hsv, Scalar(hLo,sLo,vLo),Scalar(hUp,255,255),threshold); //mark2

	erode(threshold,threshold,getStructuringElement(MORPH_ELLIPSE,Size(7,7)));
	dilate(threshold,threshold,getStructuringElement(MORPH_ELLIPSE,Size(7,7)));

	dilate(threshold,threshold,getStructuringElement(MORPH_ELLIPSE,Size(6,6)));
	erode(threshold,threshold,getStructuringElement(MORPH_ELLIPSE,Size(5,5)));

	blur(gray, gray, Size(3,3));

	vector<vector<Point> > contours;
	vector<Vec4i> rank;

	findContours(threshold, contours, rank, CV_RETR_TREE, CV_CHAIN_APPROX_SIMPLE, Point(0,0));

	vector<vector<Point>> contours_poly(contours.size());
	vector<Rect> boundRect(contours.size());
	vector<float> contourDepth;
	vector<int> approxPoly;
	Mat drawing = Mat::zeros(threshold.size(),CV_8UC3);

	cube_detection::CubeDetection cd_msg;

	
	const ObjectType objType = CUBE_2018;
	const float hFov = 105.;
	const Point2f fov(hFov * (M_PI / 180.), hFov * (M_PI / 180.) * ((float)framePtr->rows / framePtr->cols));
	float camera_elevation = 0.0;


	for(size_t i = 0; i < contours.size(); i++)
	{
		approxPolyDP(Mat(contours[i]),contours_poly[i], 3, true);
		boundRect[i] = boundingRect(Mat(contours_poly[i]));
		
	}

	for(size_t i = 0; i < contours.size(); i++)
	{
		const float x = boundRect[i].x + (boundRect[i].width/2);
		const float y = boundRect[i].y + (boundRect[i].height/2);
		vector<float> depth_sample;
		for(size_t idx = boundRect[i].width/3; idx < (boundRect[i].width/3)*2; idx++){
			for(size_t idy = boundRect[i].height/3; idy < (boundRect[i].height/3)*2; idy++){
				float depthAtPixel = depthPtr->at<float>(boundRect[i].y + idy, boundRect[i].x + idx);
				depth_sample.push_back(depthAtPixel);			
			}
		}
		float depth_sum;
		
		for(size_t id = 0; id < depth_sample.size(); id++){
			depth_sum = depth_sum + depth_sample[id];

		}
		for(size_t ind = 0; ind < depth_sample.size(); ind++){
			if(isnan(depth_sample[ind]) == 0) {
				contourDepth.push_back(depth_sum/depth_sample.size());
				//ROS_INFO_STREAM(depth_sum/depth_sample.size() << endl);
			}
		}

	}		
	
	
		cd_msg.header.seq = frameMsg->header.seq;
		cd_msg.header.stamp = frameMsg->header.stamp;
		cd_msg.header.frame_id = frameMsg->header.frame_id;
	for(size_t i = 0; i< contours.size(); i++)
	{
		
		
		double minArea = sqrt(193695.3745 * (pow(0.2226,contourDepth[i]))) + minTrans; 
		//double maxArea = sqrt(193695.3745 * (pow(0.2226,contourDepth[i])) + maxTrans); 
		double areaRect = boundRect[i].height * boundRect[i].width;
		Scalar rect_color = Scalar(0,0,255);
		Scalar color = Scalar(0,255,0);	
		//filter contours by area based on depth and side ratio
		if (areaRect < minArea) {
			continue;
		} else if (areaRect <= (drawing.rows * drawing.cols * pixelError)) {
			continue;
		} else if (abs((float)(boundRect[i].height)/(float)(boundRect[i].width)) > 1.35 && 
			   abs((float)(boundRect[i].height)/(float)(boundRect[i].width)) < 2.2) {
			Rect vRect = Rect(boundRect[i].x, boundRect[i].y, boundRect[i].width, boundRect[i].height / 2);

			Point bl = Point(boundRect[i].tl() + Point(0, vRect.height));
			Point tr = Point(boundRect[i].tl() + Point(vRect.width, 0));
			
			drawContours(drawing, contours,i,color,2,8,rank,0,Point());
			rectangle(drawing, boundRect[i].tl(), vRect.br(), rect_color, 2, 8, 0);
			rectangle(drawing, bl, boundRect[i].br(), rect_color, 2, 8, 0);

			const Point3f world_location = objType.screenToWorldCoords(boundRect[i], contourDepth[i], fov, framePtr->size(), camera_elevation);
			geometry_msgs::Point32 world_location_in; 
			world_location_in.x = world_location.y;
			world_location_in.y = world_location.x;
			world_location_in.z = world_location.z;
			cd_msg.location.push_back(world_location_in);
			cd_msg.angle = atan(world_location.y/world_location.x);

		} else if (abs(((float)boundRect[i].width/(float)boundRect[i].height)) > 1.35 && 
			   abs(((float)boundRect[i].width/(float)boundRect[i].height)) < 2.2) {
			Rect hRect = Rect(boundRect[i].x, boundRect[i].y, boundRect[i].width / 2, boundRect[i].height);

			//Point bl = Point(boundRect[i].tl() + Point(0, hRect.height));
			Point tr = Point(boundRect[i].tl() + Point(hRect.width, 0));

			drawContours(drawing, contours,i,color,2,8,rank,0,Point());
			rectangle(drawing, boundRect[i].tl(), hRect.br(), rect_color, 2, 8, 0);
			rectangle(drawing, tr, boundRect[i].br(), rect_color, 2, 8, 0);

			const Point3f world_location = objType.screenToWorldCoords(boundRect[i], contourDepth[i], fov, framePtr->size(), camera_elevation);
			geometry_msgs::Point32 world_location_in; 
			world_location_in.x = world_location.y;
			world_location_in.y = world_location.x;
			world_location_in.z = world_location.z;
			cd_msg.location.push_back(world_location_in);
			cd_msg.angle = atan(world_location.y/world_location.x);
			
		} else if (abs((float)(boundRect[i].height)/(float)(boundRect[i].width)) > 2.2) {
			Rect vRect = Rect(boundRect[i].x, boundRect[i].y, boundRect[i].width, boundRect[i].height / 3);

			Point bl = Point(boundRect[i].tl() + Point(0, vRect.height));
			Point tr = Point(boundRect[i].tl() + Point(vRect.width, 0));
			Point bbl = Point(boundRect[i].tl() + Point(0, vRect.height * 2));
			Point bbr = Point(boundRect[i].tl() + Point(vRect.width, vRect.height * 2));			

			drawContours(drawing, contours,i,color,2,8,rank,0,Point());
			rectangle(drawing, boundRect[i].tl(), vRect.br(), rect_color, 2, 8, 0);

			rectangle(drawing, bl, bbr, rect_color, 2, 8, 0);

			rectangle(drawing, bbl, boundRect[i].br(), rect_color, 2, 8, 0);

			const Point3f world_location = objType.screenToWorldCoords(boundRect[i], contourDepth[i], fov, framePtr->size(), camera_elevation);
			geometry_msgs::Point32 world_location_in; 
			world_location_in.x = world_location.y;
			world_location_in.y = world_location.x;
			world_location_in.z = world_location.z;
			cd_msg.location.push_back(world_location_in);
			cd_msg.angle = atan(world_location.y/world_location.x);

		} else if (abs(((float)boundRect[i].width/(float)boundRect[i].height)) > 2.2) {
			Rect hRect = Rect(boundRect[i].x, boundRect[i].y, boundRect[i].width / 3, boundRect[i].height);	

			Point bl = Point(boundRect[i].tl() + Point(0, hRect.height));
			Point tr = Point(boundRect[i].tl() + Point(hRect.width, 0));
			Point rrb = Point(boundRect[i].tl() + Point(hRect.width * 2, hRect.height));
			Point rrt = Point(boundRect[i].tl() + Point(hRect.width * 2, 0));

			drawContours(drawing, contours,i,color,2,8,rank,0,Point());
			rectangle(drawing, boundRect[i].tl(), hRect.br(), rect_color, 2, 8, 0);
			
			rectangle(drawing, tr, rrb, rect_color, 2, 8, 0);

			rectangle(drawing, rrt, boundRect[i].br(), rect_color, 2, 8, 0);

			const Point3f world_location = objType.screenToWorldCoords(boundRect[i], contourDepth[i], fov, framePtr->size(), camera_elevation);
			geometry_msgs::Point32 world_location_in; 
			world_location_in.x = world_location.y;
			world_location_in.y = world_location.x;
			world_location_in.z = world_location.z;
			cd_msg.location.push_back(world_location_in);
			cd_msg.angle = atan(world_location.y/world_location.x);

		} else if (contours_poly[i].size() < 4) {
			continue;
		} else {
			double areaContour = contourArea(contours[i]);
			putText(drawing, to_string(contourDepth[i]), Point(boundRect[i].x, boundRect[i].y - 15), FONT_HERSHEY_SIMPLEX, 0.45, (0,0,255), 1);
			drawContours(drawing, contours,i,color,2,8,rank,0,Point());
			rectangle(drawing, boundRect[i].tl(), boundRect[i].br(), rect_color, 2, 8, 0);
			
			const Point3f world_location = objType.screenToWorldCoords(boundRect[i], contourDepth[i], fov, framePtr->size(), camera_elevation);
			geometry_msgs::Point32 world_location_in; 
			world_location_in.x = world_location.y;
			world_location_in.y = world_location.x;
			world_location_in.z = world_location.z;
			cd_msg.location.push_back(world_location_in);
			cd_msg.angle = atan(world_location.y/world_location.x);
		}
	}


	if(visualization == true) {
	imshow("threshold",threshold); 
	//imshow("hsv",hsv);
	imshow("drawing",drawing); 
	imshow("image", *framePtr);
	//imshow("depth", *depthPtr);
	
	namedWindow("drawing",1);
		
	//trackbars for testing hsv and range values
	createTrackbar( "Lower H", "drawing", &hLo, 180);
	createTrackbar( "Lower S", "drawing", &sLo, 255);  
	createTrackbar( "Lower V", "drawing", &vLo, 255);
	createTrackbar( "Higher H", "drawing", &hUp, 180);
	createTrackbar( "minTrans", "drawing", &minTrans, 30000);
	createTrackbar( "maxTrans", "drawing", &maxTrans, 30000);	
	//createTrackbar( "minArea", "drawing", &minArea, 5000);
	//createTrackbar( "maxArea", "drawing", &maxArea, 1000000);

	}
	pub.publish(cd_msg);
	waitKey(5);

}

int main(int argc, char **argv)
{
	ros::init(argc, argv, "cube_detect");

	ros::NodeHandle nh("~");
	int sub_rate = 5;
	int pub_rate = 1;
	
	message_filters::Subscriber<Image> frame_sub(nh, "/zed_goal/left/image_rect_color", sub_rate);
	message_filters::Subscriber<Image> depth_sub(nh, "/zed_goal/depth/depth_registered", sub_rate);

	typedef sync_policies::ApproximateTime<Image, Image > MySyncPolicy2;
	// ApproximateTime takes a queue size as its constructor argument, hence MySyncPolicy(xxx)
	Synchronizer<MySyncPolicy2> sync2(MySyncPolicy2(50), frame_sub, depth_sub);
	sync2.registerCallback(boost::bind(&callback, _1, _2));

	pub = nh.advertise<cube_detection::CubeDetection>("cube_detect_msg", pub_rate);

	
	nh.getParam("visualization", visualization);

	

	ros::spin();

	return 0;
}
