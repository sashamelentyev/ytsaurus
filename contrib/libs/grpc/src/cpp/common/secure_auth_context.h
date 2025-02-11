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

#ifndef GRPC_INTERNAL_CPP_COMMON_SECURE_AUTH_CONTEXT_H
#define GRPC_INTERNAL_CPP_COMMON_SECURE_AUTH_CONTEXT_H

#include <util/generic/string.h>
#include <util/string/cast.h>
#include <vector>

#include <grpc/grpc_security.h>
#include <grpcpp/security/auth_context.h>
#include <grpcpp/support/config.h>
#include <grpcpp/support/string_ref.h>

#include "src/core/lib/gprpp/ref_counted_ptr.h"
#include "src/core/lib/security/context/security_context.h"

namespace grpc {

class SecureAuthContext final : public AuthContext {
 public:
  explicit SecureAuthContext(grpc_auth_context* ctx)
      : ctx_(ctx != nullptr ? ctx->Ref() : nullptr) {}

  ~SecureAuthContext() override = default;

  bool IsPeerAuthenticated() const override;

  std::vector<grpc::string_ref> GetPeerIdentity() const override;

  TString GetPeerIdentityPropertyName() const override;

  std::vector<grpc::string_ref> FindPropertyValues(
      const TString& name) const override;

  AuthPropertyIterator begin() const override;

  AuthPropertyIterator end() const override;

  void AddProperty(const TString& key,
                   const grpc::string_ref& value) override;

  bool SetPeerIdentityPropertyName(const TString& name) override;

 private:
  grpc_core::RefCountedPtr<grpc_auth_context> ctx_;
};

}  // namespace grpc

#endif  // GRPC_INTERNAL_CPP_COMMON_SECURE_AUTH_CONTEXT_H
