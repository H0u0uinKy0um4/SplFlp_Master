#pragma once
#include <Arduino.h>
#include <cstdint>
#include <cstddef>
#include <etl/vector.h>
#include "Config.h"
#include "LinkAdapter.h"

// One live record for a Slave Module. The master keeps the last requested
// `flap` separate from the actual flap derived from a trusted position, so a
// drifting open-loop module never silently overwrites what Home Assistant
// asked it to show.
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
    // Drum index derived from currentPos. -1 when the position is unknown or
    // cannot be mapped to a flap.
    int currentFlapIndex = -1;

    // Diagnostics reported by m<id>d (position map) and m<id>l (drum).
    String flapList;
    String positionMap;
    // Application version/bus compat from m<id>s. Empty/0 for slaves that
    // predate version tracking.
    String fwVersion;
    uint8_t fwBusCompat = 0;

    bool online = false;

    // millis() deadline until which this module is assumed to be executing a
    // commanded move and must not be queried: the slave blocks in its step
    // loop and cannot answer (see the slave's PROTOCOL.md, "Motion").
    // 0 = no move pending.
    unsigned long busyUntilMs = 0;
    // Failed queries after the busy window expired. Bounds the retries before
    // the module is declared offline.
    uint8_t busyMisses = 0;
};

// Deep module for every Slave Module effect. Callers submit intent; the RS485
// Bus command grammar, move windows, cached flap, scan slices, query order and
// settled-move readback stay inside. The link sits behind a seam so the same
// interface can be tested with a fake bus and clock.
class ModuleRuntime {
  public:
    explicit ModuleRuntime(LinkAdapter& link);
    void begin();
    void loadEepromModules();

    // Incremental bus scan. scanStep() advances a sweep in short slices so
    // MQTT, the web console and firmware sessions keep running while the bus
    // is walked.
    void startScan(uint16_t from = ID_BEGIN, uint16_t to = ID_END);
    bool scanStep();
    bool isScanning() const;
    // Percentage of ids probed, or -1 when no scan is running.
    int scanProgress() const;

    void updateModules();
    bool refreshModule(uint16_t id);
    const etl::vector<Module, 256>& getModules() const;

    // ---- Slave Module effects ----
    // Each method composes its own RS485 Bus command, records the move window
    // when the action turns the drum, and schedules its own readback.
    bool moveToFlap(uint16_t id, char flap);
    bool moveToIndex(uint16_t id, int index);
    bool moveHome(uint16_t id);
    bool moveSteps(uint16_t id, long steps);
    bool recordIndex(uint16_t id, int index);
    bool applyHomeOffset(uint16_t id, int offset);
    bool clearPositionMap(uint16_t id);
    bool writePosition(uint16_t id, int index, long pos);
    bool writeDrumCharacter(uint16_t id, int index, char flap);
    bool writeDrum(uint16_t id, const String& drum);

    // Bootloader arm and post-session application probe. Binary OTA framing,
    // PING/ACK and link resets stay in Module Firmware Session.
    bool enterBootloader(uint16_t id);
    bool probeApplicationState(uint16_t id);

    // One seam for time-driven work: settled-move readback and a deferred
    // whole-module refresh. Returns true when state changed and the caller
    // should publish a Status Payload. `now` is also the clock for windows
    // opened by effects.
    bool tick(unsigned long now);

    // Restore the last requested character after a firmware session, marking
    // the boot/auto-home window first so the module is not queried while it
    // turns. A move to the character already shown is a no-op on the slave.
    void recoverAfterFirmware(uint8_t id);

  private:
    Module* getModule(uint16_t id);
    bool validId(uint16_t id) const;
    bool sendLine(uint16_t id, const String& body);
    void extendBusy(Module& mod, unsigned long estMs);
    void markFlapMove(uint16_t id, char flap);
    void markIndexMove(uint16_t id, int index);
    void markStepMove(uint16_t id, long pos);
    void markHomeMove(uint16_t id);
    void deferRefresh(unsigned long delayMs);
    long flapTargetPos(const Module& mod, int index) const;
    void deriveActualFlap(Module& mod) const;
    void finishScan();
    bool scanIdFound(uint16_t id) const;
    void markScanIdFound(uint16_t id);
    bool sendQueryAndWait(uint16_t id, uint16_t timeoutMs, Module& outMod);
    void storeEepromId(int id);
    void sortModules();
    bool refreshSettledMoves(unsigned long now);

    LinkAdapter& link;
    unsigned long nowMs = 0;
    etl::vector<Module, 256> modules;
    static constexpr int ADDR_STORED_IDS = 0;
    static constexpr size_t EEPROM_SIZE = 512;

    bool scanning = false;
    uint16_t scanFrom = 0;
    uint16_t scanTo = 0;
    uint16_t scanNext = 0;
    uint16_t scanProbed = 0;
    unsigned long scanStartMs = 0;
    uint8_t scanFoundMask[(ID_END / 8) + 1] = {};

    // Deadline for a refresh postponed behind a blocking move. 0 = none.
    unsigned long refreshDueMs = 0;
};

extern ModuleRuntime moduleRuntime;
