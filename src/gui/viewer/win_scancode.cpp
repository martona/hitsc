#include "gui/viewer/win_scancode.hpp"

#ifdef _WIN32

namespace hitsc {

KvmScancode kvm_scancode_from_windows(unsigned scancode, bool extended)
{
    if (extended) {
        switch (scancode) {
        case 0x1C: return KvmScancode::KP_ENTER;
        case 0x1D: return KvmScancode::RCTRL;
        case 0x35: return KvmScancode::KP_DIVIDE;
        case 0x37: return KvmScancode::PRINTSCREEN;
        case 0x38: return KvmScancode::RALT;
        case 0x47: return KvmScancode::HOME;
        case 0x48: return KvmScancode::UP;
        case 0x49: return KvmScancode::PAGEUP;
        case 0x4B: return KvmScancode::LEFT;
        case 0x4D: return KvmScancode::RIGHT;
        case 0x4F: return KvmScancode::END;
        case 0x50: return KvmScancode::DOWN;
        case 0x51: return KvmScancode::PAGEDOWN;
        case 0x52: return KvmScancode::INSERT;
        case 0x53: return KvmScancode::DELETE_KEY;
        case 0x5B: return KvmScancode::LGUI;
        case 0x5C: return KvmScancode::RGUI;
        case 0x5D: return KvmScancode::APPLICATION;
        default: return KvmScancode::UNKNOWN;
        }
    }

    switch (scancode) {
    case 0x01: return KvmScancode::ESCAPE;
    case 0x02: return KvmScancode::DIGIT_1;
    case 0x03: return KvmScancode::DIGIT_2;
    case 0x04: return KvmScancode::DIGIT_3;
    case 0x05: return KvmScancode::DIGIT_4;
    case 0x06: return KvmScancode::DIGIT_5;
    case 0x07: return KvmScancode::DIGIT_6;
    case 0x08: return KvmScancode::DIGIT_7;
    case 0x09: return KvmScancode::DIGIT_8;
    case 0x0A: return KvmScancode::DIGIT_9;
    case 0x0B: return KvmScancode::DIGIT_0;
    case 0x0C: return KvmScancode::MINUS;
    case 0x0D: return KvmScancode::EQUALS;
    case 0x0E: return KvmScancode::BACKSPACE;
    case 0x0F: return KvmScancode::TAB;
    case 0x10: return KvmScancode::Q;
    case 0x11: return KvmScancode::W;
    case 0x12: return KvmScancode::E;
    case 0x13: return KvmScancode::R;
    case 0x14: return KvmScancode::T;
    case 0x15: return KvmScancode::Y;
    case 0x16: return KvmScancode::U;
    case 0x17: return KvmScancode::I;
    case 0x18: return KvmScancode::O;
    case 0x19: return KvmScancode::P;
    case 0x1A: return KvmScancode::LEFTBRACKET;
    case 0x1B: return KvmScancode::RIGHTBRACKET;
    case 0x1C: return KvmScancode::RETURN;
    case 0x1D: return KvmScancode::LCTRL;
    case 0x1E: return KvmScancode::A;
    case 0x1F: return KvmScancode::S;
    case 0x20: return KvmScancode::D;
    case 0x21: return KvmScancode::F;
    case 0x22: return KvmScancode::G;
    case 0x23: return KvmScancode::H;
    case 0x24: return KvmScancode::J;
    case 0x25: return KvmScancode::K;
    case 0x26: return KvmScancode::L;
    case 0x27: return KvmScancode::SEMICOLON;
    case 0x28: return KvmScancode::APOSTROPHE;
    case 0x29: return KvmScancode::GRAVE;
    case 0x2A: return KvmScancode::LSHIFT;
    case 0x2B: return KvmScancode::BACKSLASH;
    case 0x2C: return KvmScancode::Z;
    case 0x2D: return KvmScancode::X;
    case 0x2E: return KvmScancode::C;
    case 0x2F: return KvmScancode::V;
    case 0x30: return KvmScancode::B;
    case 0x31: return KvmScancode::N;
    case 0x32: return KvmScancode::M;
    case 0x33: return KvmScancode::COMMA;
    case 0x34: return KvmScancode::PERIOD;
    case 0x35: return KvmScancode::SLASH;
    case 0x36: return KvmScancode::RSHIFT;
    case 0x37: return KvmScancode::KP_MULTIPLY;
    case 0x38: return KvmScancode::LALT;
    case 0x39: return KvmScancode::SPACE;
    case 0x3A: return KvmScancode::CAPSLOCK;
    case 0x3B: return KvmScancode::F1;
    case 0x3C: return KvmScancode::F2;
    case 0x3D: return KvmScancode::F3;
    case 0x3E: return KvmScancode::F4;
    case 0x3F: return KvmScancode::F5;
    case 0x40: return KvmScancode::F6;
    case 0x41: return KvmScancode::F7;
    case 0x42: return KvmScancode::F8;
    case 0x43: return KvmScancode::F9;
    case 0x44: return KvmScancode::F10;
    case 0x45: return KvmScancode::NUMLOCKCLEAR;
    case 0x46: return KvmScancode::SCROLLLOCK;
    case 0x47: return KvmScancode::KP_7;
    case 0x48: return KvmScancode::KP_8;
    case 0x49: return KvmScancode::KP_9;
    case 0x4A: return KvmScancode::KP_MINUS;
    case 0x4B: return KvmScancode::KP_4;
    case 0x4C: return KvmScancode::KP_5;
    case 0x4D: return KvmScancode::KP_6;
    case 0x4E: return KvmScancode::KP_PLUS;
    case 0x4F: return KvmScancode::KP_1;
    case 0x50: return KvmScancode::KP_2;
    case 0x51: return KvmScancode::KP_3;
    case 0x52: return KvmScancode::KP_0;
    case 0x53: return KvmScancode::KP_PERIOD;
    case 0x56: return KvmScancode::NONUSBACKSLASH;
    case 0x57: return KvmScancode::F11;
    case 0x58: return KvmScancode::F12;
    default: return KvmScancode::UNKNOWN;
    }
}

} // namespace hitsc

#endif // _WIN32
