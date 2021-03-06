// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef UI_DISPLAY_X11_EDID_PARSER_X11_H_
#define UI_DISPLAY_X11_EDID_PARSER_X11_H_

#include <string>

#include "base/basictypes.h"
#include "ui/display/display_constants.h"
#include "ui/display/display_export.h"

typedef unsigned long XID;
typedef XID RROutput;

// Xrandr utility functions to help get EDID information.

namespace ui {

// Gets the EDID data from |output| and generates the display id through
// |GetDisplayIdFromEDID|.
DISPLAY_EXPORT bool GetDisplayId(XID output,
                                 uint8 index,
                                 int64* display_id_out);

// Generate the human readable string from EDID obtained from |output|.
DISPLAY_EXPORT std::string GetDisplayName(RROutput output);

// Gets the overscan flag from |output| and stores to |flag|. Returns true if
// the flag is found. Otherwise returns false and doesn't touch |flag|. The
// output will produce overscan if |flag| is set to true, but the output may
// still produce overscan even though it returns true and |flag| is set to
// false.
DISPLAY_EXPORT bool GetOutputOverscanFlag(RROutput output, bool* flag);

}  // namespace ui

#endif  // UI_DISPLAY_X11_EDID_PARSER_X11_H_
