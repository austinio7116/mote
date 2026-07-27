# arm64 only by default: every phone shipped since ~2015 is arm64, and the APK
# carries ~30 game modules per ABI. Override for the x86_64 emulator with
#   ./gradlew assembleDebug -PmoteAbis=arm64-v8a,x86_64
APP_ABI := arm64-v8a
APP_PLATFORM := android-23
APP_OPTIM := release
