# Quest 3 and every Horizon OS headset shipping is arm64.
APP_ABI := arm64-v8a
# Match minSdk in app/build.gradle. Left behind at 29 while the gradle minSdk
# moved to 32 (dropping Quest 1), the native library would be built against an
# older set of NDK stubs than the app claims to need — harmless today, and
# exactly the sort of drift that produces a link error nobody can explain the
# first time a newer libc symbol gets used.
APP_PLATFORM := android-32
APP_OPTIM := release
