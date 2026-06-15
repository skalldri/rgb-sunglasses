FROM ghcr.io/skalldri/nrf-connect-sdk-docker:3.1.1
RUN /lib/systemd/systemd-udevd --daemon
RUN apt-get update && apt-get install -y --no-install-recommends lcov && rm -rf /var/lib/apt/lists/*
RUN mkdir -p /jlink
RUN wget --post-data="accept_license_agreement=accepted&submit=Download+software" -O /jlink/JLink_Linux.deb https://www.segger.com/downloads/jlink/JLink_Linux_V860_x86_64.deb

# --- React Native (Android) toolchain ---------------------------------------
# Node.js is provided by the devcontainer "node" feature (see devcontainer.json).
# Here we add the JDK and Android SDK needed to build the Expo dev-client locally
# and to drive a physical phone over adb. iOS is not supported in-container
# (it requires macOS/Xcode) and is built on a Mac instead.
RUN apt-get update && apt-get install -y --no-install-recommends \
        openjdk-17-jdk-headless unzip \
    && rm -rf /var/lib/apt/lists/*
ENV JAVA_HOME=/usr/lib/jvm/java-17-openjdk-amd64
ENV ANDROID_HOME=/opt/android-sdk
ENV ANDROID_SDK_ROOT=/opt/android-sdk
ENV PATH=${PATH}:${ANDROID_HOME}/cmdline-tools/latest/bin:${ANDROID_HOME}/platform-tools

# Install the Android command-line tools, then the SDK packages required by
# Expo SDK 54 / React Native 0.81 (new architecture).
RUN mkdir -p ${ANDROID_HOME}/cmdline-tools \
    && wget -q https://dl.google.com/android/repository/commandlinetools-linux-11076708_latest.zip -O /tmp/cmdtools.zip \
    && unzip -q /tmp/cmdtools.zip -d ${ANDROID_HOME}/cmdline-tools \
    && mv ${ANDROID_HOME}/cmdline-tools/cmdline-tools ${ANDROID_HOME}/cmdline-tools/latest \
    && rm /tmp/cmdtools.zip \
    && yes | sdkmanager --licenses >/dev/null \
    && sdkmanager --install \
        "platform-tools" \
        "platforms;android-35" \
        "build-tools;35.0.0" \
        "ndk;27.1.12297006" \
        "cmake;3.22.1"

# Rust toolchain (via rustup) — needed for adb-wireless and other cargo tools.
# apt's cargo is too old (1.75) to build edition2024 crates.
ENV RUSTUP_HOME=/usr/local/rustup \
    CARGO_HOME=/usr/local/cargo \
    PATH=/usr/local/cargo/bin:${PATH}
RUN curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs \
    | sh -s -- -y --no-modify-path --default-toolchain stable \
 && cargo install --jobs 1 adb-wireless
