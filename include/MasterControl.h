#pragma once
#include <Arduino.h>

// Fixed topic names shared by the MQTT adapter and the control module.
namespace masterTopics {
static constexpr char STATUS[] = "splitflap/status";
static constexpr char DISPLAY_TOPIC[] = "splitflap/command/display";
static constexpr char SYNC_TOPIC[] = "splitflap/command/sync";
static constexpr char SCAN_TOPIC[] = "splitflap/command/scan";
static constexpr char UPDATE_TOPIC[] = "splitflap/command/update";
static constexpr char CALIBRATE_TOPIC[] = "splitflap/command/calibrate";
static constexpr char FIRMWARE_TOPIC[] = "splitflap/command/firmware";
static constexpr char MASTER_FIRMWARE_TOPIC[] =
    "splitflap/command/master_firmware";
}  // namespace masterTopics

// Deep module for MQTT control. Command interpretation, bus effects, scan and
// heartbeat timing, and Status Payload rendering live here. The WiFi/MQTT
// adapter only provides transport hooks.
class MasterControl {
  public:
    using PublishHook = bool (*)(const String&, const String&, bool);
    using PumpHook = void (*)();

    void begin();
    void setPublisher(PublishHook publisher) { publisher_ = publisher; }
    void setPump(PumpHook pump) { pump_ = pump; }
    void pump() { if (pump_) pump_(); }
    void setLocalIp(const String& ip) { localIp_ = ip; }

    // PubSubClient cannot publish from inside its callback. The MQTT adapter
    // brackets onCommand() with these, and the single resulting status publish
    // is flushed after client.loop() returns.
    void beginCommand() { inCommand_ = true; }
    void endCommand();
    void onCommand(const String& topic, const String& message);
    void publishStatus();
    // Drive scan slices, runtime state and the heartbeat. Returns after one
    // unit of work so WiFi, OTA and the console keep running.
    void tick();

  private:
    void handleDisplay(const String& message);
    void handleCalibrate(const String& message);
    bool busAvailable(const char* commandName);

    String currentText;
    String localIp_;
    unsigned long lastStatusPublish = 0;
    unsigned long lastScanPublish = 0;
    PublishHook publisher_ = nullptr;
    PumpHook pump_ = nullptr;
    bool inCommand_ = false;
    bool publishPending_ = false;
};

extern MasterControl masterControl;
