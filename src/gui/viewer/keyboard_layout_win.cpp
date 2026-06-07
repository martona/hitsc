#include "gui/viewer/keyboard_layout.hpp"

#ifdef _WIN32

#include "gui/viewer/win_scancode.hpp"
#include "gui/windows_registry.hpp"  // open_key / read_string_value (+ <windows.h>)

#include <string>
#include <utility>

#include <QList>

namespace hitsc {
namespace {

// HKL -> 8-hex KLID. There is no direct API, so we briefly make the layout active on
// THIS thread, read its canonical name, and restore the previous one immediately. Done
// only at enumeration (chevron open); the swap is synchronous and restored before we
// return, so the client's typing is undisturbed.
QString klid_for_hkl(HKL hkl)
{
    if (hkl == nullptr) {
        return {};
    }
    const HKL previous = GetKeyboardLayout(0);
    ActivateKeyboardLayout(hkl, 0);
    wchar_t name[KL_NAMELENGTH] = {};
    const bool ok = GetKeyboardLayoutNameW(name) != FALSE;
    if (previous != nullptr) {
        ActivateKeyboardLayout(previous, 0);
    }
    return ok ? QString::fromWCharArray(name) : QString();
}

// Friendly name from HKLM\...\Keyboard Layouts\<KLID>\Layout Text, falling back to the
// KLID itself. ("Layout Display Name" would be localized but needs an MUI resolve via
// SHLoadIndirectString -- a later nicety, kept out to avoid the Shlwapi dependency.)
QString display_name_for_klid(const QString& klid)
{
    if (klid.isEmpty()) {
        return klid;
    }
    const QString path =
        QStringLiteral("SYSTEM\\CurrentControlSet\\Control\\Keyboard Layouts\\") + klid;
    if (const auto key = open_key(HKEY_LOCAL_MACHINE, path, KEY_READ)) {
        if (const auto text = read_string_value(key->get(), L"Layout Text")) {
            if (!text->isEmpty()) {
                return *text;
            }
        }
    }
    return klid;
}

// VkKeyScanEx shift-state bits -> modifier scancodes. AltGr (Ctrl+Alt) collapses to a
// single Right-Alt, which is how a guest layout interprets AltGr over HID.
void append_modifiers(int shift_state, std::vector<KvmScancode>& mods)
{
    const bool need_shift = (shift_state & 1) != 0;
    const bool need_ctrl = (shift_state & 2) != 0;
    const bool need_alt = (shift_state & 4) != 0;
    if (need_ctrl && need_alt) {
        mods.push_back(KvmScancode::RALT);  // AltGr
    } else {
        if (need_ctrl) {
            mods.push_back(KvmScancode::LCTRL);
        }
        if (need_alt) {
            mods.push_back(KvmScancode::LALT);
        }
    }
    if (need_shift) {
        mods.push_back(KvmScancode::LSHIFT);
    }
}

class WindowsKeyboardLayout : public KeyboardLayout {
public:
    WindowsKeyboardLayout(HKL hkl, QString display_name)
        : hkl_(hkl)
        , display_name_(std::move(display_name))
    {
    }

    QString display_name() const override { return display_name_; }

