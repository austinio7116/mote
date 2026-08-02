# Quest 3 (and every Horizon OS headset shipping) is arm64. There is no 32-bit
# headset to support, so unlike the phone build there is no second ABI here.
APP_ABI := arm64-v8a
# Quest 3 runs Android 12; 29 leaves room for older headsets and is well above
# AAudio's floor of 26.
APP_PLATFORM := android-29
APP_OPTIM := release
