FROM ubuntu:20.04

RUN echo "deb http://mirrors.bfsu.edu.cn/ubuntu/ focal main restricted universe multiverse" > /etc/apt/sources.list
RUN echo "deb http://mirrors.bfsu.edu.cn/ubuntu/ focal-updates main restricted universe multiverse" >> /etc/apt/sources.list
RUN apt-get update
RUN env DEBIAN_FRONTEND=noninteractive apt-get install -y build-essential cmake git llvm qemu-user g++-arm-linux-gnueabihf time python3-tabulate clang unzip curl
RUN env DEBIAN_FRONTEND=noninteractive apt-get install -y default-jre-headless --no-install-recommends

CMD ["/bin/bash"]
