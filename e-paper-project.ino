/**
 * Waveshare 7.5" e-Paper HAT - Clean E-Book Reader
 * Portrait mode 480x800 BMP pages
 */

#include <LittleFS.h>
#include <GxEPD2_BW.h>
#include <Preferences.h>

// ── Pin Definitions ─────────────────────────────────────────────
#define EPD_CS    5
#define EPD_DC    17
#define EPD_RST   16
#define EPD_BUSY  4

#define BTN_NEXT  12
#define BTN_PREV  13

// ── Display for 7.5" (800x480 physical, we use rotation 0 for portrait) ──
#define MAX_BUFFER_SIZE 65536ul
#define MAX_HEIGHT(EPD) (EPD::HEIGHT <= MAX_BUFFER_SIZE / (EPD::WIDTH / 8) \
                           ? EPD::HEIGHT \
                           : MAX_BUFFER_SIZE / (EPD::WIDTH / 8))

GxEPD2_BW<GxEPD2_750_GDEY075T7, MAX_HEIGHT(GxEPD2_750_GDEY075T7)>
  display(GxEPD2_750_GDEY075T7(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

// ── Global State ────────────────────────────────────────────────
Preferences prefs;
int currentPage = 1;
int totalPages  = 0;

unsigned long lastNextPress = 0;
unsigned long lastPrevPress = 0;
const unsigned long DEBOUNCE_MS = 280;

// ── BMP Header ──────────────────────────────────────────────────
struct BmpHeader {
  uint32_t dataOffset;
  int32_t  width, height;
  uint16_t bitDepth;
  uint32_t rowSize;
  bool     flip;
  bool     palette_white[2];
};

static uint16_t read16(fs::File &f) {
  uint16_t v; f.read((uint8_t *)&v, 2); return v;
}
static uint32_t read32(fs::File &f) {
  uint32_t v; f.read((uint8_t *)&v, 4); return v;
}

// ── Parse BMP Header ────────────────────────────────────────────
bool parseBmpHeader(fs::File &f, BmpHeader &bmp) {
  if (read16(f) != 0x4D42) return false;

  read32(f); read32(f);
  bmp.dataOffset = read32(f);
  read32(f);
  bmp.width    = read32(f);
  bmp.height   = read32(f);
  read16(f);
  bmp.bitDepth = read16(f);
  uint32_t compress = read32(f);
  read32(f); read32(f); read32(f); read32(f); read32(f);

  if (compress != 0) return false;

  bmp.flip = (bmp.height > 0);   // positive height = bottom-up BMP
  if (bmp.height < 0) bmp.height = -bmp.height;
  bmp.rowSize = ((bmp.width * bmp.bitDepth + 31) / 32) * 4;

  bmp.palette_white[0] = true;
  bmp.palette_white[1] = false;

  if (bmp.bitDepth == 1) {
    f.seek(14 + 40, SeekSet);
    for (int i = 0; i < 2; i++) {
      uint8_t b = f.read(), g = f.read(), r = f.read(); f.read();
      bmp.palette_white[i] = ((r + g + b) > 384);
    }
  }
  return true;
}

// ── Count Pages ─────────────────────────────────────────────────
int countPages() {
  int count = 0;
  char path[32];
  while (true) {
    snprintf(path, sizeof(path), "/page_%d.bmp", count + 1);
    if (!LittleFS.exists(path)) break;
    count++;
  }
  return count;
}

// ── Draw Page ───────────────────────────────────────────────────
bool drawPage(int pageNum) {
  char path[32];
  snprintf(path, sizeof(path), "/page_%d.bmp", pageNum);

  fs::File file = LittleFS.open(path, "r");
  if (!file) {
    Serial.printf("ERROR: Cannot open %s\n", path);
    return false;
  }

  BmpHeader bmp;
  if (!parseBmpHeader(file, bmp)) {
    Serial.println("ERROR: BMP header parse failed");
    file.close();
    return false;
  }

  Serial.printf("BMP: %dx%d  Display: %dx%d\n",
                bmp.width, bmp.height, display.width(), display.height());

  display.setRotation(1);    // portrait: display reports 480 wide x 800 tall
  display.setFullWindow();

  uint8_t rowBuffer[1440];   // 480 * 3 bytes max (24-bit row)

  display.firstPage();
  do {

    for (int32_t y = 0; y < display.height(); y++) {

      if (y >= bmp.height) {
        // Fill any extra rows white if BMP is shorter than display
        for (int32_t x = 0; x < display.width(); x++)
          display.drawPixel(x, y, GxEPD_WHITE);
        continue;
      }

      uint32_t fileRow = bmp.flip ? (bmp.height - 1 - y) : y;
      file.seek(bmp.dataOffset + fileRow * bmp.rowSize);
      file.read(rowBuffer, bmp.rowSize);

      for (int32_t x = 0; x < display.width(); x++) {
        bool isWhite;

        if (bmp.bitDepth == 1) {
          uint8_t bitVal = (rowBuffer[x / 8] >> (7 - (x % 8))) & 1;
          isWhite = bmp.palette_white[bitVal];
        } else {
          // 24-bit BGR
          uint32_t idx = x * 3;
          isWhite = ((rowBuffer[idx] + rowBuffer[idx+1] + rowBuffer[idx+2]) > 384);
        }

        display.drawPixel(x, y, isWhite ? GxEPD_WHITE : GxEPD_BLACK);
      }
    }

  } while (display.nextPage());

  file.close();
  Serial.printf("Page %d rendered OK\n", pageNum);
  return true;
}

// ── Save / Load Page ────────────────────────────────────────────
void saveCurrentPage(int page) {
  prefs.begin("ebook", false);
  prefs.putInt("page", page);
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
  delay(150);
  Serial.println("\n=== 7.5\" E-Ink Book Reader ===");

  pinMode(BTN_NEXT, INPUT_PULLUP);
  pinMode(BTN_PREV, INPUT_PULLUP);

  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed!");
    while (1) delay(1000);
  }

  totalPages = countPages();
  if (totalPages == 0) {
    Serial.println("ERROR: No page_*.bmp files found!");
    while (1) delay(1000);
  }

  currentPage = loadCurrentPage();
  currentPage = constrain(currentPage, 1, totalPages);

  display.init(115200, true, 20, false);

  Serial.printf("Resuming at page %d / %d\n", currentPage, totalPages);
  Serial.printf("Display logical size: %d x %d\n", display.width(), display.height());

  drawPage(currentPage);
  display.hibernate();
}

// ── Loop ────────────────────────────────────────────────────────
void loop() {
  unsigned long now = millis();

  if (digitalRead(BTN_NEXT) == LOW && (now - lastNextPress > DEBOUNCE_MS)) {
    lastNextPress = now;
    if (currentPage < totalPages) {
      currentPage++;
      saveCurrentPage(currentPage);
      drawPage(currentPage);
      display.hibernate();
    }
  }

  if (digitalRead(BTN_PREV) == LOW && (now - lastPrevPress > DEBOUNCE_MS)) {
    lastPrevPress = now;
    if (currentPage > 1) {
      currentPage--;
      saveCurrentPage(currentPage);
      drawPage(currentPage);
      display.hibernate();
    }
  }

  delay(15);
}