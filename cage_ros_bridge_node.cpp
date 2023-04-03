#include "cageclient.hh"

#include <rclcpp/rclcpp.hpp>

#include <geometry_msgs/msg/transform_stamped.hpp>
#include <nav_msgs/msg/odometry.hpp>
#include <sensor_msgs/msg/imu.hpp>
#include <sensor_msgs/msg/nav_sat_fix.hpp>

class RosBridge : public rclcpp::Node
{
  public:
    RosBridge() : Node("cage_ros_bridge")
    {}
  private:
};

int main(int argc, char** argv)
{
}
