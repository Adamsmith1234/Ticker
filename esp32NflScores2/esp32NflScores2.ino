/*
  Sports + Stocks + Phrases + Weather Display - ESP32
*/

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <FastLED.h>
#include <FastLED_NeoMatrix.h>
#include <Adafruit_GFX.h>
#include <ArduinoJson.h>
#include <WiFiManager.h> // 
#include <ESPmDNS.h>
#include <esp_system.h>
#include <lwip/dns.h>      // for forcing IPv4 DNS servers
#include <Preferences.h>   // saves the sports filter across reboots
#include <version.h>

/* ================= CONFIG ================= */

#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>

// Increase this number every time you push a new update to GitHub
const int currentVersion = FIRMWARE_VERSION; 

// Replace with your GitHub Username and Repo name
const String baseUrl = "https://raw.githubusercontent.com/Adamsmith1234/Ticker/main/";
const String versionUrl = baseUrl + "version.txt";
const String binaryUrl  = baseUrl + "firmware.bin";

// Cloudflare Worker that returns the multi-league scores JSON
const char *SPORTS_HOST = "sportscraper.adamjsmith002.workers.dev";
const char *SPORTS_URL  = "https://sportscraper.adamjsmith002.workers.dev/";

enum DisplayMode { MODE_SPORTS, MODE_STOCKS, MODE_PHRASES, MODE_WEATHER, MODE_CYCLE, MODE_FIREPLACE };
volatile DisplayMode currentMode = MODE_CYCLE;

// Forward declarations (needed outside the Arduino IDE's auto-prototyping)
void addDebugLog(const String &message);

/* ================= NETWORK HELPERS ================= */

// Force public IPv4 DNS servers. Some routers hand out an IPv6-only DNS
// address, which the ESP32 can't use reliably, so lookups time out and
// HTTPClient returns -1. DHCP lease renewals can overwrite these, so this is
// called before every fetch (cheap) as well as after (re)connecting.
void forceDNS() {
  ip_addr_t d0, d1;
  IP_ADDR4(&d0, 1, 1, 1, 1);
  IP_ADDR4(&d1, 8, 8, 8, 8);
  dns_setserver(0, &d0);
  dns_setserver(1, &d1);
}

// A name that never changes, even if the router hands out a new IP:
//   http://ticker.local
const char *MDNS_NAME = "ticker";

void startMDNS() {
  MDNS.end(); // safe if it was never started; lets us restart cleanly after a reconnect
  if (MDNS.begin(MDNS_NAME)) {
    MDNS.addService("http", "tcp", 80);
    addDebugLog(String("mDNS started: http://") + MDNS_NAME + ".local");
  } else {
    addDebugLog("mDNS failed to start");
  }
}

// Counts consecutive network-level failures (HTTP code <= 0) across all fetches
int consecutiveFailures = 0;

void noteFetchSuccess() {
  consecutiveFailures = 0;
}

void logDnsTest(const char *host) {
  IPAddress ip;
  int ok = WiFi.hostByName(host, ip);
  addDebugLog(String("DNS test ") + host + ": " + (ok == 1 ? ip.toString() : String("FAILED")));
  addDebugLog(String("Largest free block: ") + ESP.getMaxAllocHeap());
}

void noteFetchFailure(const char *source) {
  consecutiveFailures++;
  addDebugLog(String(source) + " network failure #" + consecutiveFailures);

  if (consecutiveFailures >= 6) {
    addDebugLog("Too many consecutive failures; restarting");
    delay(300);
    ESP.restart();
  } else if (consecutiveFailures == 3) {
    addDebugLog("3 consecutive failures; cycling WiFi");
    WiFi.disconnect();
    delay(500);
    WiFi.reconnect();
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
      delay(250);
      yield();
    }
    forceDNS();
    startMDNS();
    addDebugLog(String("WiFi after cycle: ") + WiFi.status());
  }
}

// Shared HTTPClient setup. HTTP/1.0 avoids chunked transfer encoding so we can
// parse straight from the stream without buffering the payload in a String.
void configureHttp(HTTPClient &http) {
  http.setConnectTimeout(10000);
  http.setTimeout(15000);
  http.setReuse(false);
  http.useHTTP10(true);
}

void checkForUpdates() {
  Serial.println("Checking for updates....");
  addDebugLog("Checking for firmware updates");
  WiFiClientSecure client;
  client.setInsecure(); 

  HTTPClient http;
  http.setConnectTimeout(10000);
  http.setTimeout(15000);
  http.setReuse(false);

  if (!http.begin(client, versionUrl)) {
    Serial.println("Firmware version http.begin failed");
    addDebugLog("Firmware version http.begin failed");
    client.stop();
    return;
  }

  // Follow GitHub's redirects for version.txt.
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  int httpCode = http.GET();

  if (httpCode == 200) {
    int newVersion = http.getString().toInt();
    Serial.printf("Current: %d, New: %d\n", currentVersion, newVersion);
    addDebugLog(String("Firmware versions: current=") + currentVersion + ", available=" + newVersion);

    if (newVersion > currentVersion) {
      Serial.println("New version found! Starting update....");
      addDebugLog("Firmware update available; starting update");
      
      // The update() function handles the download and will automatically reboot on success
      t_httpUpdate_return ret = httpUpdate.update(client, binaryUrl);

      switch (ret) {
        case HTTP_UPDATE_FAILED:
          Serial.printf("Update Failed (%d): %s\n", httpUpdate.getLastError(), httpUpdate.getLastErrorString().c_str());
          addDebugLog(String("Firmware update failed ") + httpUpdate.getLastError() + ": " + httpUpdate.getLastErrorString());
          break;
        case HTTP_UPDATE_NO_UPDATES:
          Serial.println("No updates found.");
          addDebugLog("No firmware update found");
          break;
        case HTTP_UPDATE_OK:
          Serial.println("Update success!");
          addDebugLog("Firmware update succeeded");
          break;
      }
    } else {
      Serial.println("Software is up to date.");
      addDebugLog("Firmware is up to date");
    }
  } else {
    Serial.printf("Failed to check version. HTTP Code: %d\n", httpCode);
    addDebugLog(String("Firmware version check failed, HTTP ") + httpCode);
  }
  http.end();
  client.stop();
}

#define DATA_PIN 13 
#define WIDTH 96
#define HEIGHT 8
#define NUM_LEDS (WIDTH * HEIGHT)

CRGB leds[NUM_LEDS];
FastLED_NeoMatrix *matrix;

// Fire settings (Adjust these to change the "intensity")
#define COOLING  60   // Higher = shorter flames
#define SPARKING 140  // Higher = more random sparks
static byte heat[WIDTH * HEIGHT]; // Heat memory for every pixel

//0 = red, 1 = green, 2 = blue
int flameMode = 0;

WebServer server(80);

/* ================= DEBUG LOGGING ================= */
// Recent logs are kept in RAM and shown at /logs.
// Fixed-size char buffers (instead of String) so logging never fragments the heap.
#define MAX_DEBUG_LOGS 100
#define DEBUG_LOG_LEN 160
char debugLogs[MAX_DEBUG_LOGS][DEBUG_LOG_LEN];
int debugLogCount = 0;
int debugLogNext = 0;

