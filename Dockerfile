# Dockerfile for Undeleter Bot
FROM ubuntu:22.04

# Install dependencies
RUN apt-get update && apt-get install -y \
    cmake \
    g++ \
    libssl-dev \
    zlib1g-dev \
    git \
    && rm -rf /var/lib/apt/lists/*

# Clone and install D++
RUN git clone https://github.com/brainboxdotcc/DPP.git /tmp/DPP \
    && cd /tmp/DPP \
    && mkdir build && cd build \
    && cmake .. -DBUILD_SHARED_LIBS=ON \
    && make -j$(nproc) \
    && make install \
    && rm -rf /tmp/DPP

# Copy bot source
WORKDIR /app
COPY . .

# Build the bot
RUN mkdir build && cd build \
    && cmake .. \
    && make -j$(nproc)

# Run the bot
CMD ["./build/undeleter-bot", "config.yml"]

# Default config (can be overridden by mounting a volume)
COPY config.example.yml config.yml
