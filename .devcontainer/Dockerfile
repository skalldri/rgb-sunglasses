FROM ghcr.io/skalldri/nrf-connect-sdk-docker:3.1.1
RUN /lib/systemd/systemd-udevd --daemon
# poppler-utils: pdftotext/pdftoppm/pdfinfo, needed to read the checked-in part
# datasheets under fw/docs/datasheets/ (several are encrypted PDFs that plain
# stdlib tooling cannot open).
RUN apt-get update && apt-get install -y --no-install-recommends lcov ffmpeg qrencode dosfstools screen jq shellcheck poppler-utils && rm -rf /var/lib/apt/lists/*

# Doxygen — builds the rgbx extension API docs published at /api. Installed
# from the upstream release tarball rather than apt (which ships 1.9.1 here)
# because the docs build runs with WARN_AS_ERROR and Doxygen's warning set
# varies by release: a version mismatch between here and CI would mean "clean
# locally, red in CI". The version must stay in sync with
# .github/actions/doxygen/action.yml, which is the authoritative pin, and stay
# inside the range the vendored theme supports — see
# fw/extensions/doxygen/VENDORED.md. (Same duplicate-the-pin pattern, and same
# reason, as the wasi-sdk install further down.)
# (wget, not curl: curl is only apt-installed further down, but the base image
# already provides wget — see the J-Link download below.)
RUN wget -qO- https://github.com/doxygen/doxygen/releases/download/Release_1_12_0/doxygen-1.12.0.linux.bin.tar.gz \
        | tar -xz -C /opt \
    && ln -s /opt/doxygen-1.12.0/bin/doxygen /usr/local/bin/doxygen
RUN mkdir -p /jlink
RUN wget --post-data="accept_license_agreement=accepted&submit=Download+software" -O /jlink/JLink_Linux.deb https://www.segger.com/downloads/jlink/JLink_Linux_V860_x86_64.deb

# --- React Native (Android) toolchain ---------------------------------------
# Here we add the JDK, Android SDK, Node.js, and Rust needed to build the
# Expo dev-client locally and to drive a physical phone over adb.
# iOS is not supported in-container (it requires macOS/Xcode) and is built
# on a Mac instead.
#
# The NDK (1.5 GB download, ~3 GB extracted) and adb-wireless are NOT baked
# into the image here because they create an oversized Docker layer that
# causes CI pushes to ghcr.io to fail.  They are installed on first container
# open via postCreateCommand (see devcontainer.json) and skipped in CI.
RUN apt-get update && apt-get install -y --no-install-recommends \
        openjdk-17-jdk-headless unzip curl \
    && rm -rf /var/lib/apt/lists/*

# GitHub CLI
RUN curl -fsSL https://cli.github.com/packages/githubcli-archive-keyring.gpg \
        | dd of=/usr/share/keyrings/githubcli-archive-keyring.gpg \
    && chmod go+r /usr/share/keyrings/githubcli-archive-keyring.gpg \
    && echo "deb [arch=$(dpkg --print-architecture) signed-by=/usr/share/keyrings/githubcli-archive-keyring.gpg] https://cli.github.com/packages stable main" \
        > /etc/apt/sources.list.d/github-cli.list \
    && apt-get update && apt-get install -y --no-install-recommends gh \
    && rm -rf /var/lib/apt/lists/*
ENV JAVA_HOME=/usr/lib/jvm/java-17-openjdk-amd64
ENV ANDROID_HOME=/opt/android-sdk
ENV ANDROID_SDK_ROOT=/opt/android-sdk
ENV PATH=${PATH}:${ANDROID_HOME}/cmdline-tools/latest/bin:${ANDROID_HOME}/platform-tools

# Node.js 20 LTS — installed via NodeSource so the devcontainer "node" feature
# is not needed, which removes a GHCR pull dependency from the docker build step.
RUN curl -fsSL https://deb.nodesource.com/setup_20.x | bash - \
    && apt-get install -y --no-install-recommends nodejs \
    && rm -rf /var/lib/apt/lists/*

# wasi-sdk — pinned clang/wasm32 toolchain used by the extension simulator
# (fw/sim/) to compile rgbx animation extensions to WebAssembly. The version
# must stay in sync with fw/sim/scripts/install-toolchain.sh, which is the
# fallback path (cached download) for containers built before this layer and
# for CI.
# The image ships the VERIFIED ARCHIVE, not an extracted tree. The installer
# treats /opt as a verified-archive candidate: it re-hashes this archive
# against its own pin and extracts clang from it on first use, so trust always
# flows from bytes that match the pin rather than from a tree written into a
# layer. Keep this digest in sync with WASI_SDK_SHA256_X86_64_LINUX in
# fw/sim/scripts/install-toolchain.sh.
RUN curl -fsSL -o /opt/wasi-sdk-33.0-x86_64-linux.tar.gz \
        https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-33/wasi-sdk-33.0-x86_64-linux.tar.gz \
    && echo "0ba8b5bfaeb2adf3f29bab5841d76cf5318ab8e1642ea195f88baba1abd47bce  /opt/wasi-sdk-33.0-x86_64-linux.tar.gz" \
        | sha256sum -c -

# Android command-line tools + core SDK packages.
# NDK and cmake are intentionally excluded — they are too large to put in the
# image layer; install them via postCreateCommand on first container open.
RUN mkdir -p ${ANDROID_HOME}/cmdline-tools \
    && wget -q https://dl.google.com/android/repository/commandlinetools-linux-11076708_latest.zip -O /tmp/cmdtools.zip \
    && unzip -q /tmp/cmdtools.zip -d ${ANDROID_HOME}/cmdline-tools \
    && mv ${ANDROID_HOME}/cmdline-tools/cmdline-tools ${ANDROID_HOME}/cmdline-tools/latest \
    && rm /tmp/cmdtools.zip \
    && yes | sdkmanager --licenses >/dev/null \
    && sdkmanager --install \
        "platform-tools" \
        "platforms;android-35" \
        "build-tools;35.0.0"

# mcumgr CLI — firmware management over UART/BLE (SMP protocol).
# Requires Go; we install a pinned Go toolchain, build mcumgr, then remove Go
# to keep the layer lean.
RUN curl -fsSL https://go.dev/dl/go1.26.4.linux-amd64.tar.gz \
        | tar -C /usr/local -xz \
    && GOPATH=/tmp/gobuild /usr/local/go/bin/go install \
        github.com/apache/mynewt-mcumgr-cli/mcumgr@latest \
    && mv /tmp/gobuild/bin/mcumgr /usr/local/bin/mcumgr \
    && rm -rf /usr/local/go /tmp/gobuild

# Rust toolchain (via rustup) — needed for adb-wireless and other cargo tools.
# apt's cargo is too old (1.75) to build edition2024 crates.
# cargo install adb-wireless is deferred to postCreateCommand to avoid
# running a Rust compilation step during docker build (memory-intensive).
ENV RUSTUP_HOME=/usr/local/rustup \
    CARGO_HOME=/usr/local/cargo \
    PATH=/usr/local/cargo/bin:${PATH}
RUN curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs \
    | sh -s -- -y --no-modify-path --default-toolchain stable
