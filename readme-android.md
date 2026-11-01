# Verdandi for android

norns scripts on an android phone or tablet (arm64, Android 10 or newer). work in progress,
on the `android` branch.

## status

1. lua core, screen, keys and knobs (touch): **this step**. scripts load and run, without
   sound: there is no SuperCollider yet. a stand-in (`matron/src/platform/android/fake_sclang.cc`)
   answers matron's engine handshake with the commands read from the engine's `.sc` file, so
   `engine.hz(440)` &c do nothing instead of stopping the script.
2. crone and softcut (audio through Oboe/AAudio)
3. SuperCollider engines (scsynth for android)
4. usb midi, grid, arc, hid; maiden

## install

the `android-dev` pre-release on GitHub has the latest `Verdandi.apk` (built by
`.github/workflows/android.yml` on every push to the branch). open it on the phone and allow
installing from that source. every build is signed with the same key
(`android/verdandi-debug.keystore`), so a new one installs over the old one and keeps dust.

## use

the window is a norns: the screen, K1-K3 and E1-E3 where they sit on the device. touch a key
to press it, drag from a knob to turn it (right or up: clockwise). each finger works on its
own, so hold K1 with one while another turns E3. MIX TAPE HOME PARAMS along the top open the
menu pages.

the norns home is the app's folder `Android/data/org.verdandi.app/files`: connect the phone to
a computer over USB and copy scripts into `dust/code` there (awake and mlr come with the app).
`norns.log` in the same folder is matron's output.

## build

the android SDK and NDK, then:

```
android/fetch-deps.sh                     # SDL, lua 5.3, libmonome, the bundled scripts
git clone https://github.com/microsoft/vcpkg android/deps/vcpkg && android/deps/vcpkg/bootstrap-vcpkg.sh
ANDROID_NDK_HOME=<ndk> gradle -p android assembleDebug -PndkVersion=<ndk version>
```

cairo, freetype, liblo, libsndfile and nng come from vcpkg (`android/app/src/main/cpp/vcpkg.json`);
the rest is built from source by `android/app/src/main/cpp/CMakeLists.txt`. matron is the same
source as on norns and macOS; android differences are under `#ifdef __ANDROID__` and in
`matron/src/platform/android`.
