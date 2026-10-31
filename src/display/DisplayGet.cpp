#include "Display.h"
#include "RS485.h"
#include "WString.h"
#include <Arduino.h>
#include <cstdint>
#include "web/WebSerialConsole.h"
#include "Config.h"

Module* Display::getModule(uint16_t id) {
    for (auto& mod : modules) {
        if (mod.id == id) return &mod;
    }
    return nullptr;
}
const etl::vector<Module,256>& Display::getModules() const
{
      return modules;
}