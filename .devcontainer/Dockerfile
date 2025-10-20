FROM ghcr.io/skalldri/nrf-connect-sdk-docker:latest
RUN /lib/systemd/systemd-udevd --daemon
RUN mkdir -p /jlink
RUN wget --post-data="accept_license_agreement=accepted&submit=Download+software" -O /jlink/JLink_Linux.deb https://www.segger.com/downloads/jlink/JLink_Linux_V860_x86_64.deb
RUN pip install lz4 yaspin tqdm