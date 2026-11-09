#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
#define SCREEN_ADDRESS 0x3C // Change to 0x3D if needed

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
WebServer server(80);

// --- WIFI CREDENTIALS (REPLACE BEFORE FLASHING) ---
const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";

String rawLine1 = "Ready";
String rawLine2 = "Waiting for music...";
String paddedLine1 = "";
String paddedLine2 = "";

int scrollPos1 = 0;
int scrollPos2 = 0;
unsigned long lastScrollTime = 0;
unsigned long lineArrivalTime = 0;

// --- SCROLL SPEED CONTROLS ---
const unsigned long scrollInterval = 55;    // Higher = Slower scrolling (ms)
const unsigned long scrollStartDelay = 700; // Pause 700ms on a new line before scrolling starts
const int scrollStep = 1;                   // Shift 1 pixel at a time

unsigned long bootTime = 0;
bool showingSplash = true;
int animFrame = 0;
unsigned long lastAnimTime = 0;

const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <title>ESP32 OLED Lyrics</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    body { font-family: Arial, sans-serif; text-align: center; margin-top: 40px; background: #181818; color: #fff; }
    .card { background: #242424; padding: 25px; border-radius: 10px; display: inline-block; box-shadow: 0 4px 10px rgba(0,0,0,0.5); }
    input[type=text] { width: 280px; padding: 12px; margin: 10px 0; border: 1px solid #444; border-radius: 6px; background: #121212; color: #fff; font-size: 16px; box-sizing: border-box; }
    button { padding: 12px 24px; background: #007acc; border: none; color: white; border-radius: 6px; cursor: pointer; font-size: 16px; font-weight: bold; width: 100%; }
    button:hover { background: #005999; }
    #status { margin-top: 15px; font-weight: bold; color: #4caf50; }
  </style>
</head>
<body>
  <div class="card">
    <h2>Live OLED Controller</h2>
    <input type="text" id="line1" placeholder="Line 1 text..."><br>
    <input type="text" id="line2" placeholder="Line 2 text..."><br>
    <button onclick="sendText()">Send to OLED</button>
    <div id="status"></div>
  </div>
  <script>
    function sendText() {
      const l1 = document.getElementById('line1').value;
      const l2 = document.getElementById('line2').value;
      const status = document.getElementById('status');
      status.style.color = '#ff9800';
      status.innerText = 'Sending...';
      const formData = new URLSearchParams();
      formData.append('line1', l1);
      formData.append('line2', l2);
      fetch('/update', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: formData.toString()
      }).then(res => {
        if (res.ok) {
          status.style.color = '#4caf50';
          status.innerText = '✓ OLED Updated!';
          setTimeout(() => { status.innerText = ''; }, 3000);
        }
      });
    }
  </script>
</body>
</html>
)rawliteral";

void drawHeart(int x, int y, bool big) {
  if (big) {
    display.fillCircle(x + 3, y + 3, 3, SSD1306_WHITE);
    display.fillCircle(x + 8, y + 3, 3, SSD1306_WHITE);
    display.fillTriangle(x, y + 4, x + 11, y + 4, x + 5, y + 10, SSD1306_WHITE);
  } else {
    display.fillCircle(x + 4, y + 4, 2, SSD1306_WHITE);
    display.fillCircle(x + 7, y + 4, 2, SSD1306_WHITE);
    display.fillTriangle(x + 2, y + 5, x + 9, y + 5, x + 5, y + 9, SSD1306_WHITE);
  }
}

void drawEqualizer(int x, int y, int frame) {
  int heights[4][3] = {
    {3, 9, 5},
    {7, 3, 10},
    {10, 6, 4},
    {4, 10, 7}
  };
  for (int i = 0; i < 3; i++) {
    int h = heights[frame % 4][i];
    display.fillRect(x + (i * 4), y + (10 - h), 2, h, SSD1306_WHITE);
  }
}

void updatePaddedStrings() {
  paddedLine1 = rawLine1 + "      ";
  paddedLine2 = rawLine2 + "      ";
  scrollPos1 = 0;
  scrollPos2 = 0;
  lineArrivalTime = millis();
}

void updateOLEDDisplay(bool advanceScroll) {
  display.clearDisplay();
  display.setTextWrap(false);
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  bool canScroll = advanceScroll && (millis() - lineArrivalTime >= scrollStartDelay);

  // --- LINE 1 (Active Lyric - Top Box, Y = 12) ---
  int w1 = rawLine1.length() * 6;
  if (w1 <= 104) {
    display.setCursor(2, 12);
    display.print(rawLine1);
  } else {
    int padW1 = paddedLine1.length() * 6;
    display.setCursor(2 - scrollPos1, 12);
    display.print(paddedLine1);
    display.setCursor(2 - scrollPos1 + padW1, 12);
    display.print(paddedLine1);
    if (canScroll) {
      scrollPos1 = (scrollPos1 + scrollStep) % padW1;
    }
  }

  // --- LINE 2 (Next Lyric - Bottom Box, Y = 44) ---
  int w2 = rawLine2.length() * 6;
  if (w2 <= 104) {
    display.setCursor(2, 44);
    display.print(rawLine2);
  } else {
    int padW2 = paddedLine2.length() * 6;
    display.setCursor(2 - scrollPos2, 44);
    display.print(paddedLine2);
    display.setCursor(2 - scrollPos2 + padW2, 44);
    display.print(paddedLine2);
    if (canScroll) {
      scrollPos2 = (scrollPos2 + scrollStep) % padW2;
    }
  }

  // --- MASKING & UI BORDERS ---
  display.fillRect(108, 0, 20, 64, SSD1306_BLACK);
  display.drawFastVLine(109, 0, 64, SSD1306_WHITE);
  display.drawFastHLine(0, 31, 109, SSD1306_WHITE);

  drawHeart(113, 10, (animFrame % 2 == 0));
  drawEqualizer(114, 42, animFrame);

  display.display();
}

void handleRoot() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "text/html", index_html);
}

void handleUpdate() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "POST,GET,OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");

  if (server.hasArg("line1")) rawLine1 = server.arg("line1");
  if (server.hasArg("line2")) rawLine2 = server.arg("line2");

  updatePaddedStrings();
  showingSplash = false;
  updateOLEDDisplay(false);
  lastScrollTime = millis();

  server.send(200, "text/plain", "OK");
}

