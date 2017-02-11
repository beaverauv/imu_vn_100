/*
 * Copyright 2016
 * Authors: [Ke Sun]
 *          Andre Phu-Van Nguyen <andre-phu-van.nguyen@polymtl.ca>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "vn/math/conversions.h"
#include "vn/math/kinematics.h"
#include "vn/math/math.h"
#include "vn/sensors/sensors.h"
#include <imu_vn_100/imu_vn_100.h>

namespace imu_vn_100 {

/**
 * @brief RosVector3FromVnVector3
 * @param ros_vec3
 * @param vn_vec3
 */
void RosVector3FromVnVector3(geometry_msgs::Vector3 &ros_vec3,
                             const vn::math::vec3f &vn_vec3);

/**
 * @brief RosQuaternionFromVnQuaternion
 * @param ros_quat
 * @param vn_vec4
 */
void RosQuaternionFromVnVector4(geometry_msgs::Quaternion &ros_quat,
                                const vn::math::vec4f &vn_vec4);

/**
 * @brief asciiOrBinaryAsyncMessageReceived
 * @param userData
 * @param p
 * @param index
 */
void asciiOrBinaryAsyncMessageReceived(void *userData,
                                       vn::protocol::uart::Packet &p,
                                       size_t index);

/**
 * @brief errorMessageReceived
 * @param userData
 * @param p
 * @param index
 */
void errorMessageReceived(void *userData, vn::protocol::uart::Packet &p,
                          size_t index);

constexpr int ImuVn100::kBaseImuRate;
constexpr int ImuVn100::kDefaultImuRate;
constexpr int ImuVn100::kDefaultSyncOutRate;

void ImuVn100::SyncInfo::Update(const unsigned sync_count,
                                const ros::Time &sync_time) {
  if (rate <= 0)
    return;
  std::lock_guard<std::mutex> lock(this->info_mutex);

  if (count != sync_count) {
    count = sync_count;
    time = sync_time;
  }
}

bool ImuVn100::SyncInfo::SyncEnabled() const { return rate > 0; }

void ImuVn100::SyncInfo::FixSyncRate() {
  // Check the sync out rate
  if (SyncEnabled()) {
    if (ImuVn100::kBaseImuRate % rate != 0) {
      rate = ImuVn100::kBaseImuRate / (ImuVn100::kBaseImuRate / rate);
      ROS_INFO("Set SYNC_OUT_RATE to %d", rate);
    }
    skip_count =
        (std::floor(ImuVn100::kBaseImuRate / static_cast<double>(rate) +
                    0.5f)) -
        1;

    if (pulse_width_us > 10000) {
      ROS_INFO("Sync out pulse with is over 10ms. Reset to 1ms");
      pulse_width_us = 1000;
    }
    rate_double = rate;
  }

  ROS_INFO("Sync out rate: %d", rate);
}

ImuVn100::ImuVn100(const ros::NodeHandle &pnh)
    : pnh_(pnh), port_(std::string("/dev/ttyUSB0")), baudrate_(921600),
      frame_id_(std::string("imu")) {
  Initialize();
}

ImuVn100::~ImuVn100() { Disconnect(); }

void ImuVn100::FixImuRate() {
  if (imu_rate_ <= 0) {
    ROS_WARN("Imu rate %d is < 0. Set to %d", imu_rate_, kDefaultImuRate);
    imu_rate_ = kDefaultImuRate;
  }

  if (kBaseImuRate % imu_rate_ != 0) {
    int imu_rate_old = imu_rate_;
    imu_rate_ = kBaseImuRate / (kBaseImuRate / imu_rate_old);
    ROS_WARN("Imu rate %d cannot evenly decimate base rate %d, reset to %d",
             imu_rate_old, kBaseImuRate, imu_rate_);
  }
}

