#pragma once

// Apply before loading user settings.
void ApplySkate3AndroidDefaults();

// True on the Mali-G52 / two-fast-core class of phone (MediaTek Helio G70, G80
// and G85, for example the Redmi 9). Those devices get conservative defaults:
// see docs/REDMI9_ANDROID12.md. Always false when not building for Android.
bool Skate3AndroidIsWeakMaliDevice();

// Opt-in switches for device testing. Reads the Android system property
// debug.skate3.<name> and returns true when it is "1", so a tester can turn a
// diagnostic on with `adb shell setprop` and no rebuild. Always false when not
// building for Android.
bool Skate3AndroidDebugFlag(const char* name);
