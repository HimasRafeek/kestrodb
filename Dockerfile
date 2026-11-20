# KestroDB server image, built from source:
#   docker build -t kestrodb .
#   echo "$(openssl rand -hex 24)" > token.txt
#   docker run -d --name kestrodb -p 7820:7820 -v kestrodb-data:/data \
#     -v "$PWD/token.txt:/run/secrets/kestrodb_token:ro" -e KESTRODB_TOKEN_FILE=/run/secrets/kestrodb_token kestrodb
# Settings (environment): KESTRODB_DB (/data/kestro.kdb), KESTRODB_LISTEN (0.0.0.0:7820),
# KESTRODB_TOKEN_FILE or KESTRODB_USERS_FILE or KESTRODB_TOKEN; more options go after the image
# name (docker run ... kestrodb --backup-dir /data/backups --backup-every 1h). Without
# authentication the server refuses to listen on 0.0.0.0. docker-compose.example.yml has a single
# server and a three-server cluster. docker stop sends SIGTERM: the server stops cleanly.
FROM debian:bookworm-slim AS build
RUN apt-get update \
 && apt-get install -y --no-install-recommends g++ cmake ninja-build libssl-dev \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt ./
COPY cmake cmake
COPY src src
COPY vendor vendor
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DKESTRO_BUILD_TESTS=OFF -DKESTRO_NODE_ADDON=OFF \
      -DKESTRO_OUTPUT_DIR=/out -DCMAKE_EXE_LINKER_FLAGS=-s \
 && cmake --build build --target kestro-server

FROM debian:bookworm-slim
LABEL org.opencontainers.image.title="KestroDB" \
      org.opencontainers.image.licenses="Apache-2.0" \
      org.opencontainers.image.source="https://github.com/HimasRafeek/kestrodb"
RUN apt-get update \
 && apt-get install -y --no-install-recommends libssl3 ca-certificates \
 && rm -rf /var/lib/apt/lists/* \
 && useradd --system --uid 10001 --user-group --home-dir /data --no-create-home --shell /usr/sbin/nologin kestrodb \
 && mkdir /data && chown kestrodb:kestrodb /data && chmod 0750 /data
COPY --from=build /out/kestro-server /usr/local/bin/kestro-server
COPY packaging/docker/entrypoint.sh /usr/local/bin/kestrodb-entrypoint
COPY LICENSE NOTICE THIRD-PARTY.md /usr/share/doc/kestrodb/
COPY third-party-notices /usr/share/doc/kestrodb/third-party-notices
RUN chmod 0755 /usr/local/bin/kestro-server /usr/local/bin/kestrodb-entrypoint
USER kestrodb
WORKDIR /data
VOLUME /data
EXPOSE 7820
ENV KESTRODB_DB=/data/kestro.kdb KESTRODB_LISTEN=0.0.0.0:7820
# Healthy while the server accepts connections on its port.
HEALTHCHECK --interval=15s --timeout=3s --start-period=60s --retries=3 \
  CMD ["bash", "-c", "exec 3<>/dev/tcp/127.0.0.1/${KESTRODB_LISTEN##*:}"]
STOPSIGNAL SIGTERM
ENTRYPOINT ["kestrodb-entrypoint"]
