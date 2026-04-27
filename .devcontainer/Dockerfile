FROM ghcr.io/skalldri/nrf-connect-sdk-docker:3.1.1
RUN /lib/systemd/systemd-udevd --daemon
RUN apt-get update && apt-get install -y --no-install-recommends lcov && rm -rf /var/lib/apt/lists/*
RUN mkdir -p /jlink
RUN wget --post-data="accept_license_agreement=accepted&submit=Download+software" -O /jlink/JLink_Linux.deb https://www.segger.com/downloads/jlink/JLink_Linux_V860_x86_64.deb