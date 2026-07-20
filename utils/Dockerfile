FROM debian:trixie

RUN apt-get update && \
    env DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        clang \
        cmake \
        curl \
        default-jre-headless \
        g++-arm-linux-gnueabihf \
        git \
        llvm \
        python3 \
        python3-tabulate \
        qemu-user \
        time \
        unzip && \
    rm -rf /var/lib/apt/lists/*

CMD ["/bin/bash"]
