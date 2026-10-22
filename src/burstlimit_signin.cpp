// Xbox LIVE sign-in follows the player's controller (issue #20).
//
// The emulated profile is signed in on one guest user index (the SDK's
// UserProfile::signed_in_user, user 0 by default). The game keeps the
// controller of each player slot at 0x8424DB58 + 28 + slot * 28 (sub_82280298
// reads it, sub_82280260(slot, pad) sets it): pressing Start at the title makes
// that pad player 1 (slot 0), and the title, the Xbox LIVE menus and saving all
// ask XamUserGetSigninState about that pad. When the player's controller is
// not user 0 - a virtual pad from Steam Input / DS4Windows or a keyboard device
// took user 0, or Start was pressed on a second pad - the game said "You are not
// signed in" and greyed out Xbox LIVE Battle.
//
// So the profile moves to whichever pad becomes player 1. Still one signed-in
// user with the same XUID and gamertag (online_name), so saves, the lobby and
// the netcode see the same identity; the other pads stay signed out like
// before (player 2 in local Versus doesn't need a profile).

#include <cstdint>

#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/user_profile.h>

// sub_82280260 entry: r3 = player slot, r4 = controller (guest user index).
void BurstLimitSigninPad(PPCRegister& r3, PPCRegister& r4) {
  const int32_t slot = r3.s32;
  const int32_t pad = r4.s32;
  if (slot != 0 || pad < 0 || pad > 3) {
    return;
  }
  rex::system::KernelState* kernel = rex::system::kernel_state();
  if (!kernel || !kernel->user_profile()) {
    return;
  }
  kernel->user_profile()->set_signed_in_user(uint32_t(pad));
}
