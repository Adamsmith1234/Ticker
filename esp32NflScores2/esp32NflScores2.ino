/*
  NFL + Stocks + Phrases Display - ESP32
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

/* ================= CONFIG ================= */

#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>

// Increase this number every time you push a new update to GitHub
const int currentVersion = 5; 

// Replace with your GitHub Username and Repo name
const String baseUrl = "https://raw.githubusercontent.com/Adamsmith1234/Ticker/main/";
const String versionUrl = baseUrl + "version.txt";
const String binaryUrl  = baseUrl + "firmware.bin";

void checkForUpdates() {
  Serial.println("Checking for updates...");
  WiFiClientSecure client;
  client.setInsecure(); 

  HTTPClient http;
  http.begin(client, versionUrl);

  // ADD THIS LINE TO FIX THE 301 ERROR
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  int httpCode = http.GET();

  if (httpCode == 200) {
    int newVersion = http.getString().toInt();
    Serial.printf("Current: %d, New: %d\n", currentVersion, newVersion);

    if (newVersion > currentVersion) {
      Serial.println("New version found! Starting update...");
      
      // The update() function handles the download and will automatically reboot on success
      t_httpUpdate_return ret = httpUpdate.update(client, binaryUrl);

      switch (ret) {
        case HTTP_UPDATE_FAILED:
          Serial.printf("Update Failed (%d): %s\n", httpUpdate.getLastError(), httpUpdate.getLastErrorString().c_str());
          break;
        case HTTP_UPDATE_NO_UPDATES:
          Serial.println("No updates found.");
          break;
        case HTTP_UPDATE_OK:
          Serial.println("Update success!");
          break;
      }
    } else {
      Serial.println("Software is up to date.");
    }
  } else {
    Serial.printf("Failed to check version. HTTP Code: %d\n", httpCode);
  }
  http.end();
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

WebServer server(80);

/* ================= MODES & SETTINGS ================= */
enum DisplayMode { MODE_NFL, MODE_STOCKS, MODE_PHRASES, MODE_WEATHER, MODE_CYCLE, MODE_FIREPLACE };
volatile DisplayMode currentMode = MODE_CYCLE;

volatile int currentBrightness = 40;
volatile int scrollDelay = 70;
// RGB values for phrases (defaulting to Light Blue)
volatile uint8_t pr = 0;
volatile uint8_t pg = 150;
volatile uint8_t pb = 255;

/* ================= DATA STRUCTURES ================= */
// NFL
struct Game { String awayAbbr, awayScore, homeAbbr, homeScore; uint8_t ar, ag, ab, hr, hg, hb; };
#define MAX_GAMES 24
Game games[MAX_GAMES];
int gameCount = 0, currentGame = 0;
unsigned long lastNFLFetch = 0;

// STOCKS
struct Stock { String symbol; float price, percent; };
#define MAX_STOCKS 50
Stock stocks[MAX_STOCKS];
int stockCount = 0, currentStock = 0;
unsigned long lastStockFetch = 0;

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


const uint8_t PROGMEM sun_bmp[] = {0x18,0x3C,0x7E,0x7E,0x7E,0x7E,0x3C,0x18};
const uint8_t PROGMEM cloud_bmp[] = {0x00,0x06,0x1F,0x3F,0x7F,0x7F,0x3F,0x00};
const uint8_t PROGMEM rain_bmp[] = {0x00,0x06,0x1F,0x3F,0x3F,0x12,0x04,0x00};
const uint8_t PROGMEM snow_bmp[] = {0x24,0x66,0xFF,0x7E,0x7E,0xFF,0x66,0x24};
const uint8_t PROGMEM storm_bmp[] = {0x00,0x0E,0x1F,0x3F,0x0E,0x1C,0x18,0x10};

/* ================= TEAM COLORS ================= */
struct TeamColor { const char *abbr; uint8_t r,g,b; };
TeamColor teamColors[] = {
  {"ARI",204,0,0},{"ATL",255,0,0},{"BAL",127,0,255},{"BUF",51,51,255},{"CAR",102,178,255},
  {"CHI",255,128,0},{"CIN",222,87,0},{"CLE",255,102,0},{"DAL",0,102,204},{"DEN",255,153,51},
  {"DET",51,153,255},{"GB",0,102,51},{"HOU",0,76,153},{"IND",0,128,255},{"JAX",0,51,51},
  {"KC",255,0,0},{"LV",192,192,192},{"LAC",51,153,255},{"LAR",255,255,0},{"MIA",0,128,128},
  {"MIN",76,0,153},{"NE",0,0,255},{"NO",255,180,0},{"NYG",0,0,255},{"NYJ",0,153,0},
  {"PHI",0,102,0},{"PIT",255,204,0},{"SEA",128,255,0},{"SF",255,0,0},{"TB",204,0,0},
  {"TEN",0,34,68},{"WAS",153,0,0}
};

void getTeamColor(const String &abbr, uint8_t &r, uint8_t &g, uint8_t &b) {
  String a = abbr; a.toUpperCase();
  for (int i=0; i<32; i++) {
    if (a == teamColors[i].abbr) { r=teamColors[i].r; g=teamColors[i].g; b=teamColors[i].b; return; }
  }
  r=g=b=200;
}

/* ================= DISPLAY FUNCTIONS ================= */
void displayNFLGame(int idx) {
  Game &g = games[idx];
  String line = g.awayAbbr + ":" + g.awayScore + " - " + g.homeAbbr + ":" + g.homeScore;
  int x = WIDTH, minX = -((int)line.length() * 6);
  while (x > minX && (currentMode == MODE_NFL || currentMode == MODE_CYCLE)) {
    server.handleClient(); yield();
    matrix->fillScreen(0); matrix->setCursor(x, 1);
    matrix->setTextColor(matrix->Color(g.ar, g.ag, g.ab)); matrix->print(g.awayAbbr + ":" + g.awayScore);
    matrix->setTextColor(matrix->Color(255,255,255)); matrix->print(" - ");
    matrix->setTextColor(matrix->Color(g.hr, g.hg, g.hb)); matrix->print(g.homeAbbr + ":" + g.homeScore);
    matrix->show(); x--; delay(scrollDelay);
  }
}

void displayStock(int idx) {
  Stock &s = stocks[idx];
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
  // 1. Cool down the top 4 rows (the flame tips)
  // We cool them faster so the flames don't just stay at the top
  for(int i = 0; i < WIDTH * 4; i++) {
    heat[i] = qsub8(heat[i], random8(0, 40)); 
  }

  // 2. Heat drifts UP (from row 4 toward row 0)
  for(int y = 0; y < 4; y++) {
    for(int x = 0; x < WIDTH; x++) {
      // Pull heat from the row below it
      heat[y * WIDTH + x] = (heat[(y + 1) * WIDTH + x] + heat[(y + 2) * WIDTH + x]) / 2;
    }
  }

  // 3. The "Embers" (Bottom 4 rows)
  // We keep these high-heat but add a tiny flicker so they look alive
  for(int i = WIDTH * 4; i < WIDTH * HEIGHT; i++) {
    heat[i] = random8(140, 220); // Steady orange/red glow
  }

  // 4. Occasional bright "Sparks" from the embers into the flames
  if(random8() < 30) {
    int x = random8(WIDTH);
    heat[4 * WIDTH + x] = 255; // Ignite a bright spot at the log line
  }

  // 5. Draw to the Matrix
  for(int y = 0; y < HEIGHT; y++) {
    for(int x = 0; x < WIDTH; x++) {
      byte colorIndex = heat[y * WIDTH + x];
      CRGB color = HeatColor(colorIndex);
      matrix->drawPixel(x, y, matrix->Color(color.r, color.g, color.b));
    }
  }
  matrix->show();
  delay(40);
}
/* ================= FETCH LOGIC ================= */
void fetchScores() {
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http;
  if (http.begin(client, "https://espnscraper.adamjsmith002.workers.dev/")) {
    if (http.GET() == 200) {
      DynamicJsonDocument doc(32768);
      deserializeJson(doc, http.getString());
      gameCount = 0;
      for (JsonVariant v : doc.as<JsonArray>()) {
        if (gameCount >= MAX_GAMES) break;
        String away = v["away"]["team"] | ""; String home = v["home"]["team"] | "";
        if (away == "" || home == "") continue;
        uint8_t ar,ag,ab, hr,hg,hb;
        getTeamColor(away, ar,ag,ab); getTeamColor(home, hr,hg,hb);
        games[gameCount++] = {away, v["away"]["score"]|"", home, v["home"]["score"]|"", ar,ag,ab, hr,hg,hb};
      }
      lastNFLFetch = millis();
    }
    http.end();
  }
}

void fetchStocks() {
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http;
  if (http.begin(client, "https://stockscraper.adamjsmith002.workers.dev/")) {
    if (http.GET() == 200) {
      DynamicJsonDocument doc(16384);
      deserializeJson(doc, http.getString());
      stockCount = 0;
      for (JsonVariant v : doc.as<JsonArray>()) {
        if (stockCount >= MAX_STOCKS) break;
        stocks[stockCount++] = {v["symbol"]|"", v["price"]|0.0f, v["percent"]|0.0f};
      }
      lastStockFetch = millis();
    }
    http.end();
  }
}

void fetchWeather() {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  // Expanded URL for extra stats
  String url = "https://api.open-meteo.com/v1/forecast?latitude=41.83&longitude=-72.70&current=temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,wind_speed_10m&temperature_unit=fahrenheit&wind_speed_unit=mph";

  if (http.begin(client, url)) {
    if (http.GET() == 200) {
      DynamicJsonDocument doc(1024);
      deserializeJson(doc, http.getString());
      
      localWeather.temp = doc["current"]["temperature_2m"];
      localWeather.feelsLike = doc["current"]["apparent_temperature"];
      localWeather.humidity = doc["current"]["relative_humidity_2m"];
      localWeather.windSpeed = doc["current"]["wind_speed_10m"];
      localWeather.code = doc["current"]["weather_code"];
      
      // Map WMO codes to text
      int c = localWeather.code;
      if (c == 0) localWeather.condition = "CLEAR";
      else if (c <= 3) localWeather.condition = "CLOUDY";
      else if (c <= 48) localWeather.condition = "FOGGY";
      else if (c <= 67) localWeather.condition = "RAIN";
      else if (c <= 77) localWeather.condition = "SNOW";
      else localWeather.condition = "STORM";

      weatherLoaded = true;
      lastWeatherFetch = millis();
    }
    http.end();
  }
}

 void fetchForecastText() {
  HTTPClient http;
  // This is the specific Gridpoint for Bloomfield, CT
  String url = "https://api.weather.gov/gridpoints/BOX/68,91/forecast";
  
  http.begin(url);
  // NWS requires a User-Agent header or it will reject the request
  http.addHeader("User-Agent", "ESP32-Weather-Display"); 
  
  int httpCode = http.GET();
  if (httpCode == 200) {
    String payload = http.getString();
    
    // Use a filter to only parse the first period's detailed forecast
    StaticJsonDocument<200> filter;
    filter["properties"]["periods"][0]["detailedForecast"] = true;
    
    DynamicJsonDocument doc(4096); 
    deserializeJson(doc, payload, DeserializationOption::Filter(filter));
    
    localWeather.summary = doc["properties"]["periods"][0]["detailedForecast"].as<String>();
    Serial.println("Forecast Summary: " + localWeather.summary);
  }
  http.end();
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
    String html = "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<style>*{box-sizing: border-box;} "; // FIX: Ensures padding doesn't make boxes wider
    html += "body{font-family:sans-serif; text-align:center; background:#111; color:#fff; padding:20px;} ";
    html += ".btn, input[type=text], input[type=color]{width:100%; display:block; margin:10px 0; padding:15px; border-radius:8px; border:none; font-size:1.1em;} ";
    html += ".btn{background:#0af; color:#fff; font-weight:bold; cursor:pointer;} ";
    html += ".clear{background:#f44;} ";
    html += "input[type=color]{height:50px; cursor:pointer; background:#333;} ";
    html += "input[type=range]{width:100%; margin:15px 0;}</style></head><body>";
    
    html += "<h2>Matrix Dashboard</h2>";
    html += "<button class='btn' style='background:#f90;' onclick='fetch(\"/cycle\")'>Cycle All Modes</button>";    
    html += "<button class='btn' onclick='fetch(\"/nfl\")'>NFL Mode</button>";
    html += "<button class='btn' onclick='fetch(\"/stocks\")'>Stock Mode</button>";
    html += "<button class='btn' onclick='fetch(\"/weather\")'>Weather Mode</button>";
    html += "<button class='btn' onclick='fetch(\"/fireplace\")'>Fireplace Mode</button>";

    html += "<button class='btn' onclick='fetch(\"/phrases\")'>Phrase Mode</button>";

    html += "<hr><h3>Phrase Settings</h3>";
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
      server.send(200, "text/plain", "OK");
    }
  });

  // ... keep your other handlers (/nfl, /stocks, /phrases, /add, /clear, /brightness, /speed) ...
  server.on("/nfl", [](){ currentMode = MODE_NFL; lastNFLFetch = 0; server.send(200,"text/plain","OK"); });
  server.on("/stocks", [](){ currentMode = MODE_STOCKS; lastStockFetch = 0; server.send(200,"text/plain","OK"); });
  server.on("/weather", [](){ currentMode = MODE_WEATHER; lastStockFetch = 0; server.send(200,"text/plain","OK"); });
  server.on("/fireplace", [](){ currentMode = MODE_FIREPLACE; server.send(200,"text/plain","OK"); });
  server.on("/phrases", [](){ 
    currentMode = MODE_PHRASES; 
    server.send(200,"text/plain","OK"); 
  });
  server.on("/add", []() {
  if (server.hasArg("v") && phraseCount < MAX_PHRASES) {
    // Clean the text before storing it
    String newPhrase = cleanText(server.arg("v"));
    phrases[phraseCount++] = newPhrase;
    Serial.print("[WEB] Added clean phrase: ");
    Serial.println(newPhrase);
    server.send(200, "text/plain", "OK");
  }
});
  server.on("/clear", []() { phraseCount = 0; currentPhrase = 0; server.send(200); });
  server.on("/brightness", [](){ if(server.hasArg("v")){currentBrightness=constrain(server.arg("v").toInt(),1,40); FastLED.setBrightness(currentBrightness);} server.send(200); });
  server.on("/speed", [](){ if(server.hasArg("v")){scrollDelay=constrain(server.arg("v").toInt(),20,150);} server.send(200); });
  server.on("/cycle", [](){ 
    currentMode = MODE_CYCLE; 
    lastNFLFetch = 0; 
    lastStockFetch = 0; 
    lastWeatherFetch = 0;
    server.send(200, "text/plain", "OK"); 
  });
  server.begin();
}

