// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef DEVICE_BLUETOOTH_BLUETOOTH_UTILS_H_
#define DEVICE_BLUETOOTH_BLUETOOTH_UTILS_H_

#include <string>

#include "base/basictypes.h"

namespace device {
namespace bluetooth_utils {

// Opaque wrapper around a Bluetooth UUID. Instances of UUID represent the
// 128-bit universally unique identifiers (UUIDs) of profiles and attributes
// used in Bluetooth based communication, such as a peripheral's services,
// characteristics, and characteristic descriptors. An instance are
// constructed using a string representing 16, 32, or 128 bit UUID formats.
class UUID {
 public:
  // Possible representation formats used during construction.
  enum Format {
    kFormatInvalid,
    kFormat16Bit,
    kFormat32Bit,
    kFormat128Bit
  };

  // Single argument constructor. |uuid| can be a 16, 32, or 128 bit UUID
  // represented as a 4, 8, or 36 character string with the following
  // formats:
  //   XXXX
  //   0xXXXX
  //   XXXXXXXX
  //   0xXXXXXXXX
  //   XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX
  //
  // 16 and 32 bit UUIDs will be internally converted to a 128 bit UUID using
  // the base UUID defined in the Bluetooth specification, hence custom UUIDs
  // should be provided in the 128-bit format. If |uuid| is in an unsupported
  // format, the result might be invalid. Use IsValid to check for validity
  // after construction.
  explicit UUID(const std::string& uuid);
  ~UUID();

  // Returns true, if the UUID is in a valid canonical format.
  bool IsValid() const;

  // Returns the representation format of the UUID. This reflects the format
  // that was provided during construction.
  Format format() const { return format_; }

  // Returns the value of the UUID as a string. The representation format is
  // based on what was passed in during construction. For the supported sizes,
  // this representation can have the following formats:
  //   - 16 bit:  XXXX
  //   - 32 bit:  XXXXXXXX
  //   - 128 bit: XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX
  // where X is a lowercase hex digit.
  const std::string& value() const { return value_; }

  // Returns the underlying 128-bit value as a string in the following format:
  //   XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX
  // where X is a lowercase hex digit.
  const std::string& canonical_value() const { return canonical_value_; }

  // Permit sufficient comparison to allow a UUID to be used as a key in a
  // std::map.
  bool operator<(const UUID& uuid) const;

  // Equality operators.
  bool operator==(const UUID& uuid) const;
  bool operator!=(const UUID& uuid) const;

 private:
  UUID();

  // String representation of the UUID that was used during construction. For
  // the supported sizes, this representation can have the following formats:
  //   - 16 bit:  XXXX
  //   - 32 bit:  XXXXXXXX
  //   - 128 bit: XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX
  Format format_;
  std::string value_;

  // The 128-bit string representation of the UUID.
  std::string canonical_value_;
};

// DEPRECATED. Use bluetooth_utils::UUID instead.
//
// Takes a 4, 8 or 36 character UUID, validates it and returns it in 36
// character format with all hex digits lower case.  If |uuid| is invalid, the
// empty string is returned.
//
// Valid inputs are:
//   XXXX
//   0xXXXX
//   XXXXXXXX
//   0xXXXXXXXX
//   XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX
std::string CanonicalUuid(std::string uuid);

}  // namespace bluetooth_utils
}  // namespace device

#endif  // DEVICE_BLUETOOTH_BLUETOOTH_UTILS_H_