void ImuVn100::LoadParameters() {
  pnh_.param<std::string>("port", port_, std::string("/dev/ttyUSB0"));
  pnh_.param<std::string>("frame_id", frame_id_, pnh_.getNamespace());
  pnh_.param("baudrate", baudrate_, 115200);
  pnh_.param("imu_rate", imu_rate_, kDefaultImuRate);

  pnh_.param("enable_mag", enable_mag_, true);
  pnh_.param("enable_pres", enable_pres_, true);
  pnh_.param("enable_temp", enable_temp_, true);

  pnh_.param("sync_rate", sync_info_->rate, kDefaultSyncOutRate);
  pnh_.param("sync_pulse_width_us", sync_info_->pulse_width_us, 1000);

  pnh_.param("binary_output", binary_output_, true);

  if (!binary_output_ && (enable_pres_ | enable_temp_)) {
    ROS_ERROR("VN: Ascii mode cannot support pressure and temp.");
    enable_pres_ = enable_temp_ = false;
  }

  int vn_serial_output_tmp = 4; // init on invalid number
  pnh_.param("vn_serial_output", vn_serial_output_tmp, 1);
  switch (vn_serial_output_tmp) {
  case 0:
    vn_serial_output_ = vn::protocol::uart::ASYNCMODE_NONE;
    break;
  case 1:
    vn_serial_output_ = vn::protocol::uart::ASYNCMODE_PORT1;
    break;
  case 2:
    vn_serial_output_ = vn::protocol::uart::ASYNCMODE_PORT2;
    break;
  case 3:
    vn_serial_output_ = vn::protocol::uart::ASYNCMODE_BOTH;
    break;
  default:
    ROS_ERROR("Incorrect VN serial port chosen.");
    break;
  }

  FixImuRate();
  sync_info_->FixSyncRate();
}

void ImuVn100::CreateDiagnosedPublishers() {
  imu_rate_double_ = imu_rate_;
  pd_imu_.Create<sensor_msgs::Imu>(pnh_, "imu", updater_, imu_rate_double_);
  pd_twist_.Create<geometry_msgs::TwistStamped>(pnh_, "twist", updater_,
                                                imu_rate_double_);
  if (enable_mag_) {
    pd_mag_.Create<sensor_msgs::MagneticField>(pnh_, "magnetic_field", updater_,
                                               imu_rate_double_);
  }
  if (enable_pres_) {
    pd_pres_.Create<sensor_msgs::FluidPressure>(pnh_, "fluid_pressure",
                                                updater_, imu_rate_double_);
  }
  if (enable_temp_) {
    pd_temp_.Create<sensor_msgs::Temperature>(pnh_, "temperature", updater_,
                                              imu_rate_double_);
  }
}

void ImuVn100::Initialize() {
  LoadParameters();
  unsigned int old_baudrate;
  // Try initial opening
  try {
    ROS_INFO("Connecting to device");
    imu_.connect(port_, 115200);
    ros::Duration(1).sleep();
    ROS_INFO("Connected to device at %s", port_.c_str());

    old_baudrate = imu_.readSerialBaudRate();
    ROS_INFO("Default serial baudrate: %u", old_baudrate);

    ROS_INFO("Set serial baudrate to %d", baudrate_);
    imu_.writeSerialBaudRate(baudrate_, true);

    ROS_INFO("Disconnecting the device");
    imu_.disconnect();
    ros::Duration(0.5).sleep();
  } catch (std::exception except) {
    ROS_INFO("Failed to open device with default baudrate with exception: %s",
             except.what());
  }

  // Open with the desired baud rate
  ROS_INFO("Reconnecting to device");
  imu_.connect(port_, baudrate_);
  ros::Duration(0.5).sleep();
  ROS_INFO("Connected to device at %s", port_.c_str());

  old_baudrate = imu_.readSerialBaudRate();
  ROS_INFO("New serial baudrate: %u", old_baudrate);

  ROS_INFO("Fetching device info.");
  std::string model_num = imu_.readModelNumber();
  ROS_INFO("Model number: %s", model_num.c_str());
  unsigned int hardw_rev = imu_.readHardwareRevision();
  ROS_INFO("Hardware revision: %d", hardw_rev);
  unsigned int serial_num = imu_.readSerialNumber();
  ROS_INFO("Serial number: %d", serial_num);
  std::string firmw_rev = imu_.readFirmwareVersion();
  ROS_INFO("Firmware version: %s", firmw_rev.c_str());

  if (sync_info_->SyncEnabled()) {
    ROS_INFO("Set Synchronization Control Register.");
    imu_.writeSynchronizationControl(
        vn::protocol::uart::SYNCINMODE_COUNT,
        vn::protocol::uart::SYNCINEDGE_RISING, 0,
        vn::protocol::uart::SYNCOUTMODE_ITEMSTART,
        vn::protocol::uart::SYNCOUTPOLARITY_POSITIVE, sync_info_->skip_count,
        sync_info_->pulse_width_us * 1000, true);

    if (!binary_output_) {
      ROS_INFO("Set Communication Protocol Control Register (id:30).");
      imu_.writeCommunicationProtocolControl(
          vn::protocol::uart::COUNTMODE_SYNCOUTCOUNTER,
          vn::protocol::uart::STATUSMODE_OFF,
          vn::protocol::uart::COUNTMODE_NONE,        // SPI
          vn::protocol::uart::STATUSMODE_OFF,        // SPI
          vn::protocol::uart::CHECKSUMMODE_CHECKSUM, // serial checksum is 8bit
          vn::protocol::uart::CHECKSUMMODE_CHECKSUM, // SPI
          vn::protocol::uart::ERRORMODE_SEND, true);
    }
  }

  CreateDiagnosedPublishers();

  auto hardware_id =
      std::string("vn100-") + model_num + std::to_string(serial_num);
  updater_.setHardwareID(hardware_id);
}

