# Android → Xbox input bridge

Loopback UDP127.0.0.1:38762, opt-in ME_GUEST_INPUT. PreviewActivity sends32-byte MEI1 packets every40ms while resumed. Fields: BE magic/sequence/connected/buttons,4 signed BE16 stick axes,LT/RT bytes,reserved6zero bytes. User0 only,AnyUser bit30/FF maps0. Read timeout600ms disconnects/releases input.

Guest XamInputGetState writes big-endian16-byte X_INPUT_STATE only for assigned live source. NULL state supports query; absent source preserves output and returns1167. Capabilities20bytes advertises gamepad without vibration; hardware rumble is not implemented. Button changes queue up to32 transitions so short START presses survive low frame polling; caps queries do not consume transitions.

PreviewActivity forwards gamepad buttons/hat/axes/triggers,Enter=START,Space=A,Escape=B. Temporary START/A/B touch bar hides on detected external pad/keyboard/mouse; full touch/KM game controls are not finished. onPause/focus loss/device removal clear held input; packets are sent disconnected onPause and native timeout handles broken transport. MainActivity existing input diagnostics remain separate.

Initial compile and APK install-r succeeded, native parser/absent-device tests646passed. First Android short START event not observed in game; added transition queue. Connected controller run pending. No menu transition/profile/save support claimed. Profile remains absent, so subsequent Xbox user UI may be the next blocker. No app data cleared or saves deleted.


## Confirmed controller boundary

game-controller-start-game.log: actual controller START reached guest XamInputGetState as buttons16,sequence6284; release buttons0,sequence6339. Then game called __imp__XamShowSigninUI at LR829b81e8 and runtime stopped with explicit unsupported-import exit42. User clarified that screen did NOT change; no visual/menu transition is claimed. Last frame remaining on tablet is consistent with stopped runtime. Input transport/state delivery is confirmed, menu navigation is not.

Next implement local persistent player identity and signin UI completion/notifications together; do not simply return success while XamUserGetSigninState/XUID/profile settings remain absent. Xbox profile/save service still incomplete. SDK xam_user.cpp:478 documents signin changed0xA and UI off0x9 notifications, but those must correspond to actual local profile state. Current runtime no longer running after exit42.
