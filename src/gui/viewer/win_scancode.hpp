#pragma once

#ifdef _WIN32

#include "view_input_types.hpp"

namespace hitsc {

// Map a Windows PS/2 set-1 scan code (plus the extended-key flag) to a KvmScancode
// (a USB HID usage code). The flag distinguishes e.g. the arrows from the numpad and
// the right modifiers from the left. Shared by the live keyboard filter
// (viewer_window) and the clipboard-typing engine (keyboard_layout_win): the latter
// gets its scan code from MapVirtualKeyEx, which never sets the extended flag for the
// character-producing keys it deals with.
KvmScancode kvm_scancode_from_windows(unsigned scancode, bool extended);

} // namespace hitsc

#endif // _WIN32
