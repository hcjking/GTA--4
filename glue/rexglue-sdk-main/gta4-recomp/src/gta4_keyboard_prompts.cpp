// Keyboard prompts: the help text's button glyphs drawn as the PC keys.
//
// The Xbox 360 text names actions with ~INPUT_<action>~ tokens (and buttons
// with ~PAD_A~ style ones); sub_821F2360 turns a token into a glyph code
// (256-296 the button pictures, 255 none), asking sub_822B7C00 which pad
// button an action is bound to. The font draws a glyph with the texture of
// that button's name in buttons_360.xtd. The keyboard set
// (tools/xtd_tools/build_keyboard_prompts.py) puts a PC key in each of those
// pictures, so for an action token this returns the glyph whose picture is
// the key the PC binds the action to (gta4_input_hooks.cpp kButtonBindings,
// GTA IV PC's defaults). Raw PAD_ tokens keep their glyph; an action whose
// key has no picture shows none rather than a wrong one.

#include <cstdint>

#include <rex/ppc/context.h>

#include "rpf_button_prompts.h"

namespace gta4::keyboard_prompts {
namespace {

// The glyph of each picture in the keyboard set (sub_821F2360's codes:
// 256-259 PAD_UP ... PAD_RIGHT, 260-267 PAD_DPAD_UP, DOWN, LEFT, RIGHT,
// NONE, ALL, UPDOWN, LEFTRIGHT, 268-275 PAD_LSTICK_UP, DOWN, LEFT, RIGHT,
// NONE, ALL, UPDOWN, LEFTRIGHT, 276-283 the same for the right stick,
// 284-293 PAD_A, B, X, Y, LB, LT, RB, RT, START, BACK), as
// build_keyboard_prompts.py KEYS lays the keys out.
enum Glyph : uint32_t {
  kNone = 255,
  kArrowUp = 260, kArrowDown = 261, kArrowLeft = 262, kArrowRight = 263,  // dpad_up ... right
  kShift = 264, kW = 265, kS = 266, kG = 267,            // dpad_none / all / updown / leftright
  kWasd = 268,                                           // lstick_* (WASD)
  kCtrl = 272,                                           // lstick_none
  kMouse = 276,                                          // rstick_* (the mouse)
  kE = 280,                                              // rstick_none
  kEnter = 284, kEsc = 285, kR = 286, kSpace = 287,      // a / b / x / y
  kQ = 288, kRmb = 289, kH = 290, kLmb = 291,            // lb / lt / rb / rt
  kF = 293,                                              // back (start: Esc too)
};

// The key glyph for each action (gta4_input_hooks.cpp Action numbering).
uint32_t KeyFor(uint32_t action) {
  switch (action) {
    case 1: return kShift;                 // sprint
    case 2: return kSpace;                 // jump
    case 3: return kF;                     // enter a vehicle
    case 4: return kLmb;                   // attack
    case 6: return kRmb;                   // aim
    case 12: case 13: case 14: case 15: return kWasd;    // move
    case 16: case 17: case 18: case 19: return kMouse;   // look
    case 20: return kCtrl;                 // duck
    case 21: return kArrowUp;              // phone out
    case 22: return kEsc;                  // phone away
    case 23: return kE;                    // pick up
    case 28: return kQ;                    // cover
    case 29: return kR;                    // reload
    case 30: case 31: case 32: case 33: return kWasd;    // steer
    case 34: case 35: case 36: case 37: return kMouse;   // vehicle gun
    case 38: return kLmb;                  // vehicle attack
    case 39: return kRmb;
    case 40: return kW;                    // accelerate
    case 41: return kS;                    // brake / reverse
    case 42: return kH;                    // headlights
    case 43: return kF;                    // leave the vehicle
    case 44: return kSpace;                // handbrake
    case 46: return kW;                    // hotwire
    case 47: return kS;
    case 48: case 49: return kMouse;       // vehicle look
    case 54: return kG;                    // horn
    case 55: return kW;                    // throttle up
    case 56: return kS;                    // throttle down
    case 59: return kLmb;                  // melee
    case 60: return kR;
    case 62: return kQ;                    // kick
    case 63: return kSpace;                // block
    case 64: return kArrowDown;            // menus
    case 65: return kArrowUp;
    case 66: return kArrowLeft;
    case 67: return kArrowRight;
    case 76: return kEsc;                  // pause
    case 77: return kLmb;                  // accept
    case 78: return kEsc;                  // cancel
    case 79: return kR;
    case 80: return kSpace;
    case 81: return kQ;
    case 82: return kNone;                 // (right shoulder: no PC binding)
    case 83: return kRmb;
    case 84: return kLmb;
    default: return kNone;                 // (C, V, Caps Lock, the wheel...: no picture)
  }
}

// The action sub_822B7C00 was last asked about from inside sub_821F2360
// (the token being resolved), this thread.
thread_local uint32_t t_token_action = UINT32_MAX;

}  // namespace

void BeforeGlyph() { t_token_action = UINT32_MAX; }

void NoteBinding(uint32_t action, uint32_t caller) {
  if (caller == 0x821F2CEC && action < 86) t_token_action = action;
}

void AfterGlyph(PPCContext& context, uint8_t* base) {
  (void)base;
  const uint32_t glyph = context.r3.u32;
  if (!gta4::button_prompts::KeyboardPromptsMounted() || glyph < 256 || glyph > 296 ||
      t_token_action == UINT32_MAX) {
    return;
  }
  context.r3.u64 = KeyFor(t_token_action);
}

}  // namespace gta4::keyboard_prompts
