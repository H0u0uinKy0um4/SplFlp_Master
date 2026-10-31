#pragma once
#include <Arduino.h>

void setupRS485();
void rs485Send(const String &msg);
String rs485read(bool trim = true);
void rs485Reset();
bool rs485RxBusy();
