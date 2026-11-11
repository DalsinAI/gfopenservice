/*
 * mitchy.device: hand work to a named service. Mitchy is the services card
 * (named after the dog whose paw print is inside the A1000's case): it goes
 * off, fetches what was asked for and brings it back.
 *
 * The API is devices/gfopenservice.h's, which this includes: only the device's
 * name is new. gfopenservice.device stays, as a small device that opens this
 * one, so programs built with the old name go on working.
 *
 * MIT, Copyright (c) 2026 AmiCradle Team.
 */
#ifndef DEVICES_MITCHY_H
#define DEVICES_MITCHY_H

#include <devices/gfopenservice.h>

#define MITCHY_NAME "mitchy.device"

#endif
