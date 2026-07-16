#pragma once
#include <Arduino.h>

void wifi_manager_begin();
void wifi_manager_loop();
bool wifi_manager_is_connected();

bool wifi_manager_in_setup_mode();

String wifi_manager_last_attempted_ssid();
int wifi_manager_last_status_code();
