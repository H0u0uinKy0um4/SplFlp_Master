#pragma once
#include <Arduino.h>
#include <cstdint>
#include <etl/vector.h>
#include <sys/types.h>
#include "Config.h"


struct Module {
    Module();
    Module(const String& response);
    void changeHomeOffset(int id);
    void changeStepsPerRev(int steps);
    void addMapPos(int index, int pos);
    void changeFlap(int index, char flap);
    void eraseMapPos(int index);
    void changeId(int id);

    uint8_t id = 0;
    uint32_t currentPos = 0;
    uint32_t homeOffset = 0;
    uint32_t stepsPerRev = 4096;
    uint8_t status = 0;
    bool autoHome = false;
    char flap = '\0';

    // Diagnostics reported by the slave via m<id>d (position map) and m<id>l
    // (64-character drum). Home Assistant displays them on the module entity.
    String flapList;
    String positionMap;

    bool online = false;
    unsigned long lastSeen = 0;
};

class Display
{
  public:
    Display();
    void begin();
    size_t discoverModules(uint16_t from=ID_BEGIN,uint16_t to=ID_END);
    void loadEEpromModules();

    void updateModules();
    bool setModuleChar(uint16_t id, char flap);
    const etl::vector<Module,256>& getModules() const;



  private:
    Module* getModule(uint16_t id);
    bool sendQueryAndWait(uint16_t id, uint16_t timeoutMs, Module& outMod);
    void storeEEpromId(int id);
    void sortModules();

    etl::vector<Module,256> modules;
    static constexpr int ADDR_STORED_IDS = 0;
    static constexpr size_t EEPROM_SIZE = 512;

};

extern Display splitflap;