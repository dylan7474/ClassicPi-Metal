/*
 * keymap_usb.c - USB HID keyboard usage codes -> Mac keyboard codes.
 * Mac codes come from umac's keymap.h (MKC_*).
 *
 * Modifiers on a PC keyboard:
 *   Ctrl  -> Command (cloverleaf)   Windows/Super -> Command
 *   Alt   -> Option                 AltGr         -> Option
 */
#include <stdint.h>
#include "keymap.h"

uint8_t usb_to_mac_key(uint8_t u)
{
        static const uint8_t letters[26] = {
                MKC_A, MKC_B, MKC_C, MKC_D, MKC_E, MKC_F, MKC_G, MKC_H, MKC_I,
                MKC_J, MKC_K, MKC_L, MKC_M, MKC_N, MKC_O, MKC_P, MKC_Q, MKC_R,
                MKC_S, MKC_T, MKC_U, MKC_V, MKC_W, MKC_X, MKC_Y, MKC_Z,
        };
        static const uint8_t digits[10] = {
                MKC_1, MKC_2, MKC_3, MKC_4, MKC_5, MKC_6, MKC_7, MKC_8, MKC_9, MKC_0,
        };
        static const uint8_t fkeys[12] = {
                MKC_F1, MKC_F2, MKC_F3, MKC_F4, MKC_F5, MKC_F6,
                MKC_F7, MKC_F8, MKC_F9, MKC_F10, MKC_F11, MKC_F12,
        };
        static const uint8_t keypad[9] = {
                MKC_KP1, MKC_KP2, MKC_KP3, MKC_KP4, MKC_KP5,
                MKC_KP6, MKC_KP7, MKC_KP8, MKC_KP9,
        };

        if (u >= 0x04 && u <= 0x1d) return letters[u - 0x04];
        if (u >= 0x1e && u <= 0x27) return digits[u - 0x1e];
        if (u >= 0x3a && u <= 0x45) return fkeys[u - 0x3a];
        if (u >= 0x59 && u <= 0x61) return keypad[u - 0x59];

        switch (u) {
        case 0x28: return MKC_Return;
        case 0x29: return MKC_Escape;
        case 0x2a: return MKC_BackSpace;
        case 0x2b: return MKC_Tab;
        case 0x2c: return MKC_Space;
        case 0x2d: return MKC_Minus;
        case 0x2e: return MKC_Equal;
        case 0x2f: return MKC_LeftBracket;
        case 0x30: return MKC_RightBracket;
        case 0x31: return MKC_BackSlash;
        case 0x32: return MKC_BackSlash;        /* UK '#~' key, same place */
        case 0x33: return MKC_SemiColon;
        case 0x34: return MKC_SingleQuote;
        case 0x35: return MKC_Grave;
        case 0x36: return MKC_Comma;
        case 0x37: return MKC_Period;
        case 0x38: return MKC_Slash;
        case 0x39: return MKC_CapsLock;
        case 0x46: return MKC_Print;
        case 0x47: return MKC_ScrollLock;
        case 0x48: return MKC_Pause;
        case 0x49: return MKC_Help;             /* Insert */
        case 0x4a: return MKC_Home;
        case 0x4b: return MKC_PageUp;
        case 0x4c: return MKC_ForwardDel;
        case 0x4d: return MKC_End;
        case 0x4e: return MKC_PageDown;
        case 0x4f: return MKC_Right;
        case 0x50: return MKC_Left;
        case 0x51: return MKC_Down;
        case 0x52: return MKC_Up;
        case 0x53: return MKC_Clear;            /* Num Lock */
        case 0x54: return MKC_KPDevide;
        case 0x55: return MKC_KPMultiply;
        case 0x56: return MKC_KPSubtract;
        case 0x57: return MKC_KPAdd;
        case 0x58: return MKC_Enter;
        case 0x62: return MKC_KP0;
        case 0x63: return MKC_Decimal;
        case 0x64: return MKC_AngleBracket;     /* UK '\|' key left of Z */
        case 0x67: return MKC_KPEqual;
        case 0xe0: return MKC_Command;          /* Left Ctrl */
        case 0xe1: return MKC_Shift;
        case 0xe2: return MKC_Option;           /* Left Alt */
        case 0xe3: return MKC_Command;          /* Left GUI */
        case 0xe4: return MKC_Command;          /* Right Ctrl */
        case 0xe5: return MKC_Shift;
        case 0xe6: return MKC_Option;           /* AltGr */
        case 0xe7: return MKC_Command;          /* Right GUI */
        default:   return MKC_None;
        }
}
