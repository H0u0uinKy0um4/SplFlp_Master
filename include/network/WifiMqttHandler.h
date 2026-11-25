#pragma once

// WiFi/MQTT transport adapter. Protocol decisions live in MasterControl.
void beginWifi();
void setupMqtt();
void handleMqtt();
