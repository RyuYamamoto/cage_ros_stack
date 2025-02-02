// Copyright 2018-2020
//       Yoshitaka Hara <hara@furo.org>
//       Tomoaki Yoshida <yoshida@furo.org>
/*
This software is released under the MIT License.
http://opensource.org/licenses/mit-license.php
*/

#include "cageclient.hh"

#include <rclcpp/rclcpp.hpp>

#include <geometry_msgs/msg/transform_stamped.hpp>
#include <nav_msgs/msg/odometry.hpp>
#include <sensor_msgs/msg/imu.hpp>
#include <sensor_msgs/msg/nav_sat_fix.hpp>

#include <boost/function.hpp>
#include <boost/program_options.hpp>
#include <boost/program_options/variables_map.hpp>

#include <tf2/LinearMath/Quaternion.h>
#include <tf2_geometry_msgs/tf2_geometry_msgs.h>
#include <tf2_ros/transform_broadcaster.h>

#include <array>
#include <chrono>
#include <cmath>
#include <limits>

namespace bo = boost::program_options;
template <typename Type>
bool parseOption(bo::variables_map & vm, std::string option, Type & store)
{
  if (vm.count(option)) {
    store = vm[option].as<Type>();
    return true;
  }
  return false;
}

using Arr4d = std::array<double, 4>;
using Arr3d = std::array<double, 3>;

// Publisher
class RosBridge : public rclcpp::Node
{
  //ros::NodeHandle Node;
  std::shared_ptr<tf2_ros::TransformBroadcaster> TFb;
  struct PublisherDesc
  {
    rclcpp::PublisherBase::SharedPtr pub;
    std::string frame_id;
    std::string child_frame_id;
  };
  std::vector<PublisherDesc> Publishers;
  int ImuId;

public:
  RosBridge();

  template <typename msgType>
  int NewPublisher(
    std::string topic, uint32_t qlen, std::string frame_id, std::string child_frame_id);

  void PublishOdom(
    int navid, rclcpp::Time stamp, double x, double y, double z, tf2::Quaternion q, double vx,
    double az);
  // orientation and tfRot are quaternions and they must be in order {w, x, y, z}
  void PublishIMU(
    rclcpp::Time stamp, Arr4d orientation, Arr3d angvel, Arr3d accel, Arr3d tfTrans, Arr4d tfRot);
  void PublishNavSat(
    int pubid, rclcpp::Time stamp, double lat, double lon, Arr3d pos, tf2::Quaternion q,
    bool publishTF, unsigned int service = sensor_msgs::msg::NavSatStatus::SERVICE_GPS,
    int status = sensor_msgs::msg::NavSatStatus::STATUS_FIX);
  void PublishTFTransform(
    rclcpp::Time stamp, std::string frame_id, std::string child_frame_id, Arr3d translation,
    tf2::Quaternion q);

private:
  nav_msgs::msg::Odometry BuildNavMsg(
    double x, double y, double z, tf2::Quaternion q, double vx, double az);
};

// Odometry calculater
class SimpleOdometry
{
  const CageAPI::vehicleInfo Info;
  double LastClock;

public:
  double X = 0, Y = 0, Th = 0, Vx = 0, Az = 0;

public:
  SimpleOdometry(const CageAPI::vehicleInfo & info)
  : Info(info), LastClock(std::numeric_limits<double>::max()){};
  int Accumulate(const CageAPI::vehicleStatus & st);
  void Reset(double x = 0, double y = 0, double th = 0)
  {
    X = x;
    Y = y;
    Th = th;
  }
};

// ---------

RosBridge::RosBridge() : Node("cage_ros_bridge")
{
  TFb = std::make_shared<tf2_ros::TransformBroadcaster>(this);
  ImuId = NewPublisher<sensor_msgs::msg::Imu>("imu", 100, "base_link", "imu_link");
}

template <typename msgType>
int RosBridge::NewPublisher(
  std::string topic, uint32_t qlen, std::string frame_id, std::string child_frame_id)
{
  Publishers.push_back(
    PublisherDesc{this->create_publisher<msgType>(topic, qlen), frame_id, child_frame_id});
  return Publishers.size() - 1;
}