void addDebugLog(const String &message) {
  snprintf(debugLogs[debugLogNext], DEBUG_LOG_LEN, "[%lus] %s",
           millis() / 1000, message.c_str());
  Serial.print("[DEBUG] ");
  Serial.println(debugLogs[debugLogNext]);
  debugLogNext = (debugLogNext + 1) % MAX_DEBUG_LOGS;
  if (debugLogCount < MAX_DEBUG_LOGS) debugLogCount++;
}

void logNetworkDiagnostics(const char *source) {
  String diagnostics = String("status=") + WiFi.status() +
      ", RSSI=" + WiFi.RSSI() + " dBm, MAC=" + WiFi.macAddress() +
      ", BSSID=" + WiFi.BSSIDstr() + ", IP=" + WiFi.localIP().toString() +
      ", gateway=" + WiFi.gatewayIP().toString() +
      ", DNS=" + WiFi.dnsIP().toString() + ", heap=" + ESP.getFreeHeap() +
      ", maxBlock=" + ESP.getMaxAllocHeap();
  Serial.println(String("[NETWORK] ") + source + ": " + diagnostics);
  addDebugLog(String(source) + " network diagnostics: " + diagnostics);
}

const char* modeName(DisplayMode mode) {
  switch (mode) {
    case MODE_SPORTS:    return "SPORTS";
    case MODE_STOCKS:    return "STOCKS";
    case MODE_PHRASES:   return "PHRASES";
    case MODE_WEATHER:   return "WEATHER";
    case MODE_CYCLE:     return "CYCLE";
    case MODE_FIREPLACE: return "FIREPLACE";
    default:             return "UNKNOWN";
  }
}

void switchMode(DisplayMode newMode, const char *source) {
  DisplayMode oldMode = currentMode;
  currentMode = newMode;
  addDebugLog(String("Mode switch: ") + modeName(oldMode) + " -> " +
              modeName(newMode) + " via " + source);
}

/* ================= MODES & SETTINGS ================= */


volatile int currentBrightness = 40;
volatile int scrollDelay = 70;
// RGB values for phrases (defaulting to Light Blue)
volatile uint8_t pr = 0;
volatile uint8_t pg = 150;
volatile uint8_t pb = 255;

/* ================= DATA STRUCTURES ================= */
// SPORTS (NFL, NBA, NHL, MLB, NCAAB, ...)
// Fixed-size char buffers instead of Strings so refetching never fragments the heap.
struct Game {
  char league[8];
  char away[8];  char awayScore[8];
  char home[8];  char homeScore[8];
  char status[32];
  uint8_t awayRank, homeRank; // 0 = unranked / no rank
  uint8_t ar, ag, ab, hr, hg, hb;
};
#define MAX_GAMES 60
Game games[MAX_GAMES];
int gameCount = 0, currentGame = 0;
unsigned long lastSportsFetch = 0;

// League filter: toggled from the web dashboard and saved in flash.
// Bit i of enabledLeagues = sportsLeagues[i] is shown. Add a league here AND in the Worker.
const char *sportsLeagues[] = {"NFL", "NCAAF", "NBA", "NCAAB", "NHL", "AHL", "MLB"};
#define NUM_SPORT_LEAGUES ((int)(sizeof(sportsLeagues) / sizeof(sportsLeagues[0])))
uint16_t enabledLeagues = 0xFFFF;           // default: everything on
volatile bool sportsFilterChanged = false;  // tells the scroll loop to stop and refetch
Preferences prefs;

bool leagueEnabled(const char *league) {
  for (int i = 0; i < NUM_SPORT_LEAGUES; i++) {
    if (strcmp(league, sportsLeagues[i]) == 0) return (enabledLeagues & (1 << i)) != 0;
  }
  return true; // leagues the firmware doesn't know about always show
}

void loadLeagueFilter() {
  prefs.begin("ticker", false);
  enabledLeagues = prefs.getUShort("leagues", 0xFFFF);
  prefs.end();
  addDebugLog(String("League filter mask: 0x") + String(enabledLeagues, HEX));
}

void saveLeagueFilter() {
  prefs.begin("ticker", false);
  prefs.putUShort("leagues", enabledLeagues);
  prefs.end();
}

// STOCKS
struct Stock { String symbol; float price, percent; };
#define MAX_STOCKS 52
Stock stocks[MAX_STOCKS];
int stockCount = 0, currentStock = 0;
unsigned long lastStockFetch = 0;
const unsigned long STOCK_REFRESH_INTERVAL = 15UL * 60UL * 1000UL;

// PHRASES 
#define MAX_PHRASES 20
String phrases[MAX_PHRASES];
int phraseCount = 0;
int currentPhrase = 0;

// WEATHER
struct Weather { 
  float temp; 
  float feelsLike;
  int humidity;
  float windSpeed;
  int code; 
  String summary; // 
  String condition; 
};
Weather localWeather;
unsigned long lastWeatherFetch = 0;
bool weatherLoaded = false;


// ARTEMIS


const uint8_t PROGMEM sun_bmp[] = {0x18,0x3C,0x7E,0x7E,0x7E,0x7E,0x3C,0x18};
const uint8_t PROGMEM cloud_bmp[] = {0x00,0x06,0x1F,0x3F,0x7F,0x7F,0x3F,0x00};
const uint8_t PROGMEM rain_bmp[] = {0x00,0x06,0x1F,0x3F,0x3F,0x12,0x04,0x00};
const uint8_t PROGMEM snow_bmp[] = {0x24,0x66,0xFF,0x7E,0x7E,0xFF,0x66,0x24};
const uint8_t PROGMEM storm_bmp[] = {0x00,0x0E,0x1F,0x3F,0x0E,0x1C,0x18,0x10};

/* ================= TEAM COLORS ================= */
// Colors now come from the worker as hex strings ("a71930"). Many team colors
// are very dark (navy, black, brown) and nearly invisible on an LED matrix, so
// anything below MIN_LUMINANCE is scaled up (keeping its hue) until readable.
#define MIN_LUMINANCE 110

void parseTeamColor(const char *hex, uint8_t &r, uint8_t &g, uint8_t &b) {
  r = g = b = 200; // fallback if the color is missing or malformed
  if (!hex) return;
  if (hex[0] == '#') hex++;
  if (strlen(hex) < 6) return;

  long n = strtol(hex, NULL, 16);
  int ri = (n >> 16) & 0xFF;
  int gi = (n >> 8) & 0xFF;
  int bi = n & 0xFF;

  int lum = (ri * 299 + gi * 587 + bi * 114) / 1000;
  if (lum < MIN_LUMINANCE) {
    if (ri + gi + bi < 30) {
      // Essentially black (e.g. PIT, LV): there's no hue to preserve
      ri = gi = bi = 160;
    } else {
      float s = (float)MIN_LUMINANCE / (float)max(lum, 1);
      ri = min(255, (int)(ri * s));
      gi = min(255, (int)(gi * s));
      bi = min(255, (int)(bi * s));
    }
  }
  r = ri; g = gi; b = bi;
}

