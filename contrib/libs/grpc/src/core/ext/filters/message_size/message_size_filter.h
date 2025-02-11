//
// Copyright 2016 gRPC authors.
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

#ifndef GRPC_CORE_EXT_FILTERS_MESSAGE_SIZE_MESSAGE_SIZE_FILTER_H
#define GRPC_CORE_EXT_FILTERS_MESSAGE_SIZE_MESSAGE_SIZE_FILTER_H

#include <grpc/support/port_platform.h>

#include <stddef.h>

#include <memory>

#include "y_absl/status/statusor.h"
#include "y_absl/strings/string_view.h"

#include "src/core/lib/channel/channel_args.h"
#include "src/core/lib/channel/channel_fwd.h"
#include "src/core/lib/channel/channel_stack.h"
#include "src/core/lib/channel/context.h"
#include "src/core/lib/config/core_configuration.h"
#include "src/core/lib/json/json.h"
#include "src/core/lib/service_config/service_config_parser.h"

extern const grpc_channel_filter grpc_message_size_filter;

namespace grpc_core {

class MessageSizeParsedConfig : public ServiceConfigParser::ParsedConfig {
 public:
  struct message_size_limits {
    int max_send_size;
    int max_recv_size;
  };

  MessageSizeParsedConfig(int max_send_size, int max_recv_size) {
    limits_.max_send_size = max_send_size;
    limits_.max_recv_size = max_recv_size;
  }

  const message_size_limits& limits() const { return limits_; }

  static const MessageSizeParsedConfig* GetFromCallContext(
      const grpc_call_context_element* context,
      size_t service_config_parser_index);

 private:
  message_size_limits limits_;
};

class MessageSizeParser : public ServiceConfigParser::Parser {
 public:
  y_absl::string_view name() const override { return parser_name(); }

  y_absl::StatusOr<std::unique_ptr<ServiceConfigParser::ParsedConfig>>
  ParsePerMethodParams(const ChannelArgs& /*args*/, const Json& json) override;

  static void Register(CoreConfiguration::Builder* builder);

  static size_t ParserIndex();

 private:
  static y_absl::string_view parser_name() { return "message_size"; }
};

int GetMaxRecvSizeFromChannelArgs(const ChannelArgs& args);
int GetMaxSendSizeFromChannelArgs(const ChannelArgs& args);

}  // namespace grpc_core

#endif /* GRPC_CORE_EXT_FILTERS_MESSAGE_SIZE_MESSAGE_SIZE_FILTER_H */
