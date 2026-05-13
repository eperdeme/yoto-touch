#pragma once
// yoto_api.h — Direct Yoto cloud API client for ESP32.
// Handles OAuth device-code auth, library fetch, cover download, player control.
// All functions are blocking (intended to run on the net task, not the UI loop).

#include <Arduino.h>
#include <vector>

// ---------- Auth ----------
// Returns true if we have valid tokens in NVS.
bool yotoAuthReady();

// Start device code flow. Returns user_code and verification_uri for display.
struct DeviceCodeResult {
    String userCode;
    String verificationUri;
    String verificationUriComplete;
    String deviceCode;
    int interval;       // poll interval in seconds
    int expiresIn;      // seconds until code expires
    bool ok;
};
DeviceCodeResult yotoStartDeviceFlow();

// Poll for token completion. Returns "authorized", "pending", or "error".
String yotoPollDeviceFlow(const String &deviceCode);

// Refresh access token if expired. Returns true on success.
bool yotoEnsureToken();

// Get current access token (caller must call yotoEnsureToken first).
String yotoGetAccessToken();

// Forget stored tokens (sign out).
void yotoClearTokens();

// ---------- Devices ----------
struct YotoDevice {
    String deviceId;
    String name;
    bool online;
};
bool yotoListDevices(std::vector<YotoDevice> &out);

// Get or cache the first device ID.
String yotoGetDeviceId();

// ---------- Library ----------
struct YotoCard {
    String cardId;
    String title;
    String author;
    String coverUrl;    // Yoto CDN cover URL
    String description;
    String category;
    String language;
    String shareType;
    String series;
    int sequenceNumber; // -1 if unset
    int duration;       // seconds, 0 if unset
};
bool yotoFetchLibrary(std::vector<YotoCard> &out);

// ---------- Card detail ----------
struct YotoCardDetail {
    String description;
    int chapterCount;
    int trackCount;
    String narrator;
    String genre;
    String author;
    String series;
    String language;
    int sequenceNumber;
    int duration;
};
bool yotoFetchCardDetail(const String &cardId, YotoCardDetail &out);

// ---------- Player control ----------
bool yotoPlayCard(const String &cardId);
bool yotoPause();
bool yotoResume();
bool yotoStop();
bool yotoSetVolume(int vol);

// ---------- Cover download ----------
// Download PNG cover and decode to a newly allocated RGB565 buffer.
// Caller owns *outRgb565 and must free it with heap_caps_free().
bool yotoDownloadCover(const String &url, uint8_t **outRgb565, int thumbSize);

// ---------- Init ----------
void yotoApiInit();
