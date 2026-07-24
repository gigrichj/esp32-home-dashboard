#include "spacex_launch_service.h"
#include "wifi_manager.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <WiFi.h>
#include <time.h>
#include <JPEGDEC.h>

SpacexLaunch g_spacexLaunches[SPACEX_MAX_LAUNCHES];
int g_spacexLaunchCount = 0;
bool g_spacexValid = false;
int g_spacexLastHttpCode = -999;

uint16_t* g_spacexImagePixels = nullptr;
int g_spacexImageWidth = 0;
int g_spacexImageHeight = 0;
bool g_spacexImageValid = false;

bool g_spacexLandingValid = false;
bool g_spacexLandingAttempt = false;
String g_spacexLandingLocation = "";
String g_spacexLandingAbbrev = "";
String g_spacexLandingType = "";

// Same jpegDrawCallback/s_decodeTarget pattern as aviation_service.cpp's
// fetchAndDecodePhoto() -- file-scoped statics here don't collide with
// that file's own copy, so this is a straightforward duplication rather
// than a shared/refactored helper.
static uint16_t* s_decodeTarget = nullptr;
static int s_decodeTargetW = 0;
static int s_decodeTargetH = 0;

static int jpegDrawCallback(JPEGDRAW *pDraw) {
  if (s_decodeTarget == nullptr) return 0;
  for (int row = 0; row < pDraw->iHeight; row++) {
    int destY = pDraw->y + row;
    if (destY < 0 || destY >= s_decodeTargetH) continue;
    uint16_t *destRow = s_decodeTarget + (size_t)destY * s_decodeTargetW;
    const uint16_t *srcRow = pDraw->pPixels + (size_t)row * pDraw->iWidth;
    for (int col = 0; col < pDraw->iWidth; col++) {
      int destX = pDraw->x + col;
      if (destX < 0 || destX >= s_decodeTargetW) continue;
      destRow[destX] = srcRow[col];
    }
  }
  return 1;
}

// Same yield-safe manual read loop used throughout this project -- a
// plain http.getString() risks the same FreeRTOS watchdog crash already
// fixed elsewhere (aviation, air quality, weather).
static bool readHttpBodySafely(HTTPClient& http, String& outPayload, const char* sourceName) {
  int payloadLen = http.getSize();
  int bufSize = (payloadLen > 0) ? payloadLen + 1 : 65536;
  char *rawBuf = (char *)heap_caps_malloc(bufSize, MALLOC_CAP_8BIT);
  if (rawBuf == nullptr) {
    Serial.printf("[SpaceX] %s payload buffer alloc failed\n", sourceName);
    return false;
  }

  WiFiClient *stream = http.getStreamPtr();
  size_t readTotal = 0;
  uint32_t startMs = millis();
  bool readError = false;
  while (readTotal < (size_t)(bufSize - 1) && millis() - startMs < 15000) {
    if (!http.connected() && stream->available() == 0) break;
    size_t avail = stream->available();
    if (avail > 0) {
      int toRead = (int)min(avail, (size_t)(bufSize - 1 - readTotal));
      int r = stream->readBytes(rawBuf + readTotal, toRead);
      if (r <= 0) { readError = true; break; }
      readTotal += r;
    } else {
      vTaskDelay(pdMS_TO_TICKS(5)); // yield -- the critical fix
    }
  }
  rawBuf[readTotal] = '\0';

  if (readError) {
    Serial.printf("[SpaceX] %s payload read error\n", sourceName);
    free(rawBuf);
    return false;
  }

  outPayload = String(rawBuf);
  free(rawBuf);
  return true;
}

