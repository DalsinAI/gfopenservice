/* ACMedia's messages (acm_msg.c). MIT, Copyright (c) 2026 AmiCradle Team. */
#ifndef ACM_MSG_H
#define ACM_MSG_H
#include "acm_proto.h"
/* One message: the header, a then b (either may be empty), and fds. 0 or -1. */
int acm_send(int sock, uint32_t type, const void *a, uint32_t alen, const void *b, uint32_t blen,
             const int *fds, int nfds);
/* 1 with a message, 0 when the other side has closed, -1 on a bad one. */
int acm_recv(int sock, uint32_t *type, void *buf, uint32_t cap, uint32_t *len, int *fds, int *nfds);
#endif
