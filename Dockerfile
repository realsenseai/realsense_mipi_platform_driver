# syntax=docker/dockerfile:1

# Use a stable Ubuntu base image
FROM ubuntu:22.04

# Avoid interactive prompts during package installs
ENV DEBIAN_FRONTEND=noninteractive

# Build args for workspace and driver source folder
ARG workspace=/workspace
ARG driver_src_folder=drivers

# Make them available at runtime if needed
ENV workspace=${workspace}
ENV driver_src_folder=${driver_src_folder}

# Update and install required packages, then create directories
RUN set -xe \
    && apt-get update \
    && apt-get install -y --no-install-recommends \
       build-essential bc gcc-aarch64-linux-gnu wget flex bison curl libssl-dev xxd kmod git ca-certificates nano \
    && rm -rf /var/lib/apt/lists/* \
    && mkdir -p ${workspace}/src/${driver_src_folder}/l4t-gcc/6.x
# Configure git for better cloning in Docker
RUN git config --global http.postBuffer 1048576000 \
    && git config --global http.version HTTP/1.1 \
    && git config --global http.lowSpeedLimit 1000 \
    && git config --global http.lowSpeedTime 600 \
    && git config --global core.compression 0 \
    && git config --global transfer.fsckObjects false \
    && git config --global receive.fsckObjects false \
    && git config --global fetch.fsckObjects false

# Clone the realsense MIPI platform driver repository
WORKDIR ${workspace}/src/${driver_src_folder}
RUN set -xe \
    && git clone --depth 1 --progress --branch new-pipeline https://github.com/realsenseai/realsense_mipi_platform_driver.git \
    && cd realsense_mipi_platform_driver \

# Default working directory
WORKDIR ${workspace}/src/${driver_src_folder}/l4t-gcc/6.x

# Show final state for quick verification
RUN echo "Workspace: ${workspace}" && \
    echo "Driver src: ${driver_src_folder}" && \
    echo "PWD: $(pwd)" && \
    gcc --version && \
    aarch64-linux-gnu-gcc --version || true

# Default command
CMD ["bash"]