// Scores are strings in the current worker output, but accept numbers too.
void copyScore(JsonVariant v, char *dest, size_t destSize) {
  if (v.is<const char*>()) {
    strlcpy(dest, v.as<const char*>(), destSize);
  } else if (v.is<int>()) {
    snprintf(dest, destSize, "%d", v.as<int>());
  } else {
    strlcpy(dest, "0", destSize);
  }
}

/* ================= DISPLAY FUNCTIONS ================= */
// Format:  (NFL) ATL:14 - NO:0 | 15:00 - 2nd
// - The "(LEAGUE) " tag only shows on the first game of each league run.
// - Ranked teams get a rank prefix in their own color:  #5 OSU:14 - #10 ISU:7
// - Upcoming games (status is a start time) show "LAL @ SAC" instead of 0-0 scores.
void displaySportsGame(int idx) {
  Game &g = games[idx];
  Serial.printf("[SPORTS] displaySportsGame start idx=%d gameCount=%d\n", idx, gameCount);

  bool scheduled = strstr(g.status, " AM ") || strstr(g.status, " PM ");
  bool showLeague = (idx == 0) || strcmp(games[idx - 1].league, g.league) != 0;

  // Build each colored segment once, before the scroll loop
  char leagueTxt[16], awayName[16], homeName[16];
  char awayTxt[28], midTxt[4], homeTxt[28], statusTxt[40];

  if (showLeague && g.league[0]) snprintf(leagueTxt, sizeof(leagueTxt), "(%s) ", g.league);
  else leagueTxt[0] = '\0';

  // Team names, with an optional "#rank " prefix (same color as the team)
  if (g.awayRank > 0) snprintf(awayName, sizeof(awayName), "#%d %s", g.awayRank, g.away);
  else snprintf(awayName, sizeof(awayName), "%s", g.away);
  if (g.homeRank > 0) snprintf(homeName, sizeof(homeName), "#%d %s", g.homeRank, g.home);
  else snprintf(homeName, sizeof(homeName), "%s", g.home);

  if (scheduled) {
    snprintf(awayTxt, sizeof(awayTxt), "%s", awayName);
    strcpy(midTxt, " @ ");
    snprintf(homeTxt, sizeof(homeTxt), "%s", homeName);
  } else {
    snprintf(awayTxt, sizeof(awayTxt), "%s:%s", awayName, g.awayScore);
    strcpy(midTxt, " - ");
    snprintf(homeTxt, sizeof(homeTxt), "%s:%s", homeName, g.homeScore);
  }
  if (g.status[0]) snprintf(statusTxt, sizeof(statusTxt), " | %s", g.status);
  else statusTxt[0] = '\0';

  uint16_t cLeague = matrix->Color(170, 170, 170);
  uint16_t cAway   = matrix->Color(g.ar, g.ag, g.ab);
  uint16_t cMid    = matrix->Color(255, 255, 255);
  uint16_t cHome   = matrix->Color(g.hr, g.hg, g.hb);
  uint16_t cStatus = matrix->Color(200, 200, 200);

  int totalChars = strlen(leagueTxt) + strlen(awayTxt) + strlen(midTxt) +
                   strlen(homeTxt) + strlen(statusTxt);
  int x = WIDTH, minX = -(totalChars * 6);

  // sportsFilterChanged ends the scroll early so a filter change applies right away
  while (x > minX && (currentMode == MODE_SPORTS || currentMode == MODE_CYCLE) && !sportsFilterChanged) {
    server.handleClient(); yield();
    matrix->fillScreen(0); matrix->setCursor(x, 1);
    matrix->setTextColor(cLeague); matrix->print(leagueTxt);
    matrix->setTextColor(cAway);   matrix->print(awayTxt);
    matrix->setTextColor(cMid);    matrix->print(midTxt);
    matrix->setTextColor(cHome);   matrix->print(homeTxt);
    matrix->setTextColor(cStatus); matrix->print(statusTxt);
    matrix->show(); x--; delay(scrollDelay);
  }
  Serial.println("[SPORTS] displaySportsGame end");
}

void displayStock(int idx) {
  Stock &s = stocks[idx];
  Serial.printf("[STOCKS] displayStock start idx=%d stockCount=%d\n", idx, stockCount);
  bool up = s.percent >= 0;
  uint16_t color = up ? matrix->Color(0,255,0) : matrix->Color(255,0,0);
  String text = s.symbol + " " + String(s.price,2) + " (" + (up?"+":"") + String(s.percent,2) + "%)";
  int x = WIDTH, minX = -((int)text.length() * 6);
  while (x > minX && (currentMode == MODE_STOCKS || currentMode == MODE_CYCLE)) {
    server.handleClient(); yield();
    matrix->fillScreen(0); matrix->setCursor(x, 1);
    matrix->setTextColor(color); matrix->print(text);
    matrix->show(); x--; delay(scrollDelay);
  }
  Serial.println("[STOCKS] displayStock end");
}

void displayPhrase(int idx) {
  String text = phrases[idx];
  int x = WIDTH, minX = -((int)text.length() * 6);
  while (x > minX && (currentMode == MODE_PHRASES || currentMode == MODE_CYCLE)) {
    server.handleClient(); yield();
    matrix->fillScreen(0); 
    matrix->setCursor(x, 1);
    matrix->setTextColor(matrix->Color(pr, pg, pb)); // Uses the selected color
    matrix->print(text);
    matrix->show(); 
    x--; 
    delay(scrollDelay);
  }
}

void displayWeather() {
  if (!weatherLoaded) return;

  // 1. Setup the text parts (Condition first, then stats + summary)
  String prefix = "BLOOMFIELD: " + localWeather.condition + " ";
  String suffix = " " + String(localWeather.temp, 0) + "F | " + localWeather.summary;

  // 2. Calculate offsets
  int prefixWidth = prefix.length() * 6; // Standard font is 6 pixels wide
  int iconWidth = 10; // 8 pixels for icon + 2 for padding
  int suffixWidth = suffix.length() * 6;
  int totalWidth = prefixWidth + iconWidth + suffixWidth;

  int x = WIDTH; 
  int minX = -totalWidth;
  
  while (x > minX && (currentMode == MODE_WEATHER || currentMode == MODE_CYCLE)) {
    server.handleClient(); 
    yield();
    matrix->fillScreen(0);
    
    // --- ICON SELECTION LOGIC (Fixes the 'not declared' error) ---
    const uint8_t* icon;
    uint16_t iconColor;
    int c = localWeather.code;
    if (c == 0) { icon = sun_bmp; iconColor = matrix->Color(255,200,0); }
    else if (c <= 3) { icon = cloud_bmp; iconColor = matrix->Color(150,150,150); }
    else if (c <= 67) { icon = rain_bmp; iconColor = matrix->Color(0,100,255); }
    else if (c <= 77) { icon = snow_bmp; iconColor = matrix->Color(255,255,255); }
    else { icon = storm_bmp; iconColor = matrix->Color(200,0,255); }
    // ------------------------------------------------------------

    // 3. Draw Prefix (The "Condition" part)
    matrix->setCursor(x, 1);
    matrix->setTextColor(matrix->Color(200, 200, 200));
    matrix->print(prefix);

    // 4. Draw Icon (follows the prefix text)
    matrix->drawBitmap(x + prefixWidth, 0, icon, 8, 8, iconColor);

    // 5. Draw Suffix (The "Temperature | Forecast" part)
    matrix->setCursor(x + prefixWidth + iconWidth, 1);
    matrix->setTextColor(matrix->Color(200, 200, 200));
    matrix->print(suffix);

    matrix->show();
    x--;
    delay(scrollDelay); 
  }
}