void ImuVn100::Stream(bool async) {
  // TODO There isn't a pause function in the new lib, what do here?
  using namespace vn::protocol::uart;

  if (async) {
    imu_.writeAsyncDataOutputType(VNOFF, true);

    if (binary_output_) {
      // Set the binary output data type and data rate
      vn::sensors::BinaryOutputRegister bor(
          vn_serial_output_, kBaseImuRate / imu_rate_,
          (COMMONGROUP_TIMESTARTUP | COMMONGROUP_QUATERNION |
           COMMONGROUP_MAGPRES | COMMONGROUP_SYNCINCNT),
          TIMEGROUP_NONE, (IMUGROUP_ACCEL | IMUGROUP_ANGULARRATE),
          GPSGROUP_NONE, ATTITUDEGROUP_NONE, INSGROUP_NONE);

      imu_.writeBinaryOutput1(bor, true);
    } else {
      // Set the ASCII output data type and data rate
      vn::sensors::BinaryOutputRegister bor(
          vn_serial_output_, 0, COMMONGROUP_NONE, TIMEGROUP_NONE, IMUGROUP_NONE,
          GPSGROUP_NONE, ATTITUDEGROUP_NONE, INSGROUP_NONE);
      // disable all the binary outputs
      imu_.writeBinaryOutput1(bor, true);
      imu_.writeBinaryOutput2(bor, true);
      imu_.writeBinaryOutput3(bor, true);
      imu_.writeAsyncDataOutputType(VNQMR, true);
    }

    // add a callback function for new data event
    imu_.registerAsyncPacketReceivedHandler(this,
                                            asciiOrBinaryAsyncMessageReceived);

    ROS_INFO("Setting IMU rate to %d", imu_rate_);
    imu_.writeAsyncDataOutputFrequency(imu_rate_, true);
  } else {
    // Mute the stream
    ROS_DEBUG("Mute the device");
    imu_.writeAsyncDataOutputType(VNOFF, true);
    // Remove the callback function for new data event
    try {
      imu_.unregisterAsyncPacketReceivedHandler();
    } catch (std::exception except) {
      ROS_WARN("Unable to unregister async packet handler: %s", except.what());
    }
  }
}

void ImuVn100::Resume(bool need_reply) {}

void ImuVn100::Idle(bool need_reply) {}

void ImuVn100::Disconnect() { imu_.disconnect(); }

