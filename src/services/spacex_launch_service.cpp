#include "spacex_launch_service.h"
#include "wifi_manager.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <time.h>

SpacexLaunch g_spacexLaunches[SPACEX_MAX_LAUNCHES];
int g_spacexLaunchCount = 0;
bool g_spacexValid = false;
int g_spacexLastHttpCode = -999;

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
    out.netUnix = netUnix;
    g_spacexLaunchCount++;
  }
  g_spacexValid = true;
}