// Same "days from civil" UTC conversion duplicated per-file elsewhere in
// this project (this toolchain has no timegm(), and mktime() assumes
// local time). Launch times need minute precision (unlike the
// hourly-only weather data this pattern was originally used for), so
// minutes are folded in directly rather than added as a separate step.
static uint32_t utcToUnix(int year, int month, int day, int hour, int minute) {
  int y = year;
  int m = month;
  y -= m <= 2;
  long era = (y >= 0 ? y : y - 399) / 400;
  int yoe = (int)(y - era * 400);
  int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  long daysSinceEpoch = era * 146097L + doe - 719468L;
  return (uint32_t)(daysSinceEpoch * 86400L + (long)hour * 3600L + (long)minute * 60L);
}

void spacex_launch_service_update() {
  if (!wifi_manager_is_connected()) return;

  HTTPClient http;
  const char* url = "https://ll.thespacedevs.com/2.0.0/launch/upcoming/?lsp__name=SpaceX&limit=15";
  http.begin(url);
  // Same useHTTP10 fix as every other manual-read-loop JSON fetch in this
  // project -- without it, a chunked-transfer-encoding response corrupts
  // the raw stream read.
  http.useHTTP10(true);
  int code = http.GET();
  g_spacexLastHttpCode = code;
  if (code != 200) {
    Serial.printf("[SpaceX] HTTP %d\n", code);
    http.end();
    return;
  }

  String payload;
  if (!readHttpBodySafely(http, payload, "SpaceX")) {
    http.end();
    return;
  }
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.printf("[SpaceX] JSON parse error: %s\n", err.c_str());
    return;
  }

  JsonArray results = doc["results"].as<JsonArray>();
  time_t now = time(nullptr);
  uint32_t cutoffUnix = (now > 100000) ? (uint32_t)now + (30UL * 86400UL) : 0xFFFFFFFF;

  g_spacexLaunchCount = 0;
  for (JsonObject launch : results) {
    if (g_spacexLaunchCount >= SPACEX_MAX_LAUNCHES) break;

    String netStr = launch["net"] | "";
    int yr, mo, dy, hr, mi;
    uint32_t netUnix = 0;
    if (sscanf(netStr.c_str(), "%d-%d-%dT%d:%d", &yr, &mo, &dy, &hr, &mi) == 5) {
      netUnix = utcToUnix(yr, mo, dy, hr, mi);
    }

    // Skip anything beyond 30 days out -- the API returns launches in
    // ascending date order already, so this naturally trims the tail.
    if (netUnix == 0 || netUnix > cutoffUnix) continue;

    SpacexLaunch& out = g_spacexLaunches[g_spacexLaunchCount];
    out.displayName = launch["name"] | "";
    out.rocketName = launch["rocket"]["configuration"]["name"] | "";
    out.missionName = launch["mission"]["name"] | "";
    out.padName = launch["pad"]["name"] | "";
    out.locationName = launch["pad"]["location"]["name"] | "";
    out.statusName = launch["status"]["name"] | "";
    out.imageUrl = launch["image"] | "";
    out.launchId = launch["id"] | "";
    out.netUnix = netUnix;
    g_spacexLaunchCount++;
  }
  g_spacexValid = true;
}