void displayFireplace() {
  // --- SETTINGS FOR DEEP REDS ---
  // COOLING: Higher = more red/orange. Try 80 if it's still too yellow.
  // SPARKING: Lower = fewer "white hot" spots.
  const int customCooling = 85; 
  const int customSparking = 100;

  // 1. Cool down the top 4 rows (the flames)
  for(int i = 0; i < WIDTH * 4; i++) {
    // We cool them more aggressively to ensure they turn red before disappearing
    heat[i] = qsub8(heat[i], random8(0, customCooling)); 
  }

  // 2. Heat drifts UP (row 4 -> row 0)
  for(int y = 0; y < 4; y++) {
    for(int x = 0; x < WIDTH; x++) {
      // Average the heat from below to create a "flicker"
      heat[y * WIDTH + x] = (heat[(y + 1) * WIDTH + x] + heat[(y + 2) * WIDTH + x]) / 2;
    }
  }

  // 3. The "Embers" (Bottom 4 rows)
  // We force these into the RED/ORANGE range (heat 100-180)
  for(int i = WIDTH * 4; i < WIDTH * HEIGHT; i++) {

    if(flameMode == 2) {
      // wider range for green flames
      heat[i] = random8(40, 200);
    }
    else {
      heat[i] = random8(80, 160);
    }

  }

  if(flameMode == 2){
    for(int i = WIDTH * 4; i < WIDTH * HEIGHT; i++){
      heat[i] = qadd8(heat[i], random8(0,30));
    }
  }

  // 4. Random Sparks at the "Log Line" (Row 4)
  if(random8() < customSparking) {
    int x = random8(WIDTH);
    heat[4 * WIDTH + x] = qadd8(heat[4 * WIDTH + x], random8(100, 200));
  }

  // 5. Render with a "Warm" mapping
  for(int y = 0; y < HEIGHT; y++) {
    for(int x = 0; x < WIDTH; x++) {
      byte colorIndex = heat[y * WIDTH + x];
      CRGB color;

      // Use HeatColor but cap the intensity so it doesn't stay white/yellow

      if (flameMode == 0){
        color = HeatColor(colorIndex);
      }

      else if (flameMode == 1){
        byte heatVal = colorIndex;

        if(heatVal < 85) {
          color = CRGB(0, 0, heatVal * 3);           // deep blue base
        }
        else if(heatVal < 170) {
          heatVal -= 85;
          color = CRGB(0, heatVal * 3, 255);         // blue → cyan
        }
        else {
          heatVal -= 170;
          color = CRGB(heatVal * 3, 255, 255);       // cyan → white tips
        }

        // subtle flicker variation
        color.b = min(255, color.b + random8(0,30));
      }

      else if (flameMode == 2){
        byte heatVal = colorIndex;

        if(heatVal < 120) {
          // deep green -> bright green
          color = CRGB(0, heatVal * 2, 0);
        }
        else if(heatVal < 200) {
          // neon green / lime
          heatVal -= 120;
          color = CRGB(heatVal * 2, 200 + heatVal/2, 0);
        }
        else {
          // yellow tips
          heatVal -= 200;
          color = CRGB(200 + heatVal * 2, 255, 0);
        }

        // flicker variation so flames aren't uniform
        color.g = min(255, color.g + random8(0,60));
      }
      
      
      // OPTIONAL: Boost the Red channel slightly for extra warmth
      //if(color.r > 0) color.r = qadd8(color.r, 20); 

      matrix->drawPixel(x, y, matrix->Color(color.r, color.g, color.b));
    }
  }
  matrix->show();
  delay(60); // Slower speed makes the "rising" look more like real fire
}



/* ================= FETCH LOGIC ================= */
bool ensureWiFi(const char *source) {
  wl_status_t status = WiFi.status();
  Serial.printf("[%s] WiFi status=%d RSSI=%d dBm heap=%u\n",
                source, status, WiFi.RSSI(), ESP.getFreeHeap());
  addDebugLog(String(source) + " network: WiFi=" + status +
              ", RSSI=" + WiFi.RSSI() + " dBm, heap=" + ESP.getFreeHeap());

  if (status != WL_CONNECTED) {
    addDebugLog(String(source) + " WiFi disconnected; reconnecting");
    WiFi.reconnect();
    unsigned long reconnectStart = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - reconnectStart < 10000) {
      delay(250);
      yield();
    }

    status = WiFi.status();
    Serial.printf("[%s] WiFi after reconnect: status=%d RSSI=%d dBm\n",
                  source, status, WiFi.RSSI());
    addDebugLog(String(source) + " WiFi reconnect result: " + status);
    if (status == WL_CONNECTED) startMDNS();
  }

  if (status != WL_CONNECTED) return false;

  // Re-apply IPv4 DNS every time; DHCP renewals can overwrite it.
  forceDNS();
  return true;
}

