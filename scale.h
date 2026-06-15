#pragma once
#include <Arduino.h>
#include "HX711.h"       // bogde/HX711 library
#include "config.h"

// ============================================================
//  ScaleManager - HX711 load cell interface
//  Handles tare, calibration, filtering, and cumulative tracking
// ============================================================

class ScaleManager {
public:
    ScaleManager() : _offset_kg(0.0f), _cal_factor(SCALE_CAL_F),
                     _last_raw(0.0f), _cumulative_kg(0.0f),
                     _step_base_kg(0.0f) {}

    void begin(float calFactor) {
        _cal_factor = calFactor;
        scale.begin(SCALE_DOUT, SCALE_SCK);
        scale.set_scale(_cal_factor);
        scale.tare(10);
    }

    void setCalFactor(float cal) {
        _cal_factor = cal;
        scale.set_scale(cal);
    }

    float getCalFactor() const { return _cal_factor; }

    // Tare the scale (zero it)
    void tare() {
        scale.tare(10);
        _offset_kg = 0.0f;
    }

    // Raw reading in kg (instantaneous, no filter)
    float readRawKg() {
        if (!scale.is_ready()) return _last_raw;
        float r = scale.get_units(3);   // average 3 readings
        _last_raw = r;
        return r;
    }

    // Smoothed reading using EMA filter
    float readFilteredKg() {
        float raw = readRawKg();
        _filtered = _filtered * 0.85f + raw * 0.15f;
        return _filtered;
    }

    // Mark the base weight at start of a new ingredient step
    void markStepStart() {
        _step_base_kg = readFilteredKg();
    }

    // How much has been loaded since markStepStart()
    float loadedSinceStepStart() {
        float now = readFilteredKg();
        float delta = now - _step_base_kg;
        return delta < 0.0f ? 0.0f : delta;
    }

    // Total wagon weight right now
    float totalWagonKg() {
        return readFilteredKg();
    }

    // Calibration: place known weight, call this
    void calibrate(float knownKg) {
        if (!scale.is_ready()) return;
        float reading = scale.get_units(20);
        if (knownKg > 0.01f && reading > 0.01f) {
            _cal_factor = reading / knownKg * _cal_factor;
            scale.set_scale(_cal_factor);
        }
    }

    bool isReady() { return scale.is_ready(); }

    // Simulate mode for testing without load cell
    void enableSimMode(bool en) { _simMode = en; _simKg = 0.0f; }
    void simAddKg(float kg)    { if (_simMode) _simKg += kg; }

private:
    HX711 scale;
    float _cal_factor;
    float _offset_kg;
    float _last_raw;
    float _filtered = 0.0f;
    float _cumulative_kg;
    float _step_base_kg;
    bool  _simMode = false;
    float _simKg   = 0.0f;
};
