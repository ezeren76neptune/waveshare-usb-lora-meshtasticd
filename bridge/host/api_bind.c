// MESHTASTICD_API_BIND=<IPv4>: bind meshtasticd's TCP listeners to that address instead of every
// interface. portduino's WiFiServer hardcodes INADDR_ANY, so the build wraps bind() (-Wl,--wrap=bind).
// Only TCP sockets bound to INADDR_ANY are rewritten; UDP (multicast mesh) and explicit binds are untouched.
#if !defined(ARCH_PORTDUINO) || defined(MESHTASTIC_WSBRIDGE)

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

int __real_bind(int fd, const struct sockaddr *addr, socklen_t len);

int __wrap_bind(int fd, const struct sockaddr *addr, socklen_t len)
{
    const char *want = getenv("MESHTASTICD_API_BIND");
    if (!want || !*want || !addr || addr->sa_family != AF_INET || len < (socklen_t)sizeof(struct sockaddr_in))
        return __real_bind(fd, addr, len);

    int type = 0;
    socklen_t tlen = sizeof(type);
    struct sockaddr_in in;
    memcpy(&in, addr, sizeof(in));
    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &tlen) != 0 || type != SOCK_STREAM || in.sin_addr.s_addr != htonl(INADDR_ANY))
        return __real_bind(fd, addr, len);

    if (inet_pton(AF_INET, want, &in.sin_addr) != 1) {
        fprintf(stderr, "MESHTASTICD_API_BIND=%s is not an IPv4 address; binding to all interfaces\n", want);
        return __real_bind(fd, addr, len);
    }
    fprintf(stderr, "TCP port %u bound to %s only (MESHTASTICD_API_BIND)\n", ntohs(in.sin_port), want);
    return __real_bind(fd, (const struct sockaddr *)&in, sizeof(in));
}

#endif
