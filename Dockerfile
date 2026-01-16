FROM ubuntu:22.04 AS builder

RUN apt-get update && apt-get install -y \
    cmake \
    g++ \
    libssl-dev \
    libyaml-cpp-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY src/ src/
COPY CMakeLists.txt .

RUN mkdir build && cd build \
    && cmake .. -DBUILD_TESTS=OFF -DBUILD_BENCH=OFF \
    && cmake --build . -j$(nproc)

FROM ubuntu:22.04

RUN apt-get update && apt-get install -y \
    libssl3 \
    libyaml-cpp0.7 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY --from=builder /app/build/lb .
COPY config.yaml .

EXPOSE 8080 9090

CMD ["./lb", "config.yaml"]
