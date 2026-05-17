#pragma once
#include <Arduino.h>

// ── Voltage reading struct ────────────────────────────────────
struct Readings {
    float gridVoltage   = 0.0f;
    float backupVoltage = 0.0f;
    float battVoltage   = 0.0f;
    bool  gridOk        = false;
    bool  backupOk      = false;
    int   battPercent   = 0;
    unsigned long uptime = 0;
};

// ── Function declarations ─────────────────────────────────────
void    sensorsInit();
void    sensorsUpdate(Readings& r);
float   readAcRms(int pin, float cal);
float   readBatteryVoltage();
int     batteryPercent(float v);
