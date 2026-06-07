#pragma once

#include "view_input_types.hpp"  // KvmScancode

#include <QString>

#include <memory>
#include <vector>

namespace hitsc {

// One key press in a typing plan: the modifier keys to hold (HID scancodes) plus the
// character key. Phase 2 expands each chord into down/up KvmKeyEvents on the wire
// (mods down, key down, key up, mods up).
struct KeyChord {
    std::vector<KvmScancode> modifiers;
    KvmScancode key = KvmScancode::UNKNOWN;
};

// The first character that couldn't be typed in the active layout. line/column are
// 1-based and counted in Unicode scalars; column resets after each newline.
struct TypeError {
    char32_t codepoint = 0;
    int line = 1;
    int column = 1;
};

struct TypePlan {
    std::vector<KeyChord> chords;
    int character_count = 0;  // typable characters consumed (incl. Enter/Tab)
    int key_event_count = 0;  // down+up events Phase 2 would put on the wire
};

// Result of translate(): a plan, or the first untypable character.
struct TypeResult {
    bool ok = false;
    TypePlan plan;
    TypeError error;
};

// A keyboard layout that turns text into key chords. The Windows implementation wraps
// a selected HKL (VkKeyScanEx); a libxkbcommon implementation can slot in behind this
// same interface when the app is ported. translate() is pure (no global state touched).
class KeyboardLayout {
public:
    virtual ~KeyboardLayout() = default;
    virtual QString display_name() const = 0;
    virtual TypeResult translate(const QString& text) const = 0;
};

// One installed layout, for the chevron menu.
struct LayoutInfo {
    QString klid;          // 8-hex-digit id; the stable per-host persist key
    QString display_name;  // e.g. "United States-International"
    bool is_ime = false;   // an IME (Pinyin, etc.): no 1:1 char map -> unusable here
};

// Keyboard layouts installed on this client (Windows: GetKeyboardLayoutList).
std::vector<LayoutInfo> enumerate_keyboard_layouts();

// KLID of the client's currently active layout -- the sensible default.
QString active_keyboard_layout_klid();

// Build an engine for a KLID (must be installed on the client). Null on failure.
std::unique_ptr<KeyboardLayout> make_keyboard_layout(const QString& klid);

} // namespace hitsc
