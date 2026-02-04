FROM debian:bookworm-slim

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
      bash \
      build-essential \
      ca-certificates \
      libenet-dev \
      python3 && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /opt/enigma
COPY tools/internet_lobby_server.py tools/relay_server.cc tools/tcp_relay_server.cc tools/docker-entrypoint.sh tools/

RUN g++ -std=c++14 tools/relay_server.cc -lenet -o /usr/local/bin/enigma-relay
RUN g++ -std=c++14 tools/tcp_relay_server.cc -o /usr/local/bin/enigma-tcp-relay

COPY tools/docker-entrypoint.sh /usr/local/bin/enigma-entrypoint
RUN chmod +x /usr/local/bin/enigma-entrypoint

EXPOSE 12347/udp 12348/udp 12349/tcp

ENTRYPOINT ["/usr/local/bin/enigma-entrypoint"]