void handleOptions() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "POST,GET,OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
  server.send(204);
}

void setup() {
  Serial.begin(115200);
  Wire.begin(21, 22);
  Wire.setClock(400000);

  if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println(F("SSD1306 allocation failed"));
    for (;;);
  }

  display.clearDisplay();
  display.setTextWrap(false);
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(4, 28);
  display.println("Connecting WiFi...");
  display.display();

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false); // Prevents Wi-Fi modem sleep lag
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  display.clearDisplay();
  display.drawRect(0, 0, 128, 64, SSD1306_WHITE);
  display.setCursor(10, 12);
  display.println("ESP32 OLED SERVER");
  display.setCursor(10, 30);
  display.println("IP Address:");
  display.setCursor(10, 44);
  display.println(WiFi.localIP());
  display.display();

  bootTime = millis();
  showingSplash = true;

  updatePaddedStrings();

  server.on("/", HTTP_GET, handleRoot);
  server.on("/update", HTTP_POST, handleUpdate);
  server.on("/update", HTTP_GET, handleUpdate);
  server.on("/update", HTTP_OPTIONS, handleOptions);
  server.begin();
}

void loop() {
  server.handleClient();

  if (showingSplash) {
    if (millis() - bootTime >= 10000) {
      showingSplash = false;
    }
    return;
  }

  if (millis() - lastAnimTime >= 300) {
    lastAnimTime = millis();
    animFrame = (animFrame + 1) % 4;
  }

  if (millis() - lastScrollTime >= scrollInterval) {
    lastScrollTime = millis();
    updateOLEDDisplay(true);
  }
}
