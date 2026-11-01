/*
 * opentls on the Amiga (opentls_amiga.c). Call opentls_amiga_open() once
 * AmiSSL is open: when a board in this machine or a paired Cradle offers
 * opentls.key/1, the key maths of every TLS handshake goes there. It returns
 * where (OSWHERE_CARD or OSWHERE_LAN), or 0 when nothing offers it and
 * AmiSSL keeps doing everything itself.
 * MIT, Copyright (c) 2026 AmiCradle Team.
 */
#ifndef OPENTLS_AMIGA_H
#define OPENTLS_AMIGA_H

int opentls_amiga_open(void);
void opentls_amiga_close(void);

#endif
