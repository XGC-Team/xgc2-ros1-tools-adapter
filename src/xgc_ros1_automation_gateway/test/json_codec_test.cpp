#include "xgc_ros1_automation_gateway/json_codec.hpp"

#include <gtest/gtest.h>
#include <json/json.h>

#include <cstdint>
#include <limits>
#include <string>

#include "xgc_ros1_automation_gateway/error.hpp"
#include "xgc_ros1_automation_gateway/type_registry.hpp"

namespace xgc_ros1_automation_gateway {
namespace {

TEST(JsonCodec, RoundTripsEveryPrimitiveScalarType) {
  TypeRegistry types;
  JsonCodec codec;
  const std::pair<const char*, Json::Value> cases[] = {
      {"std_msgs/Bool", Json::Value(true)},
      {"std_msgs/UInt8", Json::Value(Json::UInt(255))},
      {"std_msgs/UInt16", Json::Value(Json::UInt(65535))},
      {"std_msgs/UInt32",
       Json::Value(Json::UInt64(std::numeric_limits<std::uint32_t>::max()))},
      {"std_msgs/UInt64", Json::Value("18446744073709551615")},
      {"std_msgs/Int8", Json::Value(Json::Int(-128))},
      {"std_msgs/Int16", Json::Value(Json::Int(-32768))},
      {"std_msgs/Int32",
       Json::Value(Json::Int64(std::numeric_limits<std::int32_t>::min()))},
      {"std_msgs/Int64", Json::Value("-9223372036854775808")},
      {"std_msgs/Float32", Json::Value(1.25)},
      {"std_msgs/Float64", Json::Value(-2.5)},
      {"std_msgs/String", Json::Value("XGC2 机器人")},
  };

  for (const auto& item : cases) {
    SCOPED_TRACE(item.first);
    auto message = types.createMessage(item.first);
    Json::Value input(Json::objectValue);
    input["data"] = item.second;
    codec.decode(input, *message);
    const Json::Value output = codec.encode(*message);
    EXPECT_EQ(item.second, output["data"]);
    EXPECT_GT(types.serialize(message)->size(), 0U);
  }
}

TEST(JsonCodec, HandlesNestedTimeAndFixedArrays) {
  TypeRegistry types;
  JsonCodec codec;

  auto header = types.createMessage("std_msgs/Header");
  Json::Value header_json(Json::objectValue);
  header_json["seq"] = Json::UInt(42);
  header_json["stamp"]["secs"] = Json::UInt(123);
  header_json["stamp"]["nsecs"] = Json::UInt(456);
  header_json["frame_id"] = "map";
  codec.decode(header_json, *header);
  EXPECT_EQ(header_json, codec.encode(*header));

  auto status = types.createMessage("mavros_msgs/OnboardComputerStatus");
  Json::Value status_json = codec.encode(*status);
  ASSERT_EQ(8U, status_json["cpu_cores"].size());
  status_json["cpu_cores"][0] = Json::UInt(99);
  status_json["temperature_core"][0] = Json::Int(-20);
  status_json["fan_speed"][0] = Json::Int(1200);
  codec.decode(status_json, *status);
  const Json::Value output = codec.encode(*status);
  EXPECT_EQ(99U, output["cpu_cores"][0].asUInt());
  EXPECT_EQ(-20, output["temperature_core"][0].asInt());
  EXPECT_EQ(1200, output["fan_speed"][0].asInt());
}

TEST(JsonCodec, HandlesDynamicPrimitiveAndCompoundArrays) {
  TypeRegistry types;
  JsonCodec codec;

  auto mavlink = types.createMessage("mavros_msgs/Mavlink");
  Json::Value mavlink_json = codec.encode(*mavlink);
  mavlink_json["payload64"].append("18446744073709551615");
  mavlink_json["signature"].append(Json::UInt(0));
  mavlink_json["signature"].append(Json::UInt(255));
  codec.decode(mavlink_json, *mavlink);
  EXPECT_EQ(mavlink_json, codec.encode(*mavlink));

  auto waypoints = types.createMessage("mavros_msgs/WaypointList");
  Json::Value waypoint_json = codec.encode(*waypoints);
  Json::Value waypoint(Json::objectValue);
  waypoint["frame"] = Json::UInt(3);
  waypoint["command"] = Json::UInt(16);
  waypoint["is_current"] = true;
  waypoint["autocontinue"] = true;
  waypoint["param1"] = 0.0;
  waypoint["param2"] = 1.0;
  waypoint["param3"] = 2.0;
  waypoint["param4"] = 3.0;
  waypoint["x_lat"] = 47.0;
  waypoint["y_long"] = 8.0;
  waypoint["z_alt"] = 10.0;
  waypoint_json["waypoints"].append(waypoint);
  codec.decode(waypoint_json, *waypoints);
  EXPECT_EQ(waypoint_json, codec.encode(*waypoints));
}

TEST(JsonCodec, RejectsUnknownFieldsOverflowAndWrongFixedLength) {
  TypeRegistry types;
  JsonCodec codec;

  auto text = types.createMessage("std_msgs/String");
  Json::Value unknown(Json::objectValue);
  unknown["data"] = "ok";
  unknown["noise"] = true;
  EXPECT_THROW(codec.decode(unknown, *text), GatewayError);

  auto value = types.createMessage("std_msgs/UInt8");
  Json::Value overflow(Json::objectValue);
  overflow["data"] = Json::UInt(256);
  EXPECT_THROW(codec.decode(overflow, *value), GatewayError);

  auto status = types.createMessage("mavros_msgs/OnboardComputerStatus");
  Json::Value status_json = codec.encode(*status);
  status_json["cpu_cores"].resize(7);
  EXPECT_THROW(codec.decode(status_json, *status), GatewayError);
}

TEST(JsonCodec, RepresentsInt64ExactlyAsDecimalStrings) {
  TypeRegistry types;
  JsonCodec codec;
  auto unsigned_value = types.createMessage("std_msgs/UInt64");
  Json::Value input(Json::objectValue);
  input["data"] = "9007199254740993";
  codec.decode(input, *unsigned_value);
  EXPECT_TRUE(codec.encode(*unsigned_value)["data"].isString());
  EXPECT_EQ("9007199254740993",
            codec.encode(*unsigned_value)["data"].asString());
}

}  // namespace
}  // namespace xgc_ros1_automation_gateway
