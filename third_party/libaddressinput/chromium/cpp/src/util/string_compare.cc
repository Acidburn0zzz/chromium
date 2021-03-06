// Copyright (C) 2014 Google Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "string_compare.h"

#include <libaddressinput/util/scoped_ptr.h>

#include "canonicalize_string.h"

namespace i18n {
namespace addressinput {

bool LooseStringCompare(const std::string& a, const std::string& b) {
  scoped_ptr<StringCanonicalizer> canonicalizer(StringCanonicalizer::Build());
  return canonicalizer->CanonicalizeString(a) ==
         canonicalizer->CanonicalizeString(b);
}

}  // namespace addressinput
}  // namespace i18n