void ImuVn100::PublishData(vn::protocol::uart::Packet &p) {
  sensor_msgs::Imu imu_msg;
  geometry_msgs::TwistStamped twist_msg;
  imu_msg.header.frame_id = frame_id_;

  vn::math::vec4f quaternion;
  vn::math::vec3f linear_accel;
  vn::math::vec3f angular_rate;
  vn::math::vec3f magnetometer;
  uint64_t time_since_startup;
  if (binary_output_) {
    // Note: With this library, we are responsible for extracting the data
    // in the appropriate order! Need to refer to manual and to how we
    // configured the common output group
    time_since_startup = p.extractUint64();
    if (first_publish_) {
      imu_msg.header.stamp = ros::Time::now();
      first_publish_ = false;
    } else {
      // basically offset the time using the vn100 clock instead of the ros
      // clock. Should get better timings this way
      ros::Duration vn100_integration_duration;
      vn100_integration_duration.fromNSec(time_since_startup -
                                          vn100_prev_timestamp_);
      imu_msg.header.stamp = ros_prev_timestamp_ + vn100_integration_duration;
    }
    ros_prev_timestamp_ = imu_msg.header.stamp;
    vn100_prev_timestamp_ = time_since_startup; // COMMONGROUP_TIMESTARTUP
    quaternion = p.extractVec4f();              // COMMONGROUP_QUATERNION
    magnetometer = p.extractVec3f();            // COMMONGROUP_MAGPRES
  } else {
    // In ascii mode, linear acceleration and angular velocity are NOT swapped
    p.parseVNQMR(&quaternion, &magnetometer, &linear_accel, &angular_rate);
  }

  twist_msg.header = imu_msg.header;

  if (enable_mag_) {
    sensor_msgs::MagneticField mag_msg;
    mag_msg.header = imu_msg.header;
    RosVector3FromVnVector3(mag_msg.magnetic_field, magnetometer);
    pd_mag_.Publish(mag_msg);
  }

  float temperature = p.extractFloat(); // COMMONGROUP_MAGPRES
  if (enable_temp_) {
    sensor_msgs::Temperature temp_msg;
    temp_msg.header = imu_msg.header;
    temp_msg.temperature = temperature;
    pd_temp_.Publish(temp_msg);
  }

  float pressure = p.extractFloat(); // COMMONGROUP_MAGPRES
  if (enable_pres_) {
    sensor_msgs::FluidPressure pres_msg;
    pres_msg.header = imu_msg.header;
    pres_msg.fluid_pressure = pressure;
    pd_pres_.Publish(pres_msg);
  }

  unsigned int syncInCnt = p.extractUint32(); // COMMONGROUP_SYNCINCNT
  linear_accel = p.extractVec3f();            // IMUGROUP_ACCEL
  angular_rate = p.extractVec3f();            // IMUGROUP_ANGULARRATE

  RosQuaternionFromVnVector4(imu_msg.orientation, quaternion);
  RosVector3FromVnVector3(imu_msg.angular_velocity, angular_rate);
  RosVector3FromVnVector3(imu_msg.linear_acceleration, linear_accel);

  RosVector3FromVnVector3(twist_msg.twist.angular, angular_rate);
  RosVector3FromVnVector3(twist_msg.twist.linear, linear_accel);

  sync_info_->Update(syncInCnt, imu_msg.header.stamp);
  pd_imu_.Publish(imu_msg);
  pd_twist_.Publish(twist_msg);

  updater_.update();
}

void RosVector3FromVnVector3(geometry_msgs::Vector3 &ros_vec3,
                             const vn::math::vec3f &vn_vec3) {
  ros_vec3.x = vn_vec3[0];
  ros_vec3.y = vn_vec3[1];
  ros_vec3.z = vn_vec3[2];
}

void RosQuaternionFromVnVector4(geometry_msgs::Quaternion &ros_quat,
                                const vn::math::vec4f &vn_vec4) {
  ros_quat.x = vn_vec4[0]; // see quaternion application note
  ros_quat.y = vn_vec4[1];
  ros_quat.z = vn_vec4[2];
  ros_quat.w = vn_vec4[3];
}