void fetchSports() {
  sportsFilterChanged = false; // this fetch picks up the current filter
  if (!ensureWiFi("SPORTS")) return;
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http;
  configureHttp(http);
  Serial.println("[SPORTS] fetchSports start");
  addDebugLog("Sports fetch started");
  lastSportsFetch = millis();
  if (http.begin(client, SPORTS_URL)) {
    int httpCode = http.GET();
    Serial.printf("[SPORTS] HTTP GET code: %d\n", httpCode);
    addDebugLog(String("Sports HTTP GET code: ") + httpCode);
    if (httpCode == 200) {
      noteFetchSuccess();

      // Only keep the fields we display (drops shortName etc.) to save RAM
      StaticJsonDocument<384> filter;
      filter[0]["league"] = true;
      filter[0]["status"] = true;
      filter[0]["home"]["team"] = true;
      filter[0]["home"]["score"] = true;
      filter[0]["home"]["color"] = true;
      filter[0]["away"]["team"] = true;
      filter[0]["away"]["score"] = true;
      filter[0]["away"]["color"] = true;
      filter[0]["home"]["rank"] = true;
      filter[0]["away"]["rank"] = true;

      DynamicJsonDocument doc(32768); // ~300 bytes per game after filtering; fits 60 with room to spare
      // Parse straight from the stream; no big String copy of the payload
      DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
      if (err) {
        Serial.print("[SPORTS] JSON parse error: "); Serial.println(err.c_str());
        addDebugLog(String("Sports JSON parse error: ") + err.c_str());
      } else {
        gameCount = 0;
        bool truncated = false;
        for (JsonVariant v : doc.as<JsonArray>()) {
          if (gameCount >= MAX_GAMES) { truncated = true; break; }

          const char *awayTeam = v["away"]["team"] | "";
          const char *homeTeam = v["home"]["team"] | "";
          if (*awayTeam == '\0' || *homeTeam == '\0') continue;

          const char *league = v["league"] | "";
          if (!leagueEnabled(league)) continue; // filtered out on the dashboard

          Game &g = games[gameCount];
          strlcpy(g.league, league, sizeof(g.league));
          strlcpy(g.status, v["status"] | "", sizeof(g.status));
          strlcpy(g.away, awayTeam, sizeof(g.away));
          strlcpy(g.home, homeTeam, sizeof(g.home));
          copyScore(v["away"]["score"], g.awayScore, sizeof(g.awayScore));
          copyScore(v["home"]["score"], g.homeScore, sizeof(g.homeScore));
          parseTeamColor(v["away"]["color"] | "", g.ar, g.ag, g.ab);
          parseTeamColor(v["home"]["color"] | "", g.hr, g.hg, g.hb);
          g.awayRank = (uint8_t)(v["away"]["rank"] | 0);
          g.homeRank = (uint8_t)(v["home"]["rank"] | 0);
          gameCount++;
        }
        Serial.printf("[SPORTS] parsed games: %d\n", gameCount);
        addDebugLog(String("Sports parsed games: ") + gameCount + (truncated ? " (truncated at MAX_GAMES)" : ""));
        lastSportsFetch = millis();
      }
    } else {
      String error = http.errorToString(httpCode);
      Serial.println("[SPORTS] HTTP GET failed or returned non-200");
      Serial.printf("[SPORTS] HTTP error: %s\n", error.c_str());
      addDebugLog(String("Sports HTTP GET failed, HTTP ") + httpCode + ": " + error);
      if (httpCode <= 0) {
        logDnsTest(SPORTS_HOST);
        http.end();
        client.stop();
        noteFetchFailure("SPORTS");
        return;
      } else {
        noteFetchSuccess(); // server answered, so the network is fine
      }
    }
    http.end();
    client.stop();
  } else {
    Serial.println("[SPORTS] http.begin failed");
    addDebugLog("Sports http.begin failed");
    client.stop();
  }
}

void fetchStocks() {
  if (!ensureWiFi("STOCKS")) return;
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  configureHttp(http);
  const char *url = "https://stockscraper.adamjsmith002.workers.dev/";

  Serial.println();
  Serial.println("========== STOCK FETCH ==========");
  Serial.printf("[STOCKS] URL: %s\n", url);
  logNetworkDiagnostics("Stocks before HTTPS GET");

  addDebugLog("Stocks fetch started");

  lastStockFetch = millis();

  if (!http.begin(client, url)) {
    Serial.println("[STOCKS] http.begin FAILED");
    addDebugLog("Stocks: http.begin failed");
    client.stop();
    return;
  }

  Serial.println("[STOCKS] Starting HTTPS GET...");
  addDebugLog("Stocks HTTPS GET started");

  int httpCode = http.GET();

  Serial.printf("[STOCKS] HTTP GET code: %d\n", httpCode);
  addDebugLog(String("Stocks HTTP GET code: ") + httpCode);

  if (httpCode <= 0) {
    logNetworkDiagnostics("Stocks after HTTPS failure");
    logDnsTest("stockscraper.adamjsmith002.workers.dev");
    Serial.printf("[STOCKS] HTTP error: %s\n", http.errorToString(httpCode).c_str());
    addDebugLog(String("Stocks HTTP error ") + httpCode + ": " + http.errorToString(httpCode));

    http.end();
    client.stop();
    Serial.println("=================================");
    noteFetchFailure("STOCKS");
    return;
  }

  noteFetchSuccess(); // server answered

  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("[STOCKS] Server returned HTTP %d\n", httpCode);
    addDebugLog(String("Stocks server HTTP ") + httpCode);

    http.end();
    client.stop();
    Serial.println("=================================");
    return;
  }

  addDebugLog("Stocks successful response");

  DynamicJsonDocument doc(16384);
  DeserializationError err = deserializeJson(doc, http.getStream());

  if (err || !doc.is<JsonArray>()) {
    Serial.print("[STOCKS] JSON parse error: ");
    if (err) {
      Serial.println(err.c_str());
    } else {
      Serial.println("response is not an array");
    }

    addDebugLog(String("Stocks JSON error: ") + (err ? err.c_str() : "not an array"));

    http.end();
    client.stop();
    Serial.println("=================================");
    return;
  }

  int parsedStockCount = 0;

  for (JsonVariant v : doc.as<JsonArray>()) {
    if (parsedStockCount >= MAX_STOCKS)
      break;

    const char *symbol = v["symbol"] | "";

    if (*symbol == '\0')
      continue;

    stocks[parsedStockCount++] = {
      symbol,
      v["price"] | 0.0f,
      v["percent"] | 0.0f
    };
  }

  stockCount = parsedStockCount;
  currentStock = 0;

  Serial.printf("[STOCKS] Parsed stocks: %d\n", stockCount);
  addDebugLog(String("Stocks fetched: ") + stockCount);

  http.end();
  client.stop();

  Serial.println("=================================");
}


