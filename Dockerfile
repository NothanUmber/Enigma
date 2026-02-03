FROM debian:bookworm-slim

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
      bash \
      build-essential \
      ca-certificates \
      python3 && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /opt/enigma
COPY . .

RUN cd lib-src/enet && ./configure --disable-shared --enable-static && make
RUN g++ -std=c++14 tools/relay_server.cc -Ilib-src/enet/include lib-src/enet/libenet.a \
    -o /usr/local/bin/enigma-relay

COPY tools/docker-entrypoint.sh /usr/local/bin/enigma-entrypoint
RUN chmod +x /usr/local/bin/enigma-entrypoint

EXPOSE 12347/udp 12348/udp

ENTRYPOINT ["/usr/local/bin/enigma-entrypoint"]
