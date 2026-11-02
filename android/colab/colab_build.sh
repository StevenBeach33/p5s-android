#!/usr/bin/env bash
# Builds the NCE APK of a game on Google Colab (Ubuntu, root, no Docker) from a clone of this repository:
# the build tools (JDK, Android SDK/NDK, Vulkan headers, .NET 10), the shader translator library for Android
# (games/p5r/tools/build_harness_android.sh, run directly: Colab is the Linux host it wants), then the APK.
# Usage, from the repository root:  bash android/colab/colab_build.sh [tactica|p5r|p5s]
# Output: private/<game>/apk_nce/<game>.apk. The first run takes a while (downloads, and the translator's NativeAOT
# compile); a second run in the same session skips what is done. Nothing of any game is used or needed: the APK is the
# runtime; the player's own game file and keys are picked on the phone.
set -euo pipefail
game="${1:-tactica}"
root="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$root"
step() { printf '\n===== %s  (%ss) =====\n' "$1" "$SECONDS"; }

step "packages"
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq openjdk-17-jdk-headless unzip zip patch curl ninja-build >/dev/null
python3 -m pip install -q cmake

step "Android SDK, platform 35, build-tools 35.0.0, NDK 28.2.13676358"
export ANDROID_HOME="${ANDROID_HOME:-/opt/android-sdk}"
if [ ! -x "$ANDROID_HOME/cmdline-tools/latest/bin/sdkmanager" ]; then
    mkdir -p "$ANDROID_HOME/cmdline-tools"
    curl -sSL -o /tmp/cmdtools.zip https://dl.google.com/android/repository/commandlinetools-linux-11076708_latest.zip
    unzip -q /tmp/cmdtools.zip -d "$ANDROID_HOME/cmdline-tools"
    rm -rf "$ANDROID_HOME/cmdline-tools/latest"
    mv "$ANDROID_HOME/cmdline-tools/cmdline-tools" "$ANDROID_HOME/cmdline-tools/latest"
fi
export PATH="$ANDROID_HOME/cmdline-tools/latest/bin:$PATH"
( yes || true ) | sdkmanager --licenses >/dev/null 2>&1 || true
sdkmanager "platforms;android-35" "build-tools;35.0.0" "ndk;28.2.13676358" >/dev/null

step "Vulkan headers 1.4.357"
vk="$root/private/third_party/Vulkan-Headers-1.4.357"
[ -d "$vk" ] || git clone -q --depth 1 --branch v1.4.357 https://github.com/KhronosGroup/Vulkan-Headers "$vk"
mkdir -p "$root/private/third_party/VulkanSDK"
ln -sfn "$vk/include" "$root/private/third_party/VulkanSDK/Include"   # the build looks for VULKAN_SDK/Include
export VULKAN_SDK="$root/private/third_party/VulkanSDK"

step ".NET SDK 10"
export DOTNET_ROOT=/opt/dotnet DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1
if [ ! -x "$DOTNET_ROOT/dotnet" ]; then
    curl -sSL https://dot.net/v1/dotnet-install.sh -o /tmp/dotnet-install.sh
    bash /tmp/dotnet-install.sh --channel 10.0 --install-dir "$DOTNET_ROOT" >/dev/null
fi
export PATH="$DOTNET_ROOT:$PATH"
dotnet --version

step "shader translator source (Ryubing a82350bb77)"
if [ ! -d "$root/private/third_party/ryubing/src/Ryujinx.Graphics.Shader" ]; then
    mkdir -p "$root/private/third_party/ryubing"
    curl -sSL -o /tmp/ryubing.tar.gz https://codeberg.org/Software_arch/Ryubing/archive/a82350bb77.tar.gz
    tar -xzf /tmp/ryubing.tar.gz -C "$root/private/third_party/ryubing" --strip-components=1
fi

step "shader translator library for Android (NativeAOT; the NDK r28c download is ~700 MB)"
harness="$root/private/build/spirv_harness_android/libspirvharness.so"
if [ ! -f "$harness" ]; then
    # the script's paths are /src (the Docker mount): Colab is root, so the repository can be linked there
    [ "$root" = /src ] || ln -sfn "$root" /src
    # the script would download NDK r28c (~700 MB) again: it is the one sdkmanager installed (28.2.13676358)
    [ -e "$root/private/third_party/android-ndk-r28c-linux" ] ||
        ln -sfn "$ANDROID_HOME/ndk/28.2.13676358" "$root/private/third_party/android-ndk-r28c-linux"
    ( cd /src && sh games/p5r/tools/build_harness_android.sh )
fi

step "APK ($game)"
bash android/build_apk_nce.sh --game "$game"
step "done"
ls -la "private/$game/apk_nce/$game.apk"