// Mirrors aviation_service.cpp's fetchAndDecodePhoto() pattern -- fetches
// raw JPEG bytes directly into a PSRAM buffer (not through readHttpBodySafely,
// since that returns a null-terminated String, not appropriate for binary
// JPEG data) and decodes via JPEGDEC into an RGB565 pixel buffer.
void spacex_fetch_next_image() {
  if (!wifi_manager_is_connected()) return;
  if (g_spacexLaunchCount == 0) return;
  String url = g_spacexLaunches[0].imageUrl;
  if (url.length() == 0) return;

  HTTPClient http;
  http.begin(url);
  int code = http.GET();
  if (code != 200) {
    Serial.printf("[SpaceX] image fetch HTTP %d\n", code);
    http.end();
    return;
  }

  int len = http.getSize();
  if (len <= 0 || len > 250000) {
    Serial.printf("[SpaceX] image size invalid: %d\n", len);
    http.end();
    return;
  }

  uint8_t *jpegBuf = (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
  if (jpegBuf == nullptr) {
    Serial.println("[SpaceX] image buffer alloc failed");
    http.end();
    return;
  }

  WiFiClient *stream = http.getStreamPtr();
  size_t readTotal = 0;
  uint32_t startMs = millis();
  bool readError = false;
  while (http.connected() && readTotal < (size_t)len && millis() - startMs < 15000) {
    size_t avail = stream->available();
    if (avail > 0) {
      int toRead = (int)min((size_t)avail, (size_t)len - readTotal);
      int r = stream->readBytes(jpegBuf + readTotal, toRead);
      if (r <= 0) { readError = true; break; }
      readTotal += r;
    } else {
      vTaskDelay(pdMS_TO_TICKS(5)); // yield -- same critical fix used throughout this project
    }
  }
  http.end();

  if (readError || readTotal == 0) {
    Serial.println("[SpaceX] image payload read error");
    free(jpegBuf);
    return;
  }

  JPEGDEC jpeg;
  if (jpeg.openRAM(jpegBuf, (int)readTotal, jpegDrawCallback)) {
    int w = jpeg.getWidth();
    int h = jpeg.getHeight();

    if (g_spacexImagePixels != nullptr) {
      free(g_spacexImagePixels);
      g_spacexImagePixels = nullptr;
      g_spacexImageValid = false;
    }

    uint16_t *photoBuf = (uint16_t *)heap_caps_malloc((size_t)w * h * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (photoBuf != nullptr) {
      s_decodeTarget = photoBuf;
      s_decodeTargetW = w;
      s_decodeTargetH = h;
      jpeg.decode(0, 0, 0);
      g_spacexImagePixels = photoBuf;
      g_spacexImageWidth = w;
      g_spacexImageHeight = h;
      g_spacexImageValid = true;
      Serial.printf("[SpaceX] image decoded %dx%d\n", w, h);
    } else {
      Serial.println("[SpaceX] image pixel buffer alloc failed");
    }
    jpeg.close();
  } else {
    Serial.println("[SpaceX] JPEG openRAM failed");
  }

  free(jpegBuf);
}

// Booster landing info for the next launch -- only available via LL2's
// single-launch detail endpoint (not the list/upcoming endpoint used
// above), which returns a much larger response (full agency/rocket/program
// descriptions we don't need, alongside the landing info we do). Reuses
// readHttpBodySafely() since this is JSON text, not binary like the image.
void spacex_fetch_next_landing_info() {
  if (!wifi_manager_is_connected()) return;
  if (g_spacexLaunchCount == 0) return;
  String launchId = g_spacexLaunches[0].launchId;
  if (launchId.length() == 0) return;

  HTTPClient http;
  String url = "https://ll.thespacedevs.com/2.0.0/launch/" + launchId + "/";
  http.begin(url);
  http.useHTTP10(true);
  int code = http.GET();
  if (code != 200) {
    Serial.printf("[SpaceX] landing detail HTTP %d\n", code);
    http.end();
    return;
  }

  String payload;
  if (!readHttpBodySafely(http, payload, "SpaceX landing detail")) {
    http.end();
    return;
  }
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.printf("[SpaceX] landing detail JSON parse error: %s\n", err.c_str());
    return;
  }

  // rocket.launcher_stage is an array; landing info (if any) lives on the
  // first stage's entry. Absent entirely for launches with no landing
  // attempt planned (e.g. expendable missions), hence the size() check.
  JsonArray stages = doc["rocket"]["launcher_stage"].as<JsonArray>();
  if (stages.size() == 0) {
    g_spacexLandingAttempt = false;
    g_spacexLandingValid = true;
    return;
  }

  JsonObject landing = stages[0]["landing"];
  g_spacexLandingAttempt = landing["attempt"] | false;
  g_spacexLandingLocation = landing["location"]["name"] | "";
  g_spacexLandingAbbrev = landing["location"]["abbrev"] | "";
  g_spacexLandingType = landing["type"]["abbrev"] | "";
  g_spacexLandingValid = true;
}