void fetchWeather() {
  if (!ensureWiFi("WEATHER")) return;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  configureHttp(http);

  const char *url = "https://api.open-meteo.com/v1/forecast?latitude=41.83&longitude=-72.70&current=temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,wind_speed_10m&temperature_unit=fahrenheit&wind_speed_unit=mph";

  logNetworkDiagnostics("Weather before HTTPS GET");
  addDebugLog("Weather fetch started");

  // Set the retry timestamp up front so a failing fetch doesn't get retried
  // in a tight loop every time the weather screen comes around.
  lastWeatherFetch = millis();

  if (!http.begin(client, url)) {
    Serial.println("[WEATHER] http.begin failed");
    addDebugLog("Weather http.begin failed");
    client.stop();
    return;
  }

  int httpCode = http.GET();
  Serial.printf("[WEATHER] HTTP GET code: %d\n", httpCode);
  addDebugLog(String("Weather HTTP GET code: ") + httpCode);

  if (httpCode == 200) {
    noteFetchSuccess();
    DynamicJsonDocument doc(2048);

    DeserializationError err = deserializeJson(doc, http.getStream());
    if (err) {
      Serial.printf("[WEATHER] JSON parse error: %s\n", err.c_str());
      addDebugLog(String("Weather JSON parse error: ") + err.c_str());
    } else {
      localWeather.temp = doc["current"]["temperature_2m"];
      localWeather.feelsLike = doc["current"]["apparent_temperature"];
      localWeather.humidity = doc["current"]["relative_humidity_2m"];
      localWeather.windSpeed = doc["current"]["wind_speed_10m"];
      localWeather.code = doc["current"]["weather_code"];

      int c = localWeather.code;
      if (c == 0) localWeather.condition = "CLEAR";
      else if (c <= 3) localWeather.condition = "CLOUDY";
      else if (c <= 48) localWeather.condition = "FOGGY";
      else if (c <= 67) localWeather.condition = "RAIN";
      else if (c <= 77) localWeather.condition = "SNOW";
      else localWeather.condition = "STORM";

      weatherLoaded = true;
      lastWeatherFetch = millis();
      addDebugLog(String("Weather loaded: ") + localWeather.condition + ", " + localWeather.temp + "F");
    }
  } else {
    logNetworkDiagnostics("Weather after HTTPS failure");
    Serial.printf("[WEATHER] HTTP error: %s\n", http.errorToString(httpCode).c_str());
    addDebugLog(String("Weather HTTP error ") + httpCode + ": " + http.errorToString(httpCode));
    if (httpCode <= 0) {
      logDnsTest("api.open-meteo.com");
      http.end();
      client.stop();
      noteFetchFailure("WEATHER");
      return;
    } else {
      noteFetchSuccess();
    }
  }

  http.end();
  client.stop();
}


 void fetchForecastText() {
  if (!ensureWiFi("FORECAST")) return;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  configureHttp(http);

  const char *url = "https://api.weather.gov/gridpoints/BOX/68,91/forecast";

  logNetworkDiagnostics("Forecast before HTTPS GET");

  if (!http.begin(client, url)) {
    Serial.println("[WEATHER] Forecast http.begin failed");
    addDebugLog("Forecast http.begin failed");
    client.stop();
    return;
  }

  http.addHeader("User-Agent", "ESP32-Weather-Display");

  int httpCode = http.GET();
  Serial.printf("[WEATHER] Forecast HTTP GET code: %d\n", httpCode);
  addDebugLog(String("Forecast HTTP GET code: ") + httpCode);

  if (httpCode == 200) {
    noteFetchSuccess();

    StaticJsonDocument<512> filter;
    filter["properties"]["periods"][0]["detailedForecast"] = true;

    DynamicJsonDocument doc(8192);
    DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));

    if (!err) {
      localWeather.summary = doc["properties"]["periods"][0]["detailedForecast"].as<String>();
      Serial.println("Forecast Summary: " + localWeather.summary);
      addDebugLog("Forecast summary loaded");
    } else {
      Serial.printf("[WEATHER] Forecast JSON parse error: %s\n", err.c_str());
      addDebugLog(String("Forecast JSON parse error: ") + err.c_str());
    }
  } else {
    logNetworkDiagnostics("Forecast after HTTPS failure");
    addDebugLog(String("Forecast HTTP error ") + httpCode + ": " + http.errorToString(httpCode));
    if (httpCode <= 0) {
      logDnsTest("api.weather.gov");
      http.end();
      client.stop();
      noteFetchFailure("FORECAST");
      return;
    } else {
      noteFetchSuccess();
    }
  }

  http.end();
  client.stop();
}


String cleanText(String text) {
  // Replace common "Smart" characters with standard ASCII
  text.replace("’", "'");  // Smart apostrophe
  text.replace("‘", "'");  // Smart opening single quote
  text.replace("“", "\""); // Smart opening double quote
  text.replace("”", "\""); // Smart closing double quote
  text.replace("–", "-");  // En dash
  text.replace("—", "-");  // Em dash
  return text;
}

void configModeCallback (WiFiManager *myWiFiManager) {
  // This only runs if Brandon needs to connect to "Ticker-Setup"
  matrix->fillScreen(0);
  matrix->setTextColor(matrix->Color(255, 0, 0)); // Red for attention
  matrix->setCursor(2, 1);
  matrix->print("SETUP");
  matrix->show();
  
  // Optional: Wait 2 seconds then scroll the Setup IP (192.168.4.1) 
  // so he knows exactly what to type to reach the portal.
  delay(2000);
  String setupIP = WiFi.softAPIP().toString();
  int msgWidth = (setupIP.length() * 6) + WIDTH;
  for (int x = WIDTH; x > -msgWidth; x--) {
    matrix->fillScreen(0);
    matrix->setCursor(x, 1);
    matrix->print(setupIP);
    matrix->show();
    delay(100);
  }
}

