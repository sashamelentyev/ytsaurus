//
// Copyright 2022 gRPC authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#ifndef GRPC_CORE_EXT_XDS_XDS_LB_POLICY_REGISTRY_H
#define GRPC_CORE_EXT_XDS_XDS_LB_POLICY_REGISTRY_H

#include <grpc/support/port_platform.h>

#include <map>
#include <memory>

#include "y_absl/status/statusor.h"
#include "y_absl/strings/string_view.h"
#include "envoy/config/cluster/v3/cluster.upb.h"

#include "src/core/ext/xds/xds_resource_type.h"
#include "src/core/lib/json/json.h"

namespace grpc_core {

// A registry that maintans a set of converters that are able to map xDS
// loadbalancing policy configurations to gRPC's JSON format.
class XdsLbPolicyRegistry {
 public:
  class ConfigFactory {
   public:
    virtual ~ConfigFactory() {}
    virtual y_absl::StatusOr<Json::Object> ConvertXdsLbPolicyConfig(
        const XdsResourceType::DecodeContext& context,
        y_absl::string_view configuration, int recursion_depth) = 0;

    virtual y_absl::string_view type() = 0;
  };

  // Converts an xDS cluster load balancing policy message to gRPC's JSON
  // format. An error is returned if none of the lb policies in the list are
  // supported, or if a supported lb policy configuration conversion fails. \a
  // recursion_depth indicates the current depth of the tree if lb_policy
  // configuration recursively holds other lb policies.
  static y_absl::StatusOr<Json::Array> ConvertXdsLbPolicyConfig(
      const XdsResourceType::DecodeContext& context,
      const envoy_config_cluster_v3_LoadBalancingPolicy* lb_policy,
      int recursion_depth = 0);

 private:
  XdsLbPolicyRegistry();

  static XdsLbPolicyRegistry* Get();

  // A map of config factories that goes from the type of the lb policy config
  // to the config factory.
  std::map<y_absl::string_view /* Owned by ConfigFactory */,
           std::unique_ptr<ConfigFactory>>
      policy_config_factories_;
};

}  // namespace grpc_core

#endif  // GRPC_CORE_EXT_XDS_XDS_LB_POLICY_REGISTRY_H
