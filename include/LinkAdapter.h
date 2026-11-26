#pragma once

#include <Arduino.h>

// The seam at which Module Runtime reaches the RS485 Bus. Two adapters earn
// it: the thin adapter over the real Rs485Link free functions, and the fake
// used by the native tests. Callers never see command bytes.
class LinkAdapter {
  public:
    virtual ~LinkAdapter() = default;
    virtual void sendLine(const String& line) = 0;
    virtual String readLine(bool trim = true) = 0;
    virtual bool rxBusy() = 0;
};
