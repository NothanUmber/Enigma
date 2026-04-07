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
COPY tools/internet_lobby_server.py tools/relay_server.cc tools/tcp_relay_server.cc tools/ws_relay_server.py tools/docker-entrypoint.sh tools/

RUN g++ -std=c++14 tools/relay_server.cc -lenet -o /usr/local/bin/enigma-relay
RUN g++ -std=c++14 tools/tcp_relay_server.cc -o /usr/local/bin/enigma-tcp-relay

COPY tools/docker-entrypoint.sh /usr/local/bin/enigma-entrypoint
RUN chmod +x /usr/local/bin/enigma-entrypoint

# 12347/udp: Internet lobby server (room coordination over UDP)
# 12347/tcp: Internet lobby control fallback (HTTP/HTTPS-reverse-proxy target)
# 12348/udp: UDP relay (ENet) for gameplay transport fallback
# 12349/tcp: TCP relay for networks that block UDP
# 12350/tcp: WebSocket relay for proxy-friendly gameplay fallback
EXPOSE 12347/udp 12347/tcp 12348/udp 12349/tcp 12350/tcp

ENTRYPOINT ["/usr/local/bin/enigma-entrypoint"]
