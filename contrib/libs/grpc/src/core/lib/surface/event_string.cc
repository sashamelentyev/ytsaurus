/*
 *
 * Copyright 2015 gRPC authors.
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
 *
 */

#include <grpc/support/port_platform.h>

#include "src/core/lib/surface/event_string.h"

#include <algorithm>
#include <vector>

#include "y_absl/strings/str_format.h"
#include "y_absl/strings/str_join.h"

static void addhdr(grpc_event* ev, std::vector<TString>* buf) {
  buf->push_back(y_absl::StrFormat("tag:%p", ev->tag));
}

static const char* errstr(int success) { return success ? "OK" : "ERROR"; }

static void adderr(int success, std::vector<TString>* buf) {
  buf->push_back(y_absl::StrFormat(" %s", errstr(success)));
}

TString grpc_event_string(grpc_event* ev) {
  if (ev == nullptr) return "null";
  std::vector<TString> out;
  switch (ev->type) {
    case GRPC_QUEUE_TIMEOUT:
      out.push_back("QUEUE_TIMEOUT");
      break;
    case GRPC_QUEUE_SHUTDOWN:
      out.push_back("QUEUE_SHUTDOWN");
      break;
    case GRPC_OP_COMPLETE:
      out.push_back("OP_COMPLETE: ");
      addhdr(ev, &out);
      adderr(ev->success, &out);
      break;
  }
  return y_absl::StrJoin(out, "");
}