nav_msgs::msg::Odometry RosBridge::BuildNavMsg(
  double x, double y, double z, tf2::Quaternion q, double vx, double az)
{
  nav_msgs::msg::Odometry odo;
  odo.pose.pose.position.x = x;
  odo.pose.pose.position.y = y;
  odo.pose.pose.position.z = z;
  odo.pose.pose.orientation = tf2::toMsg(q);
  odo.pose.covariance = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                         0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  odo.twist.twist.linear.x = vx;
  odo.twist.twist.linear.y = 0.;
  odo.twist.twist.linear.z = 0.;
  odo.twist.twist.angular.x = 0.;
  odo.twist.twist.angular.y = 0.;
  odo.twist.twist.angular.z = az;
  odo.twist.covariance = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                          0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  return odo;
}

void RosBridge::PublishOdom(
  int pubid, rclcpp::Time stamp, double x, double y, double z, tf2::Quaternion q, double vx, double az)
{
  if (pubid >= Publishers.size()) return;
  const PublisherDesc & np = Publishers[pubid];

  // publish
  nav_msgs::msg::Odometry odo = BuildNavMsg(x, y, z, q, vx, az);
  odo.header.frame_id = np.frame_id;
  odo.header.stamp = stamp;
  odo.child_frame_id = np.child_frame_id;

  rclcpp::Publisher<nav_msgs::msg::Odometry>::SharedPtr odom_pub = std::dynamic_pointer_cast<rclcpp::Publisher<nav_msgs::msg::Odometry>>(np.pub);
  odom_pub->publish(odo);
  PublishTFTransform(stamp, np.frame_id, np.child_frame_id, {x, y, z}, q);
}

void RosBridge::PublishIMU(
  rclcpp::Time stamp, Arr4d orientation, Arr3d angvel, Arr3d accel, Arr3d tfTransform, Arr4d tfRot)
{
  const PublisherDesc & np = Publishers[ImuId];

  sensor_msgs::msg::Imu imu;
  imu.header.frame_id = np.frame_id;
  imu.header.stamp = stamp;
  imu.orientation.w = orientation[0];
  imu.orientation.x = orientation[1];
  imu.orientation.y = orientation[2];
  imu.orientation.z = orientation[3];
  imu.orientation_covariance = {0, 0, 0, 0, 0, 0, 0, 0, 0};
  imu.angular_velocity.x = angvel[0];
  imu.angular_velocity.y = angvel[1];
  imu.angular_velocity.z = angvel[2];
  imu.angular_velocity_covariance = {0, 0, 0, 0, 0, 0, 0, 0, 0};
  imu.linear_acceleration.x = accel[0];
  imu.linear_acceleration.y = accel[1];
  imu.linear_acceleration.z = accel[2];
  imu.linear_acceleration_covariance = {0, 0, 0, 0, 0, 0, 0, 0, 0};

  rclcpp::Publisher<sensor_msgs::msg::Imu>::SharedPtr imu_pub = std::dynamic_pointer_cast<rclcpp::Publisher<sensor_msgs::msg::Imu>>(np.pub);
  imu_pub->publish(imu);

  tf2::Quaternion q{
    tfRot[1],  // x
    tfRot[2],  // y
    tfRot[3],  // z
    tfRot[0]   // w
  };
  PublishTFTransform(stamp, np.frame_id, np.child_frame_id, tfTransform, q);
}

