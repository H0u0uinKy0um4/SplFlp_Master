#pragma once
#include <Arduino.h>
#include <cstdint>
#include <etl/vector.h>
#include <sys/types.h>
#include "Config.h"


struct Module {
    Module();
    Module(const String& response);

    uint8_t id = 0;
    uint32_t currentPos = 0;
    uint32_t homeOffset = 0;
    uint32_t stepsPerRev = 4096;
    uint8_t status = 0;
    bool autoHome = false;
    bool positionKnown = false;
    char flap = '\0';
    // Drum index derived from currentPos (see Display::deriveActualFlap).
    // -1 when the position is unknown or cannot be mapped.
    int currentFlapIndex = -1;

    // Diagnostics reported by the slave via m<id>d (position map) and m<id>l
    // (64-character drum). Home Assistant displays them on the module entity.
    String flapList;
    String positionMap;
    // Application firmware version/bus compat reported in m<id>s. Empty/0
    // for slaves that predate version tracking.
    String fwVersion;
    uint8_t fwBusCompat = 0;

    bool online = false;

    // millis() deadline until which this module is assumed to be executing a
    // commanded move and must not be queried: the slave blocks in its step
    // loop and cannot answer (see the slave's PROTOCOL.md, "Motion").
    // 0 = no move pending.
    unsigned long busyUntilMs = 0;
    // Queries that failed after the busy window expired. Bounds the retries
    // before the module is declared offline.
    uint8_t busyMisses = 0;
};

class Display
{
  public:
    Display();
    void begin();
    void loadEEpromModules();

    // Incremental bus scan. startScan() begins a sweep that scanStep()
    // advances in short time slices (SCAN_SLICE_MS), so the MQTT loop, web
    // console and OTA keep running while the bus is walked; a blocking scan
    // froze all of them for ~10 s. Commands keep being served during the
    // scan; modules that are mid-move are counted as found (they obviously
    // are on the bus) and are not queried until their window expires.
    void startScan(uint16_t from = ID_BEGIN, uint16_t to = ID_END);
    // Advance the scan by one time slice. Returns false once the scan has
    // finished (also when no scan is running).
    bool scanStep();
    bool isScanning() const;
    // Percentage of ids probed, or -1 when no scan is running.
    int scanProgress() const;

    void updateModules();
    bool refreshModule(uint16_t id);
    bool setModuleChar(uint16_t id, char flap);
    const etl::vector<Module,256>& getModules() const;

    // Record an in-flight move so bus queries for that module are deferred
    // until it should have finished. The estimate is the forward distance at
    // ~1 ms per half-step; a home also pays the seek distance, and unknown
    // positions fall back to one full revolution. Sending is never delayed:
    // the slave queues commands while it turns, so the clock or a playlist
    // can keep streaming at full rate.
    void markMovingFlap(uint16_t id, char flap);
    void markMovingIndex(uint16_t id, int index);
    void markMovingPos(uint16_t id, long pos);
    void markMovingHome(uint16_t id);
    // Re-query one module whose busy window just expired, so its true state
    // reaches Home Assistant without anyone asking. Returns true when a
    // module was refreshed (the caller should publish).
    bool refreshSettledMoves();

  private:
    Module* getModule(uint16_t id);
    void extendBusy(Module& mod, unsigned long estMs);
    long flapTargetPos(const Module& mod, int index) const;
    // Map the reported step position back to a drum index and flap, so the
    // published flap is the module's real state instead of the last requested
    // character. Only trusted while the module reports position_known.
    void deriveActualFlap(Module& mod) const;
    void finishScan();
    bool scanIdFound(uint16_t id) const;
    void markScanIdFound(uint16_t id);
    bool sendQueryAndWait(uint16_t id, uint16_t timeoutMs, Module& outMod);
    void storeEEpromId(int id);
    void sortModules();

    etl::vector<Module,256> modules;
    static constexpr int ADDR_STORED_IDS = 0;
    static constexpr size_t EEPROM_SIZE = 512;

    // Incremental scan state. scanFoundMask is a bit per bus id (1..255).
    bool scanning = false;
    uint16_t scanFrom = 0;
    uint16_t scanTo = 0;
    uint16_t scanNext = 0;
    uint16_t scanProbed = 0;
    unsigned long scanStartMs = 0;
    uint8_t scanFoundMask[(ID_END / 8) + 1] = {};

};

extern Display splitflap;