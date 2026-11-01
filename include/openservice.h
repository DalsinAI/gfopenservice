/*
 * openservice: hand work to a named service, on the service card or on a
 * Cradle in the nursery (the LAN), or do it here when neither is there.
 * MIT, Copyright (c) 2026 AmiCradle Team.
 */
#ifndef OPENSERVICE_H
#define OPENSERVICE_H

/* Boards with the services block (ACSV at their base): AmiCradle Team boards of any
 * product. The wire is the project's design/SERVICES_CARD.md (v1); the
 * emulator side is AC090's. */
#define OPENSERVICE_MANUFACTURER     0xDA15   /* AmiCradle Team */

/* The CLASS register ($20): what kind of board it is. Only for showing;
 * each board's directory says what it offers. */
#define OPENSERVICE_CLASS_SERVICES 1
#define OPENSERVICE_CLASS_CORES    2
#define OPENSERVICE_CLASS_FPU      3
#define OPENSERVICE_CLASS_TPU      4

/* The first longword at the board's base. */
#define OPENSERVICE_MAGIC 0x41435356UL        /* "ACSV" */

/* The Cradles this Amiga has paired with, one per line: the Cradle's
 * fingerprint (its mDNS fp=), then, when paired with a code, the key in hex
 * and its name. A line with the fingerprint alone allows unsealed frames
 * (test Cradles started with --allow-plain). */
#define OPENSERVICE_PAIRED_FILE "ENV:GFOpenService/Paired"
#define OPENSERVICE_PAIRED_KEEP "ENVARC:GFOpenService/Paired"

/* This Amiga's identity, 16 bytes in hex, made by the first pairing. */
#define OPENSERVICE_ID_FILE "ENV:GFOpenService/ID"
#define OPENSERVICE_ID_KEEP "ENVARC:GFOpenService/ID"

/* 1 when this fingerprint is in OPENSERVICE_PAIRED_FILE. */
int openservice_paired(const char *fingerprint);

/* 1 with the key when it was paired with a code. */
int openservice_psk(const char *fingerprint, unsigned char key[32]);

int openservice_amiga_id(unsigned char id[16]);
int openservice_hex(const char *text, unsigned char *out, int length);

#endif
