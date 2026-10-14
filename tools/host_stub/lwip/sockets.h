// Host-test stub for lwip/sockets.h: real address structs, captured sendto.
#pragma once

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

// Implemented in tools/aoip_host_stubs.c: appends the packet to the capture.
ssize_t host_sendto_capture(int s, const void *buf, size_t len, int flags,
                            const struct sockaddr *dst, socklen_t slen);

#define sendto host_sendto_capture
#include <unistd.h>
