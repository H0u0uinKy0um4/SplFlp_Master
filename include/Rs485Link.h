#pragma once

#include <Arduino.h>
#include <cstddef>

// The one owner of the half-duplex RS485 bus. ASCII command lines and binary
// OTA frames share its DE timing, echo suppression and console trace.
void rs485Begin();
void rs485SendLine(const String& line);
String rs485ReadLine(bool trim = true);
void rs485Reset();
bool rs485RxBusy();

void rs485SendFrame(const uint8_t* frame, size_t length);
size_t rs485Available();
int rs485ReadByte();