/* ================= SETUP & LOOP ================= */
void setup() {
  Serial.begin(115200);
  FastLED.addLeds<WS2812B, DATA_PIN, GRB>(leds, NUM_LEDS);
  FastLED.setBrightness(currentBrightness);
  matrix = new FastLED_NeoMatrix(leds, WIDTH, HEIGHT, NEO_MATRIX_TOP + NEO_MATRIX_LEFT + NEO_MATRIX_COLUMNS + NEO_MATRIX_ZIGZAG);
  matrix->begin(); 
  matrix->setTextWrap(false);
  
  WiFiManager wm;
  
  // This is the key: It only shows the IP if it's NOT connected to WiFi [cite: 100-101, 225].
  wm.setAPCallback(configModeCallback);

  if (!wm.autoConnect("Ticker-Setup")) {
      Serial.println("Connection Failed");
      ESP.restart();
  }

  // Once it connects, it skips all the IP stuff and just starts the app
  Serial.println("WiFi Connected!");
  Serial.println(WiFi.localIP()); // Still prints to your computer's Serial Monitor for you

  checkForUpdates();
  setupWeb();
}

void loop() {
  server.handleClient();
  yield();

  if (currentMode == MODE_NFL) {
    if (currentGame == 0 && (millis() - lastNFLFetch > 60000 || lastNFLFetch == 0)) fetchScores();
    if (gameCount > 0) { displayNFLGame(currentGame++); if (currentGame >= gameCount) currentGame = 0; }
  } 
  else if (currentMode == MODE_STOCKS) {
    if (currentStock == 0 && (millis() - lastStockFetch > 60000 || lastStockFetch == 0)) fetchStocks();
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
    static int cycleStage = 0; // 0:NFL, 1:Stock, 2:Phrase, 3:Weather
    
    if (cycleStage == 0) {
      if (currentGame == 0 && (millis() - lastNFLFetch > 60000 || lastNFLFetch == 0)) fetchScores();
      if (gameCount > 0) { 
        displayNFLGame(currentGame++); 
        if (currentGame >= gameCount) { currentGame = 0; cycleStage = 1; }
      } else { cycleStage = 1; }
    } 
    else if (cycleStage == 1) {
      if (currentStock == 0 && (millis() - lastStockFetch > 60000 || lastStockFetch == 0)) fetchStocks();
      if (stockCount > 0) { 
        displayStock(currentStock++); 
        if (currentStock >= stockCount) { currentStock = 0; cycleStage = 2; }
      } else { cycleStage = 2; }
    }
    else if (cycleStage == 2) {
      if (phraseCount > 0) { 
        displayPhrase(currentPhrase++); 
        if (currentPhrase >= phraseCount) { currentPhrase = 0; cycleStage = 3; }
      } else { cycleStage = 3; }
    }
    else if (cycleStage == 3) {
      if (millis() - lastWeatherFetch > 900000 || lastWeatherFetch == 0) {
        fetchWeather();
        //fetchForecastText();
      }
      displayWeather();
      cycleStage = 0; // Restart cycle
    }
  }
}