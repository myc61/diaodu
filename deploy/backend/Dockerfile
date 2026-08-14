FROM ubuntu:22.04 AS build

ARG DEBIAN_FRONTEND=noninteractive
ARG DROGON_VERSION=v1.9.13

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        cmake \
        git \
        libboost-system-dev \
        libjsoncpp-dev \
        nlohmann-json3-dev \
        libssl-dev \
        uuid-dev \
        zlib1g-dev \
        libpq-dev \
        libpqxx-dev \
    && rm -rf /var/lib/apt/lists/*

RUN git clone \
        --branch "${DROGON_VERSION}" \
        --depth 1 \
        --recurse-submodules \
        https://github.com/drogonframework/drogon.git \
        /tmp/drogon \
    && cmake \
        -S /tmp/drogon \
        -B /tmp/drogon-build \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/opt/drogon \
        -DBUILD_CTL=OFF \
        -DBUILD_EXAMPLES=OFF \
        -DBUILD_ORM=OFF \
        -DBUILD_SHARED_LIBS=OFF \
        -DBUILD_TESTING=OFF \
    && cmake --build /tmp/drogon-build --parallel 2 \
    && cmake --install /tmp/drogon-build

WORKDIR /src
COPY CMakeLists.txt ./
COPY backend ./backend

RUN cmake \
        -S . \
        -B build \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_PREFIX_PATH=/opt/drogon \
        -DDISPATCHER_REQUIRE_DROGON=ON \
        -DBUILD_TESTING=OFF \
    && cmake --build build --target dispatcher_api --parallel 2

FROM ubuntu:22.04 AS runtime

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates \
        curl \
        libboost-system1.74.0 \
        libjsoncpp25 \
        libssl3 \
        libuuid1 \
        zlib1g \
        libpq5 \
        libpqxx-6.4 \
        openssh-client \
    && rm -rf /var/lib/apt/lists/*

RUN groupadd --system dispatcher \
    && useradd --system --gid dispatcher --home /var/lib/dispatcher dispatcher \
    && mkdir -p /var/lib/dispatcher/maps \
    && chown -R dispatcher:dispatcher /var/lib/dispatcher

COPY --from=build /src/build/backend/dispatcher_api /usr/local/bin/dispatcher_api

USER dispatcher
WORKDIR /var/lib/dispatcher

ENV DISPATCHER_HTTP_PORT=8080
ENV DISPATCHER_MAP_ROOT=/var/lib/dispatcher/maps
EXPOSE 8080

HEALTHCHECK --interval=10s --timeout=3s --start-period=10s --retries=3 \
  CMD curl --fail --silent http://127.0.0.1:8080/api/v1/health >/dev/null || exit 1

ENTRYPOINT ["/usr/local/bin/dispatcher_api"]
