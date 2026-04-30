/**
 * Waveshare 4.26" e-Paper (GDEQ0426T82) - Optimized E-Book Reader
 * ESP32 + GxEPD2 + LittleFS
 * Fixed paged drawing - April 2026 version
 */

#include <LittleFS.h>
#include <GxEPD2_BW.h>
#include <Preferences.h>

// ── Pins ────────────────────────────────────────────────────────
#define EPD_CS    5
#define EPD_DC    17
#define EPD_RST   16
#define EPD_BUSY  4

#define BTN_NEXT  12
#define BTN_PREV  13

// ── Display ─────────────────────────────────────────────────────
#define MAX_BUFFER_SIZE 65536ul
#define MAX_HEIGHT(EPD) (EPD::HEIGHT <= MAX_BUFFER_SIZE / (EPD::WIDTH / 8) \
                        ? EPD::HEIGHT : MAX_BUFFER_SIZE / (EPD::WIDTH / 8))

GxEPD2_BW<GxEPD2_426_GDEQ0426T82, MAX_HEIGHT(GxEPD2_426_GDEQ0426T82)> 
  display(GxEPD2_426_GDEQ0426T82(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

// ── Globals ─────────────────────────────────────────────────────
Preferences prefs;
int currentPage = 1;
int totalPages  = 0;

unsigned long lastNext = 0;
unsigned long lastPrev = 0;
const unsigned long DEBOUNCE = 280;

// ── BMP Header ──────────────────────────────────────────────────
struct BmpHeader {
  uint32_t dataOffset;
  int32_t  width, height;
  uint16_t bitDepth;
  uint32_t rowSize;
  bool     flip;
  bool     palette_white[2];
};

static uint16_t read16(fs::File &f) { uint16_t v; f.read((uint8_t*)&v, 2); return v; }
static uint32_t read32(fs::File &f) { uint32_t v; f.read((uint8_t*)&v, 4); return v; }

// ── Count Pages ─────────────────────────────────────────────────
int countPages() {
  int n = 0;
  char path[32];
  while (true) {
    snprintf(path, sizeof(path), "/page_%d.bmp", n+1);
    if (!LittleFS.exists(path)) break;
    n++;
  }
  return n;
}

// ── Parse BMP ───────────────────────────────────────────────────
bool parseBmpHeader(fs::File &f, BmpHeader &bmp) {
  if (read16(f) != 0x4D42) return false;

  read32(f); read32(f);                    // size + reserved
  bmp.dataOffset = read32(f);
  read32(f);                               // DIB size
  bmp.width  = read32(f);
  bmp.height = read32(f);
  read16(f);                               // planes
  bmp.bitDepth = read16(f);
  uint32_t compress = read32(f);
  read32(f); read32(f); read32(f); read32(f); read32(f);

  if (compress != 0) return false;

  bmp.flip = (bmp.height > 0);
  if (bmp.height < 0) bmp.height = -bmp.height;
  bmp.rowSize = ((bmp.width * bmp.bitDepth + 31) / 32) * 4;

  // Simple 1-bit palette
  bmp.palette_white[0] = true;
  bmp.palette_white[1] = false;
  if (bmp.bitDepth == 1) {
    f.seek(14 + 40, SeekSet);               // Go to palette
    for (int i = 0; i < 2; i++) {
      uint8_t b = f.read(), g = f.read(), r = f.read(); f.read();
      bmp.palette_white[i] = ((r + g + b) > 384);   // ~0x80 * 1.5 threshold
    }
  }
  return true;
}

// ── Corrected Draw Page ─────────────────────────────────────────
bool drawPage(int pageNum) {
  char path[32];
  snprintf(path, sizeof(path), "/page_%d.bmp", pageNum);

  fs::File file = LittleFS.open(path, "r");
  if (!file) {
    Serial.printf("Failed to open %s\n", path);
    return false;
  }

  BmpHeader bmp;
  if (!parseBmpHeader(file, bmp)) {
    Serial.println("BMP header parse failed");
    file.close();
    return false;
  }

  Serial.printf("Drawing page %d (%dx%d, %dbit)\n", pageNum, bmp.width, bmp.height, bmp.bitDepth);

  display.setRotation(1);
  display.setFullWindow();

  uint8_t rowBuffer[2400];        // max for 800px 24-bit
  int16_t lastFileRow = -1;

  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);

    // Correct way to get current stripe range
    uint16_t pageHeight = display.pageHeight();
    uint16_t yStart = display.getCursorY();           // GxEPD2 sets this for current page
    uint16_t yEnd   = yStart + pageHeight;

    for (uint16_t y = yStart; y < yEnd && y < (uint16_t)bmp.height; y++) {
      uint32_t fileRow = bmp.flip ? (bmp.height - 1 - y) : y;

      if ((int16_t)fileRow != lastFileRow) {
        file.seek(bmp.dataOffset + fileRow * bmp.rowSize);
        file.read(rowBuffer, bmp.rowSize);
        lastFileRow = fileRow;
      }

      for (uint16_t x = 0; x < (uint16_t)bmp.width; x++) {
        bool white;
        if (bmp.bitDepth == 1) {
          uint8_t byte = rowBuffer[x / 8];
          uint8_t bit  = (byte >> (7 - (x % 8))) & 1;
          white = bmp.palette_white[bit];
        } else { // 24-bit BGR
          uint32_t idx = x * 3;
          uint8_t b = rowBuffer[idx];
          uint8_t g = rowBuffer[idx+1];
          uint8_t r = rowBuffer[idx+2];
          white = (r + g + b) > 384;        // good threshold for most images
        }
        display.drawPixel(x, y, white ? GxEPD_WHITE : GxEPD_BLACK);
      }
    }
  } while (display.nextPage());

  file.close();
  return true;
}

