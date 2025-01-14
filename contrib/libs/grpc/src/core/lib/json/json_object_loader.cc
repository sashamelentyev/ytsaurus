// Copyright 2020 gRPC authors.
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

#include <grpc/support/port_platform.h>

#include "src/core/lib/json/json_object_loader.h"

#include <utility>

#include "y_absl/strings/ascii.h"
#include "y_absl/strings/str_cat.h"
#include "y_absl/strings/strip.h"

namespace grpc_core {
namespace json_detail {

void LoadScalar::LoadInto(const Json& json, const JsonArgs& /*args*/, void* dst,
                          ValidationErrors* errors) const {
  // We accept either STRING or NUMBER for numeric values, as per
  // https://developers.google.com/protocol-buffers/docs/proto3#json.
  if (json.type() != Json::Type::STRING &&
      (!IsNumber() || json.type() != Json::Type::NUMBER)) {
    errors->AddError(
        y_absl::StrCat("is not a ", IsNumber() ? "number" : "string"));
    return;
  }
  return LoadInto(json.string_value(), dst, errors);
}

bool LoadString::IsNumber() const { return false; }

void LoadString::LoadInto(const TString& value, void* dst,
                          ValidationErrors*) const {
  *static_cast<TString*>(dst) = value;
}

bool LoadDuration::IsNumber() const { return false; }

void LoadDuration::LoadInto(const TString& value, void* dst,
                            ValidationErrors* errors) const {
  y_absl::string_view buf(value);
  if (!y_absl::ConsumeSuffix(&buf, "s")) {
    errors->AddError("Not a duration (no s suffix)");
    return;
  }
  buf = y_absl::StripAsciiWhitespace(buf);
  auto decimal_point = buf.find('.');
  int nanos = 0;
  if (decimal_point != y_absl::string_view::npos) {
    y_absl::string_view after_decimal = buf.substr(decimal_point + 1);
    buf = buf.substr(0, decimal_point);
    if (!y_absl::SimpleAtoi(after_decimal, &nanos)) {
      errors->AddError("Not a duration (not a number of nanoseconds)");
      return;
    }
    if (after_decimal.length() > 9) {
      // We don't accept greater precision than nanos.
      errors->AddError("Not a duration (too many digits after decimal)");
      return;
    }
    for (size_t i = 0; i < (9 - after_decimal.length()); ++i) {
      nanos *= 10;
    }
  }
  int seconds;
  if (!y_absl::SimpleAtoi(buf, &seconds)) {
    errors->AddError("Not a duration (not a number of seconds)");
    return;
  }
  *static_cast<Duration*>(dst) =
      Duration::FromSecondsAndNanoseconds(seconds, nanos);
}

bool LoadNumber::IsNumber() const { return true; }

void LoadBool::LoadInto(const Json& json, const JsonArgs&, void* dst,
                        ValidationErrors* errors) const {
  if (json.type() == Json::Type::JSON_TRUE) {
    *static_cast<bool*>(dst) = true;
  } else if (json.type() == Json::Type::JSON_FALSE) {
    *static_cast<bool*>(dst) = false;
  } else {
    errors->AddError("is not a boolean");
  }
}

void LoadUnprocessedJsonObject::LoadInto(const Json& json, const JsonArgs&,
                                         void* dst,
                                         ValidationErrors* errors) const {
  if (json.type() != Json::Type::OBJECT) {
    errors->AddError("is not an object");
    return;
  }
  *static_cast<Json::Object*>(dst) = json.object_value();
}

void LoadVector::LoadInto(const Json& json, const JsonArgs& args, void* dst,
                          ValidationErrors* errors) const {
  if (json.type() != Json::Type::ARRAY) {
    errors->AddError("is not an array");
    return;
  }
  const auto& array = json.array_value();
  const LoaderInterface* element_loader = ElementLoader();
  for (size_t i = 0; i < array.size(); ++i) {
    ValidationErrors::ScopedField field(errors, y_absl::StrCat("[", i, "]"));
    void* element = EmplaceBack(dst);
    element_loader->LoadInto(array[i], args, element, errors);
  }
}

void AutoLoader<std::vector<bool>>::LoadInto(const Json& json,
                                             const JsonArgs& args, void* dst,
                                             ValidationErrors* errors) const {
  if (json.type() != Json::Type::ARRAY) {
    errors->AddError("is not an array");
    return;
  }
  const auto& array = json.array_value();
  const LoaderInterface* element_loader = LoaderForType<bool>();
  std::vector<bool>* vec = static_cast<std::vector<bool>*>(dst);
  for (size_t i = 0; i < array.size(); ++i) {
    ValidationErrors::ScopedField field(errors, y_absl::StrCat("[", i, "]"));
    bool elem = false;
    element_loader->LoadInto(array[i], args, &elem, errors);
    vec->push_back(elem);
  }
}

void LoadMap::LoadInto(const Json& json, const JsonArgs& args, void* dst,
                       ValidationErrors* errors) const {
  if (json.type() != Json::Type::OBJECT) {
    errors->AddError("is not an object");
    return;
  }
  const LoaderInterface* element_loader = ElementLoader();
  for (const auto& pair : json.object_value()) {
    ValidationErrors::ScopedField field(errors,
                                        y_absl::StrCat("[\"", pair.first, "\"]"));
    void* element = Insert(pair.first, dst);
    element_loader->LoadInto(pair.second, args, element, errors);
  }
}

void LoadOptional::LoadInto(const Json& json, const JsonArgs& args, void* dst,
                            ValidationErrors* errors) const {
  if (json.type() == Json::Type::JSON_NULL) return;
  void* element = Emplace(dst);
  size_t starting_error_size = errors->size();
  ElementLoader()->LoadInto(json, args, element, errors);
  if (errors->size() > starting_error_size) Reset(dst);
}

bool LoadObject(const Json& json, const JsonArgs& args, const Element* elements,
                size_t num_elements, void* dst, ValidationErrors* errors) {
  if (json.type() != Json::Type::OBJECT) {
    errors->AddError("is not an object");
    return false;
  }
  for (size_t i = 0; i < num_elements; ++i) {
    const Element& element = elements[i];
    if (element.enable_key != nullptr && !args.IsEnabled(element.enable_key)) {
      continue;
    }
    ValidationErrors::ScopedField field(errors,
                                        y_absl::StrCat(".", element.name));
    const auto& it = json.object_value().find(element.name);
    if (it == json.object_value().end()) {
      if (element.optional) continue;
      errors->AddError("field not present");
      continue;
    }
    char* field_dst = static_cast<char*>(dst) + element.member_offset;
    element.loader->LoadInto(it->second, args, field_dst, errors);
  }
  return true;
}

const Json* GetJsonObjectField(const Json::Object& json,
                               y_absl::string_view field,
                               ValidationErrors* errors, bool required) {
  auto it = json.find(TString(field));
  if (it == json.end()) {
    if (required) errors->AddError("field not present");
    return nullptr;
  }
  return &it->second;
}

}  // namespace json_detail
}  // namespace grpc_core