void RosBridge::PublishNavSat(
  int pubid, rclcpp::Time stamp, double lat, double lon, Arr3d pos, tf2::Quaternion q, bool publishTF,
  unsigned int service, int status)
{
  if (pubid >= Publishers.size()) return;
  const PublisherDesc & np = Publishers[pubid];

  // publish
  sensor_msgs::msg::NavSatFix nav;
  //auto frameid = np.child_frame_id;
  nav.header.frame_id = np.child_frame_id;
  nav.header.stamp = stamp;
  nav.status.service = service;
  nav.status.status = status;
  nav.latitude = lat;
  nav.longitude = lon;
  nav.altitude = std::numeric_limits<double>::quiet_NaN();
  nav.position_covariance[0] = 1;
  nav.position_covariance[4] = 1;
  nav.position_covariance[8] = 1;
  nav.position_covariance_type = sensor_msgs::msg::NavSatFix::COVARIANCE_TYPE_APPROXIMATED;
  rclcpp::Publisher<sensor_msgs::msg::NavSatFix>::SharedPtr nav_sat_fix_pub = std::dynamic_pointer_cast<rclcpp::Publisher<sensor_msgs::msg::NavSatFix>>(np.pub);
  nav_sat_fix_pub->publish(nav);
  if (publishTF) PublishTFTransform(stamp, np.frame_id, np.child_frame_id, pos, q);
}
void RosBridge::PublishTFTransform(
  rclcpp::Time stamp, std::string frame_id, std::string child_frame_id, Arr3d translation,
  tf2::Quaternion q)
{
  geometry_msgs::msg::TransformStamped tf_dev;
  tf_dev.header.stamp = stamp;
  tf_dev.header.frame_id = frame_id;
  tf_dev.child_frame_id = child_frame_id;
  tf_dev.transform.translation.x = translation[0];
  tf_dev.transform.translation.y = translation[1];
  tf_dev.transform.translation.z = translation[2];
  tf_dev.transform.rotation = tf2::toMsg(q);
  TFb->sendTransform(tf_dev);
}

// ---------

/*
   st を積算して位置姿勢を更新
   st.simClockが前回のsimClockより小さければ積算結果を0クリアする (return -1)
   st.simClockが前回から1ms未満しか変化していなければ無視する (return 0)
   そうでなければ位置姿勢を更新する (return 1)
*/
int SimpleOdometry::Accumulate(const CageAPI::vehicleStatus & st)
{
  if (LastClock > st.simClock) {
    // reset odometry
    LastClock = st.simClock;
    X = 0;
    Y = 0;
    Th = 0;
    return -1;
  }
  // simple odometry
  double dt = st.simClock - LastClock;
  //double ts;

  if (dt < 0.001)  // 1ms
    return 0;
  LastClock = st.simClock;

  double vr = -st.rrpm * Info.WheelPerimeterR / Info.ReductionRatio / 60.;
  double vl = st.lrpm * Info.WheelPerimeterL / Info.ReductionRatio / 60.;
  double vx = (vr + vl) / 2.;
  // az=(vr-vl)/treadWidth; // use wheel rotation and body parameters
  double az = st.rz;  // use angular velocity sensor
  double dx = vx * cos(Th) * dt;
  double dy = vx * sin(Th) * dt;
  X += dx;
  Y += dy;
  Th += az * dt;
  Vx = vx;
  Az = az;
  return 1;
}

// --------------------------------

class CageRosBridgeMain
{
  CageAPI Cage;
  std::shared_ptr<RosBridge> rosIF;
  std::unique_ptr<SimpleOdometry> Odo;

  rclcpp::executors::SingleThreadedExecutor executor;

  int pubOdom;
  int pubOdom_gt;
  int pubLatLon;
  rclcpp::Subscription<geometry_msgs::msg::Twist>::SharedPtr SubCmdVel;
  tf2::Quaternion WorldRotation;
  tf2::Quaternion QLidar;
  CageAPI::Transform LidarTransform;
  CageAPI::Transform IMUTransform;

  // int MapPublishingRatio=10;
  unsigned long long Seq = 0;

public:
  bool Ok = false;
  std::string Err;

