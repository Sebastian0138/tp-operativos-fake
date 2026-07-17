#include "sockets.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netdb.h>

int crear_servidor(const char* puerto) {
    struct addrinfo hints, *server_info;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    if (getaddrinfo(NULL, puerto, &hints, &server_info) != 0) {
        return -1;
    }

    int fd_escucha = socket(server_info->ai_family,
                            server_info->ai_socktype,
                            server_info->ai_protocol);

    if (fd_escucha == -1) {
        freeaddrinfo(server_info);
        return -1;
    }

    // Evitar "Address already in use" al reiniciar el servidor
    setsockopt(fd_escucha, SOL_SOCKET, SO_REUSEPORT, &(int){1}, sizeof(int));

    if (bind(fd_escucha, server_info->ai_addr, server_info->ai_addrlen) == -1) {
        close(fd_escucha);
        freeaddrinfo(server_info);
        return -1;
    }

    if (listen(fd_escucha, SOMAXCONN) == -1) {
        close(fd_escucha);
        freeaddrinfo(server_info);
        return -1;
    }

    freeaddrinfo(server_info);
    return fd_escucha;
}

int crear_conexion(const char* ip, const char* puerto) {
    struct addrinfo hints, *server_info;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(ip, puerto, &hints, &server_info) != 0) {
        return -1;
    }

    int fd_conexion = socket(server_info->ai_family,
                             server_info->ai_socktype,
                             server_info->ai_protocol);

    if (fd_conexion == -1) {
        freeaddrinfo(server_info);
        return -1;
    }

    if (connect(fd_conexion, server_info->ai_addr, server_info->ai_addrlen) == -1) {
        close(fd_conexion);
        freeaddrinfo(server_info);
        return -1;
    }

    freeaddrinfo(server_info);
    return fd_conexion;
}
