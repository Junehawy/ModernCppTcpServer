FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    cmake \
    ninja-build \
    build-essential \
    pkg-config \
    libprotobuf-dev \
    protobuf-compiler \
    libgrpc++-dev \
    protobuf-compiler-grpc \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTS=OFF \
    && cmake --build build --parallel

FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    libprotobuf-dev \
    libgrpc++-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=builder /src/build/bin/server .
RUN mkdir -p logs

EXPOSE 9999
EXPOSE 50051
CMD ["./server"]