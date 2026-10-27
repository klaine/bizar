FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
    gcc \
    gcc-aarch64-linux-gnu \
    python3 \
    libc6-dev \
    libc6-dev-arm64-cross \
    binutils-aarch64-linux-gnu \
    file \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /work
CMD ["sh", "./scripts/build.sh"]
