#!/usr/bin/env bash
# The native-execution (NX_NCE) APK of a game, on Linux (Colab, CI): the steps of android/build_apk.ps1 -Nce, in bash.
# cmake (NDK) -> lib<game>.so + lib<game>import.so -> aapt2 -> javac + d8 -> zip -> zipalign -> apksigner.
# The APK holds no game code or data: it runs the exefs of the player's own game file.
#
# Usage: [ANDROID_KEYSTORE=path] android/build_apk_nce.sh [--game tactica|p5r|p5s] [--release]
# (the same keystore on every build, or Android refuses to update the installed app and it must be uninstalled first)
# Needs: ANDROID_HOME with platform 35, build-tools 35.0.0, NDK 28.2.13676358; a JDK 17+ (javac, keytool); cmake, ninja,
# zip; the Vulkan headers 1.4.357 in VULKAN_SDK/Include (android/colab/colab_build.sh sets all of it up);
# private/build/spirv_harness_android/libspirvharness.so (games/p5r/tools/build_harness_android.sh).
# Output: private/<game>/apk_nce/<game>.apk
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
game=tactica
release=0
while [ $# -gt 0 ]; do
    case "$1" in
        --game) game="$2"; shift 2 ;;
        --release) release=1; shift ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done
case "$game" in tactica|p5r|p5s) ;; *) echo "--game: tactica, p5r or p5s (the games with an on-phone import)" >&2; exit 2 ;; esac

sdk="${ANDROID_HOME:?set ANDROID_HOME to the Android SDK}"
ndk="$sdk/ndk/28.2.13676358"
bt="$sdk/build-tools/35.0.0"
jar="$sdk/platforms/android-35/android.jar"
for f in "$ndk/build/cmake/android.toolchain.cmake" "$bt/aapt2" "$jar"; do
    [ -e "$f" ] || { echo "missing $f (SDK platform 35, build-tools 35.0.0, NDK 28.2.13676358)" >&2; exit 1; }
done
[ -e "${VULKAN_SDK:-/nonexistent}/Include/vulkan/vulkan.h" ] || { echo "VULKAN_SDK/Include/vulkan/vulkan.h not found" >&2; exit 1; }
harness="$root/private/build/spirv_harness_android/libspirvharness.so"
[ -f "$harness" ] || { echo "missing $harness: build the shader translator first (games/p5r/tools/build_harness_android.sh)" >&2; exit 1; }

priv="$root/private/$game"
out="$priv/apk_nce"
build="$priv/ba_nce"
host=linux-x86_64
llvm="$ndk/toolchains/llvm/prebuilt/$host/bin"

if [ ! -f "$build/build.ninja" ]; then
    cmake -S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_static \
        -DNX_GAME="$game" -DNX_NCE=ON
fi
if [ "$release" = 1 ]; then cmake -B "$build" -DNX_RELEASE=ON >/dev/null; fi
touch "$root/games/$game/game.cpp"   # its build stamp (nx.log "build:" line) is then this build's
cmake --build "$build" --target "$game" "${game}import"

rm -rf "$out"
mkdir -p "$out/lib/arm64-v8a" "$out/res"
"$llvm/llvm-strip" --strip-unneeded -o "$out/lib/arm64-v8a/lib$game.so" "$build/lib$game.so"
"$llvm/llvm-strip" --strip-unneeded -o "$out/lib/arm64-v8a/lib${game}import.so" "$build/lib${game}import.so"
"$llvm/llvm-strip" --strip-unneeded -o "$out/lib/arm64-v8a/libspirvharness.so" "$harness"

"$bt/aapt2" compile --dir "$root/android/res" -o "$out/res.zip"
# a new versionCode every build (minutes since the epoch): the installer then updates the installed app in place,
# keeping its data (as long as the signing key is the same one: ANDROID_KEYSTORE, kept between builds)
vcode=$(( $(date +%s) / 60 ))
sed -E "s/android:versionCode=\"[0-9]+\"/android:versionCode=\"$vcode\"/" \
    "$root/games/$game/AndroidManifest.xml" > "$out/AndroidManifest.base.xml"
manifest="$out/AndroidManifest.base.xml"
res_zips=("$out/res.zip")
# the launcher icon: the project's own for the game, else the player's (made on their side from their dump)
icon_res="$root/games/$game/icon/res"
[ -d "$icon_res" ] || icon_res="$priv/icon/res"
if [ -d "$icon_res" ]; then
    "$bt/aapt2" compile --dir "$icon_res" -o "$out/icon.zip"
    res_zips+=("$out/icon.zip")
    sed 's/<application/<application android:icon="@mipmap\/ic_launcher"/' "$manifest" > "$out/AndroidManifest.xml"
    manifest="$out/AndroidManifest.xml"
fi
"$bt/aapt2" link -o "$out/base.apk" -I "$jar" --manifest "$manifest" "${res_zips[@]}"

# the game's launcher: javac against android.jar, d8 to classes.dex
java="$root/games/$game/java"
if [ -d "$java" ]; then
    mkdir -p "$out/classes" "$out/dex"
    find "$java" -name '*.java' > "$out/sources.txt"
    javac -encoding UTF-8 -source 8 -target 8 -Xlint:-options -bootclasspath "$jar" -d "$out/classes" @"$out/sources.txt"
    find "$out/classes" -name '*.class' > "$out/classes.txt"
    "$bt/d8" --release --min-api 30 --lib "$jar" --output "$out/dex" @"$out/classes.txt"
    (cd "$out/dex" && zip -q "$out/base.apk" classes.dex)
fi
# the native libraries (compressed; the manifest has extractNativeLibs)
(cd "$out" && zip -q -r "$out/base.apk" lib)

"$bt/zipalign" -f -p 4 "$out/base.apk" "$out/aligned.apk"
ks="${ANDROID_KEYSTORE:-$HOME/.android/debug.keystore}"
if [ ! -f "$ks" ]; then
    mkdir -p "$(dirname "$ks")"
    keytool -genkeypair -keystore "$ks" -storepass android -keypass android -alias androiddebugkey \
        -dname "CN=Android Debug,O=Android,C=US" -keyalg RSA -validity 10000 >/dev/null 2>&1
fi
"$bt/apksigner" sign --ks "$ks" --ks-pass pass:android --key-pass pass:android \
    --out "$out/$game.apk" "$out/aligned.apk"
echo "APK: $out/$game.apk ($(du -h "$out/$game.apk" | cut -f1))"
