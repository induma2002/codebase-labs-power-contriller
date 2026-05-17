#include "sensors.h"
#include "config.h"

namespace {
float applyEma(float previous, float current, float alpha) {
    return (previous * alpha) + (current * (1.0f - alpha));
}
}

void sensorsInit() {
    analogReadResolution(12);           // 0 – 4095
    analogSetAttenuation(ADC_11db);     // 0 – 3.3 V range
}

// ─────────────────────────────────────────────────────────────
//  readAcRms()
//
//  Samples the ZMPT101B output for exactly 20 ms (one full
//  50 Hz cycle at 500 samples → one sample every 40 µs).
//
//  The module output is centred at VCC/2 ≈ 1.65 V (DC offset).
//  We subtract the running mean from every sample so only the
//  AC swing remains, then compute RMS and scale by CAL.
//
//  CAL is determined by comparing the ESP32 reading to a real
//  multimeter on the same socket — see config.h for details.
// ─────────────────────────────────────────────────────────────
float readAcRms(int pin, float cal) {
    const int   N          = 500;
    const int   PERIOD_US  = 20000 / N;   // 40 µs
    long        buf[N];
    long        sumRaw     = 0;

    // 1. Collect samples
    for (int i = 0; i < N; i++) {
        buf[i]  = analogRead(pin);
        sumRaw += buf[i];
        delayMicroseconds(PERIOD_US);
    }

    // 2. DC offset (mean)
    float mean = (float)sumRaw / N;

    // 3. RMS of AC component
    float sumSq = 0.0f;
    for (int i = 0; i < N; i++) {
        float v = (float)buf[i] - mean;
        sumSq  += v * v;
    }
    float rmsAdc = sqrtf(sumSq / N);

    // 4. Convert ADC counts → real volts and apply calibration
    //    rmsAdc counts × (3.3 V / 4095 counts) × CAL
    return rmsAdc * (3.3f / 4095.0f) * cal;
}

// ─────────────────────────────────────────────────────────────
//  readBatteryVoltage()
//
//  Averages 64 ADC samples from the 100 kΩ / 22 kΩ divider.
//  adcVolts × (1 / BATT_RATIO) = battery terminal voltage.
// ─────────────────────────────────────────────────────────────
float readBatteryVoltage() {
    long sum = 0;
    for (int i = 0; i < 64; i++) {
        sum += analogRead(PIN_BATTERY);
        delayMicroseconds(200);
    }
    float adcVolts = (sum / 64.0f) * (3.3f / 4095.0f);
    return (adcVolts / BATT_RATIO) * BATTERY_CAL;
}

// ─────────────────────────────────────────────────────────────
//  batteryPercent()
//
//  Resting open-circuit voltage curve for 12 V SLA / AGM.
//  These numbers assume no load. Apply your own correction
//  factor if the UPS is actively supplying current.
// ─────────────────────────────────────────────────────────────
int batteryPercent(float v) {
    if (v >= 12.70f) return 100;
    if (v >= 12.50f) return (int)map((long)(v * 100), 1250, 1270,  75, 100);
    if (v >= 12.40f) return (int)map((long)(v * 100), 1240, 1250,  50,  75);
    if (v >= 12.20f) return (int)map((long)(v * 100), 1220, 1240,  25,  50);
    if (v >= 11.90f) return (int)map((long)(v * 100), 1190, 1220,   0,  25);
    return 0;
}

// ─────────────────────────────────────────────────────────────
//  sensorsUpdate()   — call every SENSOR_INTERVAL_MS
// ─────────────────────────────────────────────────────────────
void sensorsUpdate(Readings& r) {
    static bool filtersPrimed = false;

    const float rawGridVoltage   = readAcRms(PIN_GRID_AC,   GRID_CAL);
    const float rawBackupVoltage = readAcRms(PIN_BACKUP_AC, BACKUP_CAL);
    const float rawBattVoltage   = readBatteryVoltage();

    if (!filtersPrimed) {
        r.gridVoltage   = rawGridVoltage;
        r.backupVoltage = rawBackupVoltage;
        r.battVoltage   = rawBattVoltage;
        filtersPrimed   = true;
    } else {
        r.gridVoltage   = applyEma(r.gridVoltage,   rawGridVoltage,   AC_SMOOTHING_ALPHA);
        r.backupVoltage = applyEma(r.backupVoltage, rawBackupVoltage, AC_SMOOTHING_ALPHA);
        r.battVoltage   = applyEma(r.battVoltage,   rawBattVoltage,   BATT_SMOOTHING_ALPHA);
    }

    r.gridOk        = r.gridVoltage   > AC_OK_VOLTS;
    r.backupOk      = r.backupVoltage > AC_OK_VOLTS;
    r.battPercent   = batteryPercent(r.battVoltage);
    r.uptime        = millis() / 1000;
}