void asciiOrBinaryAsyncMessageReceived(void *userData,
                                       vn::protocol::uart::Packet &p,
                                       size_t index) {
  using namespace vn::protocol::uart;
  ImuVn100 *imu = (ImuVn100 *)userData;

  if (imu->IsBinaryOutput()) {
    if (!p.isCompatible((COMMONGROUP_TIMESTARTUP | COMMONGROUP_QUATERNION |
                         COMMONGROUP_MAGPRES | COMMONGROUP_SYNCINCNT),
                        TIMEGROUP_NONE, (IMUGROUP_ACCEL | IMUGROUP_ANGULARRATE),
                        GPSGROUP_NONE, ATTITUDEGROUP_NONE, INSGROUP_NONE)) {
      // Not the type of binary packet we are expecting.
      ROS_WARN("VN: Received malformatted binary packet.");
      return;
    }
  } else {
    // ascii format
    if (p.type() != vn::protocol::uart::Packet::TYPE_ASCII) {
      ROS_WARN("VN: Requested ascii, but got wrong type.");
      return;
    }

    if (p.determineAsciiAsyncType() != vn::protocol::uart::VNQMR) {
      ROS_WARN("VN: Wrong ascii format received.");
      return;
    }
  }

  if (!p.isValid()) {
    ROS_WARN("Vn: Invalid packet received. CRC or checksum failed.");
    return;
  }

  imu->PublishData(p);
}

void errorMessageReceived(void *userData, vn::protocol::uart::Packet &p,
                          size_t index) {
  using vn::protocol::uart::SensorError;
  ImuVn100 *imu = (ImuVn100 *)userData;
  SensorError se = p.parseError();

  if (se == 0)
    return;

  switch (se) {
  case SensorError::ERR_HARD_FAULT:
    ROS_ERROR("VN: Hard fault. Processor will force restart.");
    break;
  case SensorError::ERR_SERIAL_BUFFER_OVERFLOW: ///< Serial buffer overflow.
    // We tried sending some kind of crazy long command which is impossible
    // Throw because the developper shouldn't do this.
    throw std::runtime_error("VN: Serial buffer overflow.");
    break;
  case SensorError::ERR_INVALID_CHECKSUM: ///< Invalid checksum.
    ROS_WARN("VN: Invalid checksum on packet %s",
             std::to_string(index).c_str());
    break;
  case SensorError::ERR_INVALID_COMMAND: ///< Invalid command.
    ROS_WARN("VN: Invalid command on packet %s", std::to_string(index).c_str());
    break;
  case SensorError::ERR_NOT_ENOUGH_PARAMETERS: ///< Not enough parameters.
    ROS_WARN("VN: Not enough parameters.");
    break;
  case SensorError::ERR_TOO_MANY_PARAMETERS: ///< Too many parameters.
    ROS_WARN("VN: Too many parameters.");
    break;
  case SensorError::ERR_INVALID_PARAMETER: ///< Invalid parameter.
    ROS_WARN("VN: Invalid parameter.");
    break;
  case SensorError::ERR_INVALID_REGISTER: ///< Invalid register.
    ROS_WARN("VN: Invalid register.");
    break;
  case SensorError::ERR_UNAUTHORIZED_ACCESS: ///< Unauthorized access.
    ROS_WARN("VN: Unauthorized access to a register.");
    break;
  case SensorError::ERR_WATCHDOG_RESET: ///< Watchdog reset
    ROS_WARN(
        "VN: Watchdog reset has occured. VN should have restarted within 50 "
        "ms.");
    break;
  case SensorError::ERR_OUTPUT_BUFFER_OVERFLOW: ///< Output buffer overflow.
    ROS_WARN("VN: Output buffer overflow.");
    break;
  case SensorError::ERR_INSUFFICIENT_BAUD_RATE: ///< Insufficient baud rate.
    ROS_WARN("VN: Insufficient baud rate for requested async data output and "
             "rate.");
    break;
  case SensorError::ERR_ERROR_BUFFER_OVERFLOW: ///< Error buffer overflow.
    ROS_WARN("VN: System error buffer overflow.");
    break;
  default:
    throw std::runtime_error("VN: Unknown error code " + std::to_string(se));
    break;
  }
}

} // end namespace
