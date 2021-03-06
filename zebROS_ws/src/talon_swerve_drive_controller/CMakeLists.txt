cmake_minimum_required(VERSION 2.8.3)
project(talon_swerve_drive_controller)

include ("../cmake_modules/CMakeOpt.cmake")

find_package(catkin REQUIRED COMPONENTS
    cmake_modules
    controller_interface
    std_msgs
	std_srvs
    nav_msgs
    geometry_msgs
	swerve_math
    trajectory_msgs
    realtime_tools
    talon_interface
    talon_controllers
    tf
    urdf
    message_generation
)

find_package(Eigen3 REQUIRED)

add_message_files (
	FILES
	SwervePoint.msg
	SwervePointSet.msg
	)

add_service_files (
  FILES
  MotionProfile.srv
  MotionProfilePoints.srv
  WheelPos.srv
)

generate_messages(
  DEPENDENCIES
  std_msgs
  geometry_msgs
  trajectory_msgs
  talon_swerve_drive_controller
)

catkin_package(
  CATKIN_DEPENDS
  controller_interface
  INCLUDE_DIRS include
  #LIBRARIES ${PROJECT_NAME}
  DEPENDS
)

include_directories(
  include ${catkin_INCLUDE_DIRS}
  ${EIGEN3_INCLUDE_DIR}
)

add_library(${PROJECT_NAME} 
	src/swerve_drive_controller.cpp 
	src/odometry.cpp 
	src/speed_limiter.cpp
)

add_dependencies(${PROJECT_NAME}
  ${${PROJECT_NAME}_EXPORTED_TARGETS}
  ${catkin_EXPORTED_TARGETS}
  talon_swerve_drive_controller_generate_messages_cpp
)

target_link_libraries(${PROJECT_NAME} ${catkin_LIBRARIES})

install(DIRECTORY include/${PROJECT_NAME}/
        DESTINATION ${CATKIN_PACKAGE_INCLUDE_DESTINATION})

install(TARGETS ${PROJECT_NAME} 
  ARCHIVE DESTINATION ${CATKIN_PACKAGE_LIB_DESTINATION}
  LIBRARY DESTINATION ${CATKIN_PACKAGE_LIB_DESTINATION}
  RUNTIME DESTINATION ${CATKIN_PACKAGE_BIN_DESTINATION}
  )

install(FILES ${PROJECT_NAME}_plugins.xml
    DESTINATION ${CATKIN_PACKAGE_SHARE_DESTINATION})