/* ================= WEB DASHBOARD ================= */
void setupWeb() {
  server.on("/", []() {
    String html = "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<style>*{box-sizing: border-box;} "; // FIX: Ensures padding doesn't make boxes wider
    html += "body{font-family:sans-serif; text-align:center; background:#111; color:#fff; padding:20px; max-width:420px; margin:auto;} ";
    html += ".btn, input[type=text], input[type=color], select{width:100%; display:block; margin:10px 0; padding:15px; border-radius:8px; border:none; font-size:1.1em;} ";
    html += ".btn{background:#0af; color:#fff; font-weight:bold; cursor:pointer;} ";
    html += ".btn:hover{background:#09c;} ";
    html += "h3{margin-top:25px; color:#0af;} ";
    html += "select{background:#0af; color:#fff; font-weight:bold; cursor:pointer;} ";
    html += ".clear{background:#f44;} ";
    html += "input[type=color]{height:50px; cursor:pointer; background:#333;} ";
    html += ".leagues{display:flex; flex-wrap:wrap; gap:8px; justify-content:center;} ";
    html += ".lg{background:#222; padding:10px 14px; border-radius:8px; cursor:pointer;} ";
    html += ".lg input{margin-right:6px; transform:scale(1.3);} ";
    html += "input[type=range]{width:100%; margin:15px 0;}</style></head><body>";
    
    html += String("<h2>Matrix Dashboard V1.") + currentVersion + "</h2>";
    html += "<button class='btn' style='background:#f90;' onclick='fetch(\"/cycle\")'>Cycle All Modes</button>";
    html += "<button class='btn' style='background:#555;' onclick='location.href=\"/logs\"'>Debug Logs</button>";  
    html += "<hr><h3>Basic Modes</h3>";  
    html += "<button class='btn' onclick='fetch(\"/sports\")'>Sports Mode</button>";
    html += "<button class='btn' onclick='fetch(\"/stocks\")'>Stock Mode</button>";
    html += "<button class='btn' onclick='fetch(\"/weather\")'>Weather Mode</button>";

    // Sports filter: one checkbox per league, saved on the device
    html += "<hr><h3>Sports Filter</h3><div class='leagues'>";
    for (int i = 0; i < NUM_SPORT_LEAGUES; i++) {
      html += "<label class='lg'><input type='checkbox' ";
      if (enabledLeagues & (1 << i)) html += "checked ";
      html += "onchange='setLeague(" + String(i) + ",this.checked)'>" + String(sportsLeagues[i]) + "</label>";
    }
    html += "</div><script>function setLeague(i,on){fetch('/league?i='+i+'&on='+(on?1:0));}</script>";
    html += "<hr><h3>Fireplace Mode</h3>";
    //html += "<button class='btn' onclick='fetch(\"/fireplace\")'>Fireplace Mode</button>";
    html += "<select id='fireMode' onchange='setFireMode(this.value)'>";
    html += "<option value='red'> Red </option>";
    html += "<option value='blue'> Blue </option>";
    html += "<option value='green'> Green </option>";
    html += "</select>";
    
    html += "<script>";
    html += "function setFireMode(mode){ fetch('/fireplace'); fetch('/flame_' + mode); }";
    html += "</script>";

    html += "<hr><h3>Phrase Mode</h3>";
    html += "<button class='btn' onclick='fetch(\"/phrases\")'>Phrase Mode</button>";
    html += "<input type='text' id='p' placeholder='Type phrase here...'>";
    html += "<button class='btn' onclick='fetch(\"/add?v=\"+encodeURIComponent(document.getElementById(\"p\").value)); document.getElementById(\"p\").value=\"\"'>Add to List</button>";
    
    // Color Picker
    html += "<label>Phrase Color:</label>";
    html += "<input type='color' value='#0096FF' onchange='fetch(\"/color?v=\"+this.value.substring(1))'>";
    
    html += "<button class='btn clear' onclick='fetch(\"/clear\")'>Clear All Phrases</button>";

    html += "<hr><h3>Display Settings</h3>";
    html += "Brightness: <input type='range' min='1' max='40' value='" + String(currentBrightness) + "' onchange='fetch(\"/brightness?v=\"+this.value)'>";
    html += "Delay: <input type='range' min='1' max='150' value='" + String(scrollDelay) + "' onchange='fetch(\"/speed?v=\"+this.value)'>";
    
    html += "</body></html>";
    server.send(200, "text/html", html);
  });

  // New Color Handler
  server.on("/color", []() {
    if (server.hasArg("v")) {
      String hex = server.arg("v");
      long number = strtol(hex.c_str(), NULL, 16);
      pr = number >> 16;
      pg = (number >> 8) & 0xFF;
      pb = number & 0xFF;
      Serial.printf("[WEB] Phrase Color: R:%d G:%d B:%d\n", pr, pg, pb);
      addDebugLog(String("Phrase color: R=") + pr + ", G=" + pg + ", B=" + pb);
      server.send(200, "text/plain", "OK");
    }
  });

  // ... keep your other handlers (/sports, /stocks, /phrases, /add, /clear, /brightness, /speed) ...
  // Recent debug log page
  server.on("/logs", []() {
    String html = "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<meta http-equiv='refresh' content='5'>";
    html += "<style>body{font-family:monospace;background:#111;color:#eee;padding:16px;margin:auto;max-width:800px;}";
    html += "h2{font-family:sans-serif;color:#0af;}.log{padding:8px 10px;border-bottom:1px solid #333;";
    html += "white-space:pre-wrap;word-break:break-word;}.top{font-family:sans-serif;margin-bottom:15px;}";
    html += ".metrics{font-family:monospace;background:#1b1b1b;border:1px solid #333;padding:12px;margin:12px 0 20px;}";
    html += ".metric{display:flex;justify-content:space-between;padding:4px 0;border-bottom:1px solid #2b2b2b;}";
    html += ".metric:last-child{border-bottom:0;}.metric-label{color:#aaa;}.metric-value{color:#7fdaff;}";
    html += "a{color:#0af;}</style></head><body>";
    html += "<div class='top'><a href='/'>Dashboard</a></div>";
    html += "<h2>Recent Debug Logs</h2>";
    unsigned long heapSize = ESP.getHeapSize();
    unsigned long freeHeap = ESP.getFreeHeap();
    unsigned long usedHeap = heapSize > freeHeap ? heapSize - freeHeap : 0;
    html += "<div class='metrics'><div class='metric'><span class='metric-label'>Heap total</span><span class='metric-value'>" + String(heapSize) + " bytes</span></div>";
    html += "<div class='metric'><span class='metric-label'>Heap used</span><span class='metric-value'>" + String(usedHeap) + " bytes</span></div>";
    html += "<div class='metric'><span class='metric-label'>Heap free</span><span class='metric-value'>" + String(freeHeap) + " bytes</span></div>";
    html += "<div class='metric'><span class='metric-label'>Minimum free heap</span><span class='metric-value'>" + String(ESP.getMinFreeHeap()) + " bytes</span></div>";
    html += "<div class='metric'><span class='metric-label'>Largest free block</span><span class='metric-value'>" + String(ESP.getMaxAllocHeap()) + " bytes</span></div>";
    html += "<div class='metric'><span class='metric-label'>Consecutive fetch failures</span><span class='metric-value'>" + String(consecutiveFailures) + "</span></div>";
    html += "<div class='metric'><span class='metric-label'>DNS server</span><span class='metric-value'>" + WiFi.dnsIP().toString() + "</span></div>";
    html += "<div class='metric'><span class='metric-label'>Uptime</span><span class='metric-value'>" + String(millis() / 1000) + " seconds</span></div></div>";

    if (debugLogCount == 0) {
      html += "<div class='log'>No logs yet.</div>";
    } else {
      int start = (debugLogNext - debugLogCount + MAX_DEBUG_LOGS) % MAX_DEBUG_LOGS;
      for (int i = 0; i < debugLogCount; i++) {
        int index = (start + i) % MAX_DEBUG_LOGS;
        html += "<div class='log'>" + String(debugLogs[index]) + "</div>";
      }
    }

    html += "</body></html>";
    server.send(200, "text/html", html);
  });

  // "/nfl" kept as an alias so old bookmarks/buttons still work
  auto sportsHandler = [](){ switchMode(MODE_SPORTS, "WEB /sports"); currentGame = 0; lastSportsFetch = 0; server.send(200,"text/plain","OK"); };
  server.on("/sports", sportsHandler);
  server.on("/nfl", sportsHandler);
  server.on("/league", []() {
    if (server.hasArg("i") && server.hasArg("on")) {
      int i = server.arg("i").toInt();
      bool on = server.arg("on").toInt() != 0;
      if (i >= 0 && i < NUM_SPORT_LEAGUES) {
        if (on) enabledLeagues |= (1 << i);
        else    enabledLeagues &= ~(1 << i);
        saveLeagueFilter();
        addDebugLog(String("League ") + sportsLeagues[i] + (on ? " ON" : " OFF") +
                    ", mask=0x" + String(enabledLeagues, HEX));
        // Apply right away: stop the current scroll and refetch from the top
        currentGame = 0;
        lastSportsFetch = 0;
        sportsFilterChanged = true;
      }
    }
    server.send(200, "text/plain", "OK");
  });
  server.on("/stocks", [](){ switchMode(MODE_STOCKS, "WEB /stocks"); currentStock = 0; lastStockFetch = 0; server.send(200,"text/plain","OK"); });
  server.on("/weather", [](){ switchMode(MODE_WEATHER, "WEB /weather"); lastWeatherFetch = 0; server.send(200,"text/plain","OK"); });
  server.on("/fireplace", [](){ switchMode(MODE_FIREPLACE, "WEB /fireplace"); server.send(200,"text/plain","OK"); });
  server.on("/phrases", [](){ 
    switchMode(MODE_PHRASES, "WEB /phrases"); 
    server.send(200,"text/plain","OK"); 
  });
  server.on("/add", []() {
  if (server.hasArg("v") && phraseCount < MAX_PHRASES) {
    // Clean the text before storing it
    String newPhrase = cleanText(server.arg("v"));
    phrases[phraseCount++] = newPhrase;
    Serial.print("[WEB] Added clean phrase: ");
    Serial.println(newPhrase);
    addDebugLog(String("Phrase added: ") + newPhrase);
    server.send(200, "text/plain", "OK");
  }
});
  server.on("/clear", []() { phraseCount = 0; currentPhrase = 0; server.send(200); });
  server.on("/brightness", [](){ if(server.hasArg("v")){currentBrightness=constrain(server.arg("v").toInt(),1,40); FastLED.setBrightness(currentBrightness);} server.send(200); });
  server.on("/speed", [](){ if(server.hasArg("v")){scrollDelay=constrain(server.arg("v").toInt(),20,150);} server.send(200); });
  server.on("/cycle", [](){ 
    switchMode(MODE_CYCLE, "WEB /cycle"); 
    currentStock = 0;
    currentGame = 0;
    lastSportsFetch = 0; 
    lastStockFetch = 0; 
    lastWeatherFetch = 0;
    server.send(200, "text/plain", "OK"); 
  });
  server.on("/flame_red", [](){ flameMode = 0; addDebugLog("Fireplace color: RED"); server.send(200); });
  server.on("/flame_blue", [](){ flameMode = 1; addDebugLog("Fireplace color: BLUE"); server.send(200); });
  server.on("/flame_green", [](){ flameMode = 2; addDebugLog("Fireplace color: GREEN"); server.send(200); });
  server.begin();
  addDebugLog("Web dashboard started; logs available at /logs");
}

