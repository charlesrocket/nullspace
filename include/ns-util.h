#pragma once

#include <stdbool.h>
#include <sys/socket.h>

bool set_nosigpipe(int fd) {
    const int val = 1;
    return setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &val, sizeof(val)) == 0;
}
