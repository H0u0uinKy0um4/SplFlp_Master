#pragma once

void connectWifi();
void setupMqtt();
void handleMqtt();
void publishMqttResponse(const char* message);
void publishDiscovery();
void publishStatus();