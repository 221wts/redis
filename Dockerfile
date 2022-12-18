FROM ubuntu:latest
CMD bash

# 下面的已经可以用与构建镜像和容器了，但是还是不太符合需求，毕竟需要一个
# Install Ubuntu packages.
# Please add packages in alphabetical order.
RUN apt-get -y update && \
    apt-get -y install \
      build-essential \
      gdb \
      net-tools \
      git