/* ================= SETUP & LOOP ================= */
void setup() {
  Serial.begin(115200);
  addDebugLog("Booting firmware");
  Serial.printf("Reset reason code: %d\n", (int)esp_reset_reason());
  addDebugLog(String("Reset reason code: ") + (int)esp_reset_reason());
  FastLED.addLeds<WS2812B, DATA_PIN, GRB>(leds, NUM_LEDS);
  FastLED.setBrightness(currentBrightness);
  matrix = new FastLED_NeoMatrix(leds, WIDTH, HEIGHT, NEO_MATRIX_TOP + NEO_MATRIX_LEFT + NEO_MATRIX_COLUMNS + NEO_MATRIX_ZIGZAG);
  matrix->begin(); 
  matrix->setTextWrap(false);
  
  WiFi.setHostname(MDNS_NAME); // shows up as "ticker" in your router's device list
  WiFiManager wm;
  
  // This is the key: It only shows the IP if it's NOT connected to WiFi [cite: 100-101, 225].
  wm.setAPCallback(configModeCallback);

  if (!wm.autoConnect("Ticker-Setup")) {
      Serial.println("Connection Failed");
      ESP.restart();
  }

  // Once it connects, it skips all the IP stuff and just starts the app
  Serial.println("WiFi Connected!");
  addDebugLog("WiFi connected");
  Serial.println(WiFi.localIP()); // Still prints to your computer's Serial Monitor for you

  // Keep the radio awake and let the stack auto-reconnect
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);

  // Use IPv4 DNS (some routers advertise an IPv6-only DNS server)
  forceDNS();
  startMDNS(); // http://ticker.local
  logNetworkDiagnostics("Boot");
  logDnsTest("api.open-meteo.com");

  loadLeagueFilter();
  checkForUpdates();
  setupWeb();
  Serial.printf("Free sketch space: %u\n", ESP.getFreeSketchSpace());
}

void loop() {
  server.handleClient();
  yield();

  if (currentMode == MODE_SPORTS) {
    if (currentGame == 0 && (millis() - lastSportsFetch > 60000 || lastSportsFetch == 0)) fetchSports();
    if (gameCount > 0) { displaySportsGame(currentGame++); if (currentGame >= gameCount) currentGame = 0; }
    else {
      matrix->fillScreen(0); matrix->setTextColor(matrix->Color(200, 200, 200));
      matrix->setCursor(2, 1); matrix->print("NO GAMES"); matrix->show(); delay(500);
    }
  } 
  else if (currentMode == MODE_STOCKS) {
    // Fetch only at the beginning of a list. This prevents a long list from
    // being replaced halfway through because the refresh timer expired.
    if (currentStock == 0 && (millis() - lastStockFetch > STOCK_REFRESH_INTERVAL || lastStockFetch == 0)) fetchStocks();
    if (stockCount > 0) { displayStock(currentStock++); if (currentStock >= stockCount) currentStock = 0; }
  }
  else if (currentMode == MODE_PHRASES) {
    if (phraseCount > 0) { 
      displayPhrase(currentPhrase++); 
      if (currentPhrase >= phraseCount) currentPhrase = 0; 
    } else {
      matrix->fillScreen(0); matrix->setCursor(2, 1); matrix->print("EMPTY"); matrix->show(); delay(500);
    }
  }
  else if (currentMode == MODE_WEATHER) {
// Fetch every 15 minutes (900,000 ms)
    if (millis() - lastWeatherFetch > 900000 || lastWeatherFetch == 0) {
      fetchWeather();        // Gets the codes and numbers.
      fetchForecastText();   // Gets the plain-text sentence
    }
    displayWeather();
  }

  else if (currentMode == MODE_FIREPLACE) {
    displayFireplace();
  }

  else if (currentMode == MODE_CYCLE) {
    static int cycleStage = 0; // 0:Sports, 1:Stock, 2:Phrase, 3:Weather
    
    if (cycleStage == 0) {
      if (currentGame == 0 && (millis() - lastSportsFetch > 60000 || lastSportsFetch == 0)) fetchSports();
      if (gameCount > 0) { 
        displaySportsGame(currentGame++); 
        if (currentGame >= gameCount) { currentGame = 0; cycleStage = 1; addDebugLog("Cycle stage: SPORTS -> STOCKS"); }
      } else { cycleStage = 1; addDebugLog("Cycle stage: SPORTS -> STOCKS (no games)"); }
    } 

    else if (cycleStage == 1) { // STOCKS
      // Fetch only at the beginning of a list. This prevents a long list from
      // being replaced halfway through because the refresh timer expired.
      if (currentStock == 0 && (millis() - lastStockFetch > STOCK_REFRESH_INTERVAL || lastStockFetch == 0)) fetchStocks();
      if (stockCount > 0) { 
        displayStock(currentStock++); 
        if (currentStock >= stockCount) { currentStock = 0; cycleStage = 2; addDebugLog("Cycle stage: STOCKS -> PHRASES"); }
      } else { cycleStage = 2; addDebugLog("Cycle stage: STOCKS -> PHRASES (no stocks)"); }
    }

    else if (cycleStage == 2) {
      if (phraseCount > 0) { 
        displayPhrase(currentPhrase++); 
        if (currentPhrase >= phraseCount) { currentPhrase = 0; cycleStage = 3; addDebugLog("Cycle stage: PHRASES -> WEATHER"); }
      } else { cycleStage = 3; addDebugLog("Cycle stage: PHRASES -> WEATHER (no phrases)"); }
    }
    else if (cycleStage == 3) {
      if (millis() - lastWeatherFetch > 900000 || lastWeatherFetch == 0) {
        fetchWeather();
        fetchForecastText();
      }
      displayWeather();
      cycleStage = 0; // Restart cycle
      addDebugLog("Cycle stage: WEATHER -> SPORTS");
    }
  }
}