  CageRosBridgeMain(std::string device) : Cage(device)
  {
    rosIF = std::make_shared<RosBridge>();
    // Register default transforms in case the server cannot provide such information
    Cage.setDefaultTransform("Lidar", {-.22, 0, .518}, /*rot (x,y,z,w)*/ {0, 0, 0, 1});
    Cage.setDefaultTransform("IMU", {0, 0, 0}, /*rot (x,y,z,w)*/ {0, 0, 0, 1});

    pubOdom = rosIF->NewPublisher<nav_msgs::msg::Odometry>("odom", 100, "odom", "base_link");
    pubOdom_gt = rosIF->NewPublisher<nav_msgs::msg::Odometry>("odom_gt", 100, "odom", "base_link_gt");
    pubLatLon = rosIF->NewPublisher<sensor_msgs::msg::NavSatFix>("gps/fix", 100, "base_link", "latlon");
    boost::function<void(const geometry_msgs::msg::Twist & r_msg)> fromRos = [this](const auto & msg) {
      if (!Cage.isValid()) return;
      //std::cout << "setVW:" << msg.linear.x << "," << msg.angular.z << std::endl;
      Cage.setVW(msg.linear.x, msg.angular.z);
    };
    SubCmdVel = rosIF->create_subscription<geometry_msgs::msg::Twist>("cmd_vel", 10, fromRos);

    //rclcpp::spin(rosIF);
  }

  ~CageRosBridgeMain() = default;

  std::shared_ptr<RosBridge> get() { return rosIF; }
  void set(std::shared_ptr<RosBridge> rosIF) { this->rosIF = rosIF; }

  bool initialize()
  {
    Seq = 0;

    if (!Cage.connect()) {
      Err = "Failed to connect:" + Cage.getError();
      Ok = false;
      return false;
    }

    CageAPI::vehicleStatus st;
    if (!Cage.getStatusOne(st, 100)) {
      Err = "getStatus failed:" + Cage.getError();
      Ok = false;
      return false;
    }

    Odo = std::make_unique<SimpleOdometry>(Cage.VehicleInfo);
    Odo->Accumulate(st);
    // ground truth
    LidarTransform = Cage.VehicleInfo.Transforms["Lidar"];
    QLidar = tf2::Quaternion{
      LidarTransform.rot[1],  // x
      LidarTransform.rot[2],  // y
      LidarTransform.rot[3],  // z
      LidarTransform.rot[0]   // w
    };
    IMUTransform = Cage.VehicleInfo.Transforms["IMU"];

    WorldRotation = tf2::Quaternion(0, 0, 0, 1);
    if (Cage.WorldInfo.valid) {
      auto geoRot = tf2::Quaternion(
        Cage.WorldInfo.ReferenceRotation[1], Cage.WorldInfo.ReferenceRotation[2],
        Cage.WorldInfo.ReferenceRotation[3], Cage.WorldInfo.ReferenceRotation[0]);
      tf2::Quaternion geo2world;
      geo2world.setRPY(0, 0, M_PI / 2.);
      WorldRotation = tf2::Quaternion(st.ox, st.oy, st.oz, st.ow) * geoRot * geo2world;
      std::cout << "World information\n lat0, lon0, x, y, z, q.w, q.x, q.y, q.z\n"
                << Cage.WorldInfo.Latitude0 << " " << Cage.WorldInfo.Longitude0 << " "
                << Cage.WorldInfo.ReferenceLocation[0] << " " << Cage.WorldInfo.ReferenceLocation[1]
                << " " << Cage.WorldInfo.ReferenceLocation[2] << " "
                << Cage.WorldInfo.ReferenceRotation[0] << " " << Cage.WorldInfo.ReferenceRotation[1]
                << " " << Cage.WorldInfo.ReferenceRotation[2] << " "
                << Cage.WorldInfo.ReferenceRotation[3] << std::endl;
    }

    std::cout << "Vehicle Parameters\n tread, wheelPerimeterR,L reductionRatio"
              << Cage.VehicleInfo.TreadWidth << " " << Cage.VehicleInfo.WheelPerimeterR << " "
              << Cage.VehicleInfo.WheelPerimeterL << " " << Cage.VehicleInfo.ReductionRatio
              << std::endl;

    Cage.setVW(0, 0);

    Err.clear();
    Ok = true;
    return true;
  }

