/*
 * Copyright (C) 2012 Vermeer Manufacturing Co.
 * License: GPLv3 with additional permissions (see README).
 */

#include <assert.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#define WIN_SOCKET_PORT "50007"
#else
#include <sys/socket.h>
#include <sys/un.h>
#endif


#include "ipc.h"

struct ipc_softc {
	int socket;
	go_handler h_go;
	write_handler h_write;
	read_handler h_read;
	void *user;
};

struct ipc_softc *ipc_connect(const char *sockaddr,
	go_handler h_go, write_handler h_write, read_handler h_read, void *user)
{
	struct ipc_softc *sc;
#ifdef _WIN32
	struct addrinfo hints, *my_addrinfo;
	WSADATA wsaData;
#else
	struct sockaddr_un addr;
#endif

	sc = malloc(sizeof(struct ipc_softc));
	if(!sc) return NULL;

	sc->h_go = h_go;
	sc->h_write = h_write;
	sc->h_read = h_read;
	sc->user = user;

#ifdef _WIN32
	/* Initialize Winsock. */
	if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
		free(sc);
		return NULL;
	}

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	if(getaddrinfo(sockaddr, WIN_SOCKET_PORT, NULL, &my_addrinfo) != 0) {
		free(sc);
		return NULL;
	}

	sc->socket = socket(AF_INET, SOCK_STREAM, 0);
	if(sc->socket < 0) {
		free(sc);
		return NULL;
	}

	if(connect(sc->socket, my_addrinfo->ai_addr, my_addrinfo->ai_addrlen) != 0) {
		close(sc->socket);
		free(sc);
		return NULL;
	}
#else
	sc->socket = socket(AF_UNIX, SOCK_SEQPACKET, 0);
	if(sc->socket < 0) {
		free(sc);
		return NULL;
	}

	addr.sun_family = AF_UNIX;
	strcpy(addr.sun_path, sockaddr);
	if(connect(sc->socket, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		close(sc->socket);
		free(sc);
		return NULL;
	}
#endif

	return sc;
}

void ipc_destroy(struct ipc_softc *sc)
{
	close(sc->socket);
	free(sc);
#ifdef _WIN32
	WSACleanup();
#endif
}

enum {
	MESSAGE_TICK = 0,
	MESSAGE_GO,
	MESSAGE_WRITE,
	MESSAGE_READ,
	MESSAGE_READ_REPLY
};

#define MAX_LEN 2048

/*
 * 0 -> error
 * 1 -> success
 * 2 -> graceful shutdown
 */
int ipc_receive(struct ipc_softc *sc)
{
	unsigned char buffer[MAX_LEN];
	ssize_t l = 0;
	int i;
#ifdef _WIN32
	int expected_num;
	int received_num = 0;
#endif

#ifdef _WIN32
	/* Initial recv. Includes a length identifier so that we wait
	 * until a full message is received. The length of the message
	 * includes the length identifier. */
	while(received_num < 2) {
		/* Ensure we wait to get the packet length,
		 * which requires two bytes, before continuing. */
		received_num = recv(sc->socket, (char *)&buffer[l], \
			(MAX_LEN - received_num), 0);
		l += received_num;

		if(received_num == 0)
			return 2;
		if((received_num < 0) || (received_num >= MAX_LEN) || \
			(l >= MAX_LEN))
			return 0;
	}

	expected_num = ((buffer[1] << 8) | (buffer[0]));
	while(l < expected_num) {
		/* received_num will never exceed MAX_LEN, unless
		 * recv is broken. */
		received_num = recv(sc->socket, (char *)&buffer[l], \
			(MAX_LEN - received_num), 0);
		l += received_num;

		if(received_num == 0)
			return 2;
		if((received_num < 0) || (received_num >= MAX_LEN) || \
			(l >= MAX_LEN))
			return 0;
	} /* l is assumed to have the message length
	   * in the message unpacking code. */
	i = 2; /* Skip the length identifier. */
#else
	/* On Unix, SOCK_SEQPACKET will take care of ensuring message
	 * boundaries are satisfied. */
	l = recv(sc->socket, buffer, MAX_LEN, 0);
	if(l == 0)
		return 2;
	if((l < 0) || (l >= MAX_LEN))
		return 0;
	i = 0; /* No length identifier, so we care about the entire buffer */
#endif

	switch(buffer[i++]) {
		case MESSAGE_GO:
			assert((l - i) == 0);

			return sc->h_go(sc->user);
		case MESSAGE_WRITE: {
			char *name;
			int nchunks;
			unsigned char *chunks;
			unsigned int chunk_index;

			name = (char *)&buffer[i];
			i += strlen(name) + 1;
			assert((i+4) < l);
			chunk_index = buffer[i] | buffer[i+1] << 8 | buffer[i+2] << 16 | buffer[i+3] << 24;
			i += 4;
			nchunks = buffer[i++];
			assert(i + nchunks == l);
			chunks = (unsigned char *)&buffer[i];

			return sc->h_write(name, chunk_index, nchunks, chunks, sc->user);
		}
		case MESSAGE_READ: {
			char *name;
			unsigned int name_index;

			name = (char *)&buffer[i];
			i += strlen(name) + 1;
			assert((i+4) == l);
			name_index = buffer[i] | buffer[i+1] << 8 | buffer[i+2] << 16 | buffer[i+3] << 24;

			return sc->h_read(name, name_index, sc->user);
		}
		default:
			return 0;
	}
}

int ipc_tick(struct ipc_softc *sc)
{
	ssize_t l;

#ifdef _WIN32
	char c[3];

	c[0] = 3;
	c[1] = 0;
	c[2] = MESSAGE_TICK;
	l = send(sc->socket, c, 3, 0);
	if(l != 3)
		return 0;
#else
	char c;

	c = MESSAGE_TICK;
	l = send(sc->socket, &c, 1, 0);
	if(l != 1)
		return 0;
#endif

	return 1;
}

int ipc_read_reply(struct ipc_softc *sc, int nchunks, const unsigned char *chunks)
{
	int len;
	char buffer[MAX_LEN];
	ssize_t l;

#ifdef _WIN32
	len = nchunks + 4;
	assert(len < MAX_LEN);
	assert(nchunks < 256);

	buffer[0] = len & 0xFF;
	buffer[1] = (0xFF00 & len) >> 8;
	buffer[2] = MESSAGE_READ_REPLY;
	buffer[3] = nchunks;
	memcpy(&buffer[4], chunks, nchunks);
#else
	len = nchunks + 2;
	assert(len < MAX_LEN);
	assert(nchunks < 256);

	buffer[0] = MESSAGE_READ_REPLY;
	buffer[1] = nchunks;
	memcpy(&buffer[2], chunks, nchunks);
#endif

	l = send(sc->socket, buffer, len, 0);
	if(l != len)
		return 0;
	return 1;
}

