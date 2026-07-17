#include <gtest/gtest.h>

#include <string>

#include "xgc_ros1_tools_adapter/error.hpp"
#include "xgc_ros1_tools_adapter/generated_contract.hpp"
#include "xgc_ros1_tools_adapter/tools_adapter.hpp"

namespace xgc_ros1_tools_adapter {
namespace {

constexpr char kScopeKey[] =
    "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

xgc::v1::SchemaReference schema(const contract::Schema& source) {
  xgc::v1::SchemaReference result;
  result.set_message_id(source.message_id);
  result.set_type_name(source.type_name);
  result.set_schema_version(source.version);
  result.set_schema_fingerprint(source.fingerprint);
  return result;
}

xgc::adapter::v1::AdapterInstanceSpec nativeContextSpec(
    const std::string& master_uri = "http://127.0.0.1:11311",
    const std::string& ros_ip = "", const std::string& ros_hostname = "") {
  xgc::adapter::v1::AdapterInstanceSpec spec;
  *spec.mutable_configuration()->mutable_schema() =
      schema(contract::kConfiguration);
  spec.mutable_configuration()->set_encoding(xgc::v1::PAYLOAD_ENCODING_JSON);
  spec.mutable_configuration()->set_value(
      "{\"rosMasterUri\":\"" + master_uri + "\",\"rosIp\":\"" + ros_ip +
      "\",\"rosHostname\":\"" + ros_hostname + "\"}");
  auto* scope = spec.mutable_scope();
  scope->set_kind("ros1-native-context");
  scope->set_key(kScopeKey);
  (*scope->mutable_attributes())["master-uri"] = master_uri;
  if (!ros_ip.empty()) {
    (*scope->mutable_attributes())["ip"] = ros_ip;
  }
  if (!ros_hostname.empty()) {
    (*scope->mutable_attributes())["hostname"] = ros_hostname;
  }
  return spec;
}

TEST(NativeContext, ParsesExactConfigurationAndScope) {
  const NativeContext context = NativeContext::FromInstanceSpec(
      nativeContextSpec("http://ros.example.test:11311/", "", "robot.local"));

  EXPECT_EQ("http://ros.example.test:11311/", context.ros_master_uri);
  EXPECT_TRUE(context.ros_ip.empty());
  EXPECT_EQ("robot.local", context.ros_hostname);
  EXPECT_EQ(kScopeKey, context.scope_key);
}

TEST(NativeContext, RejectsInvalidPortAndAmbiguousNetworkIdentity) {
  EXPECT_THROW(
      NativeContext::FromInstanceSpec(nativeContextSpec("http://127.0.0.1:0")),
      Ros1ToolsError);
  EXPECT_THROW(NativeContext::FromInstanceSpec(
                   nativeContextSpec("http://127.0.0.1:99999")),
               Ros1ToolsError);
  EXPECT_THROW(NativeContext::FromInstanceSpec(nativeContextSpec(
                   "http://127.0.0.1:11311", "127.0.0.1", "robot.local")),
               Ros1ToolsError);
  EXPECT_THROW(NativeContext::FromInstanceSpec(
                   nativeContextSpec("https://127.0.0.1:11311")),
               Ros1ToolsError);
  EXPECT_THROW(NativeContext::FromInstanceSpec(nativeContextSpec(
                   "http://127.0.0.1:11311", "not-an-ip", "")),
               Ros1ToolsError);
  EXPECT_THROW(NativeContext::FromInstanceSpec(nativeContextSpec(
                   "http://127.0.0.1:11311", "", "bad..hostname")),
               Ros1ToolsError);
}

TEST(NativeContext, RejectsScopeDriftAndAdditionalAttributes) {
  auto spec = nativeContextSpec();
  (*spec.mutable_scope()->mutable_attributes())["master-uri"] =
      "http://127.0.0.1:11312";
  EXPECT_THROW(NativeContext::FromInstanceSpec(spec), Ros1ToolsError);

  spec = nativeContextSpec();
  (*spec.mutable_scope()->mutable_attributes())["legacy"] = "forbidden";
  EXPECT_THROW(NativeContext::FromInstanceSpec(spec), Ros1ToolsError);

  spec = nativeContextSpec();
  spec.mutable_scope()->set_key("sha256:not-canonical");
  EXPECT_THROW(NativeContext::FromInstanceSpec(spec), Ros1ToolsError);
}

TEST(NativeContext, RejectsSchemaDriftAndSecrets) {
  auto spec = nativeContextSpec();
  spec.mutable_configuration()->mutable_schema()->set_schema_fingerprint(1);
  EXPECT_THROW(NativeContext::FromInstanceSpec(spec), Ros1ToolsError);

  spec = nativeContextSpec();
  auto* secret = spec.add_secrets();
  secret->set_name("forbidden");
  secret->set_reference("secret/ref");
  secret->set_version("1");
  EXPECT_THROW(NativeContext::FromInstanceSpec(spec), Ros1ToolsError);
}

}  // namespace
}  // namespace xgc_ros1_tools_adapter