// ── Indicator ───────────────────────────────────────────────────
void showLoading(int page, int total) {
  display.setRotation(1);
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);
    display.setTextSize(2);
    display.setCursor(90, 210);
    display.printf("Page %d / %d", page, total);
  } while (display.nextPage());
}

// ── Preferences ─────────────────────────────────────────────────
void saveCurrentPage(int p) {
  prefs.begin("ebook", false);
  prefs.putInt("page", p);
  prefs.end();
}

int loadCurrentPage() {
  prefs.begin("ebook", true);
  int p = prefs.getInt("page", 1);
  prefs.end();
  return p;
}

// ── Setup ───────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(BTN_NEXT, INPUT_PULLUP);
  pinMode(BTN_PREV, INPUT_PULLUP);

  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed!");
    while(1) delay(1000);
  }

  totalPages = countPages();
  if (totalPages == 0) {
    Serial.println("No BMP pages found!");
    while(1) delay(1000);
  }

  currentPage = loadCurrentPage();
  currentPage = constrain(currentPage, 1, totalPages);

  display.init(115200, true, 2, false);   // Important for Waveshare HAT

  Serial.printf("Starting E-Book Reader - %d pages, resuming at %d\n", totalPages, currentPage);

  showLoading(currentPage, totalPages);
  drawPage(currentPage);
  display.hibernate();
}

// ── Loop ────────────────────────────────────────────────────────
void loop() {
  unsigned long now = millis();

  if (digitalRead(BTN_NEXT) == LOW && (now - lastNext > DEBOUNCE)) {
    lastNext = now;
    if (currentPage < totalPages) {
      currentPage++;
      saveCurrentPage(currentPage);
      showLoading(currentPage, totalPages);
      drawPage(currentPage);
      display.hibernate();
    }
  }

  if (digitalRead(BTN_PREV) == LOW && (now - lastPrev > DEBOUNCE)) {
    lastPrev = now;
    if (currentPage > 1) {
      currentPage--;
      saveCurrentPage(currentPage);
      showLoading(currentPage, totalPages);
      drawPage(currentPage);
      display.hibernate();
    }
  }

  delay(15);
}