  void Spin()
  {
    CageAPI::vehicleStatus st;
    if (!Cage.getStatusOne(st, 100)) {
      Err = "getStatus failed:" + Cage.getError();
      Ok = false;
      return;
    }
    ++Seq;
    // std::cout<<"st: "<<st.toString();

    auto stamp = rclcpp::Clock().now();

    auto ret = Odo->Accumulate(st);
    if (ret <= 0) {
      if (ret < 0)  // world was restarted
      {
        Ok = false;
        Err = "World restart detected";
      }  // else delta time was too small
      return;
    }

#if 0
      //std::cout<<"rrpm, lrpm, vr, vl, dt"<<st.rrpm<<" "<<st.lrpm<<" "<<vr<<"  "<<vl<<"  "<<dt<<std::endl;
      std::cout << "rx, ry, rz " << std::setw(12) << std::fixed << st.rz << " "
                << std::setw(12) << std::fixed << st.ry << " " << std::setw(12)
                << std::fixed << st.rz << std::endl;
#endif

    // odometry
    tf2::Quaternion q;
    q.setRPY(0, 0, Odo->Th);
    rosIF->PublishOdom(pubOdom, stamp, Odo->X, Odo->Y, 0, q, Odo->Vx, Odo->Az);

    // ground truth
    tf2::Quaternion qgt(st.ox, st.oy, st.oz, st.ow);
    rosIF->PublishOdom(pubOdom_gt, stamp, st.wx, st.wy, st.wz, qgt, Odo->Vx, st.rz);

    // Orientation: Ground Truth,  Angulrar velocity, Linear Acceleration: values from physics
    // engine
    rosIF->PublishIMU(
      stamp, {st.ow, st.ox, st.oy, st.oz}, {st.rx, st.ry, st.rz}, {st.ax, st.ay, st.az},
      IMUTransform.trans, IMUTransform.rot);

    // !! disable this feature for now
    bool publishMapTF = false;  // (Seq % MapPublishingRatio ==0);

    // latitude and longitude as gnss fix
    tf2::Quaternion qu(0, 0, 0, 1);
    rosIF->PublishNavSat(pubLatLon, stamp, st.latitude, st.longitude, {0, 0, 0}, qu, publishMapTF);

    // Scanner position
    //std::cout << qlidar.w() << ", " << qlidar.x() << ", " << qlidar.y() << ", " << qlidar.z() <<
    // ", " <<std::endl;
    rosIF->PublishTFTransform(stamp, "base_link", "lidar3d_link", LidarTransform.trans, QLidar);

    // world - map transform
    if (publishMapTF) rosIF->PublishTFTransform(stamp, "map", "odom", {0, 0, 0}, WorldRotation);

    //rclcpp::spin(rosIF);
    //executor.spin();
  }
};

int main(int argc, char ** argv)
try {
  bo::options_description opts("Options");
  opts.add_options()("help,h", "Show usage")("version,v", "Show program version");
  opts.add_options()("device,d", bo::value<std::string>());

  bo::variables_map vm;
  std::string device, port;
  std::vector<std::string> command;
  bool scan = false;
  int canspeed = 0;
  std::cout << "CageRos" << std::endl;
  std::cout << "   2019/02/20  yoshida@furo.org" << std::endl;

  try {
    bo::store(bo::parse_command_line(argc, argv, opts), vm);
    if (vm.count("help")) {
      std::cout << opts << std::endl;
      exit(0);
    }
    if (vm.count("version")) {
      exit(0);
    }
    if (!parseOption(vm, "device", device)) {
      std::cerr << "No device specified." << std::endl;
      exit(0);
    }
  } catch (std::exception & e) {
    std::cerr << e.what() << std::endl;
    std::cout << opts << std::endl;
    exit(0);
  }
  rclcpp::init(argc, argv);
  {
    CageRosBridgeMain Bridge(device);
    rclcpp::executors::SingleThreadedExecutor executor;
    while (rclcpp::ok()) {
      if (!Bridge.Ok) {
        std::cout << "Initializeing Bridge: " << Bridge.Err << std::endl;
        Bridge.initialize();
        continue;
      }
      Bridge.Spin();
      rclcpp::spin_some(Bridge.get());
    }
    std::cout << "Shutting down" << std::endl;
  }
  rclcpp::shutdown();
  return 0;
} catch (const std::exception & ex) {
  std::cerr << "EXCEPTION: " << ex.what() << std::endl;
  return 1;
}