    TypeResult translate(const QString& text) const override
    {
        TypeResult result;
        TypePlan& plan = result.plan;

        const auto scalars = text.toUcs4();  // QList<uint>, one entry per code point
        int line = 1;
        int column = 0;
        for (qsizetype i = 0; i < scalars.size(); ++i) {
            const char32_t cp = static_cast<char32_t>(scalars[i]);
            ++column;

            // Newlines -> Enter (layout-independent). Swallow the CR of a CRLF pair so
            // "\r\n" yields a single Enter.
            if (cp == U'\r') {
                if (i + 1 < scalars.size() && scalars[i + 1] == U'\n') {
                    --column;  // the CR isn't its own column; the LF lands next
                    continue;
                }
                append_simple(plan, KvmScancode::RETURN);
                line += 1;
                column = 0;
                continue;
            }
            if (cp == U'\n') {
                append_simple(plan, KvmScancode::RETURN);
                line += 1;
                column = 0;
                continue;
            }
            if (cp == U'\t') {
                append_simple(plan, KvmScancode::TAB);
                continue;
            }

            // VkKeyScanExW takes a single UTF-16 unit, so anything outside the BMP is
            // unreachable as a keystroke.
            if (cp > 0xFFFF) {
                result.error = TypeError{cp, line, column};
                return result;
            }
            const SHORT vks = VkKeyScanExW(static_cast<WCHAR>(cp), hkl_);
            if (vks == -1) {
                result.error = TypeError{cp, line, column};
                return result;
            }
            const int vk = LOBYTE(vks);
            const int shift_state = HIBYTE(vks);
            if (vk == 0) {
                result.error = TypeError{cp, line, column};
                return result;
            }
            const UINT vsc = MapVirtualKeyExW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC, hkl_);
            if (vsc == 0) {
                result.error = TypeError{cp, line, column};
                return result;
            }
            const KvmScancode key = kvm_scancode_from_windows(vsc, /*extended=*/false);
            if (key == KvmScancode::UNKNOWN) {
                result.error = TypeError{cp, line, column};
                return result;
            }

            KeyChord chord;
            append_modifiers(shift_state, chord.modifiers);
            chord.key = key;
            plan.chords.push_back(std::move(chord));
            plan.character_count += 1;
        }

        for (const KeyChord& chord : plan.chords) {
            plan.key_event_count += 2 * (static_cast<int>(chord.modifiers.size()) + 1);
        }
        result.ok = true;
        return result;
    }

private:
    static void append_simple(TypePlan& plan, KvmScancode key)
    {
        KeyChord chord;
        chord.key = key;
        plan.chords.push_back(std::move(chord));
        plan.character_count += 1;
    }

    HKL hkl_ = nullptr;
    QString display_name_;
};

} // namespace

std::vector<LayoutInfo> enumerate_keyboard_layouts()
{
    const int count = GetKeyboardLayoutList(0, nullptr);
    if (count <= 0) {
        return {};
    }
    std::vector<HKL> hkls(static_cast<std::size_t>(count), nullptr);
    GetKeyboardLayoutList(count, hkls.data());

    std::vector<LayoutInfo> out;
    out.reserve(hkls.size());
    for (HKL hkl : hkls) {
        if (hkl == nullptr) {
            continue;
        }
        LayoutInfo info;
        // A real IME (Pinyin, etc.) is identified by its HKL device handle living in the
        // 0xExxx range; ordinary layouts (US = 0x0409xxxx, alternates like Dvorak = 0xFxxx)
        // are not. We do NOT use ImmIsIME(): on TSF-era Windows it reports true even for
        // plain layouts (IMM sits over the Text Services Framework, where every input
        // processor looks IME-ish), which mislabels US.
        const auto raw = static_cast<unsigned long>(reinterpret_cast<ULONG_PTR>(hkl));
        info.is_ime = (raw & 0xF0000000UL) == 0xE0000000UL;
        info.klid = klid_for_hkl(hkl);
        info.display_name = display_name_for_klid(info.klid);
        out.push_back(std::move(info));
    }
    return out;
}

QString active_keyboard_layout_klid()
{
    return klid_for_hkl(GetKeyboardLayout(0));
}

std::unique_ptr<KeyboardLayout> make_keyboard_layout(const QString& klid)
{
    if (klid.isEmpty()) {
        return nullptr;
    }
    // Load (without activating) the layout for the given KLID. It must be installed on
    // the client; if it isn't, LoadKeyboardLayout fails and we report no engine.
    const std::wstring wide = klid.toStdWString();
    const HKL hkl = LoadKeyboardLayoutW(wide.c_str(), KLF_NOTELLSHELL);
    if (hkl == nullptr) {
        return nullptr;
    }
    return std::make_unique<WindowsKeyboardLayout>(hkl, display_name_for_klid(klid));
}

} // namespace hitsc

#else  // !_WIN32

namespace hitsc {

std::vector<LayoutInfo> enumerate_keyboard_layouts()
{
    return {};
}

QString active_keyboard_layout_klid()
{
    return {};
}

std::unique_ptr<KeyboardLayout> make_keyboard_layout(const QString&)
{
    return nullptr;
}

} // namespace hitsc

#endif // _WIN32
