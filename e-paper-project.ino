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
#define BTN_NEXT  25
#define BTN_PREV  26

// ── Display ─────────────────────────────────────────────────────
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

// ── BMP Header ──────────────────────────────────────────────────
// Defined BEFORE any function that uses it
struct BmpHeader {
  uint32_t dataOffset;
  int32_t  width, height;
  uint16_t bitDepth;
  uint32_t rowSize;
  bool     flip;
  bool     palette_white[2];
};

// ── Interrupt State ─────────────────────────────────────────────
// Defined AFTER struct, BEFORE ISR functions
volatile int pageStep = 0;
unsigned long lastInterruptTime = 0;
const unsigned long DEBOUNCE_MS = 200;

// ── Interrupt Handlers ───────────────────────────────────────────
void IRAM_ATTR onNextPressed() {
  unsigned long now = millis();
  if (now - lastInterruptTime > DEBOUNCE_MS) {
    lastInterruptTime = now;
    pageStep = 1;
  }
}

void IRAM_ATTR onPrevPressed() {
  unsigned long now = millis();
  if (now - lastInterruptTime > DEBOUNCE_MS) {
    lastInterruptTime = now;
    pageStep = -1;
  }
}

// ── BMP Helpers ─────────────────────────────────────────────────
static uint16_t read16(fs::File &f) {
  uint16_t v; f.read((uint8_t *)&v, 2); return v;
}
static uint32_t read32(fs::File &f) {
  uint32_t v; f.read((uint8_t *)&v, 4); return v;
}

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

  bmp.flip    = (bmp.height > 0);
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
    Serial.println("ERROR: BMP parse failed");
    file.close();
    return false;
  }

  Serial.printf("Page %d  BMP:%dx%d  depth:%d\n",
                pageNum, bmp.width, bmp.height, bmp.bitDepth);

  display.setRotation(3);
  display.setFullWindow();

  uint8_t rowBuffer[1440];
  int32_t lastFileRow = -1;
  int32_t baseY = 0;   // 🔥 tracks vertical offset

  display.firstPage();
  do {
    uint16_t page_h = display.pageHeight();

    for (int32_t y = 0; y < page_h; y++) {

      int32_t screenY = baseY + y;

      if (screenY >= bmp.height) {
        // fill remaining with white
        for (int32_t x = 0; x < display.width(); x++) {
          display.drawPixel(x, screenY, GxEPD_WHITE);
        }
        continue;
      }

      uint32_t fileRow = bmp.flip
                         ? (bmp.height - 1 - screenY)
                         : screenY;

      if (fileRow != (uint32_t)lastFileRow) {
        file.seek(bmp.dataOffset + fileRow * bmp.rowSize);
        file.read(rowBuffer, bmp.rowSize);
        lastFileRow = fileRow;
      }

      for (int32_t x = 0; x < display.width() && x < bmp.width; x++) {

        bool isWhite;
        if (bmp.bitDepth == 1) {
          uint8_t bitVal =
            (rowBuffer[x / 8] >> (7 - (x % 8))) & 1;
          isWhite = bmp.palette_white[bitVal];
        } else {
          uint32_t idx = x * 3;
          isWhite = ((rowBuffer[idx] +
                      rowBuffer[idx + 1] +
                      rowBuffer[idx + 2]) > 384);
        }

        display.drawPixel(
          x,
          screenY,
          isWhite ? GxEPD_WHITE : GxEPD_BLACK
        );
      }
    }

    baseY += page_h;   // 🔥 move to next stripe

  } while (display.nextPage());

  file.close();
  Serial.printf("Page %d done.\n", pageNum);
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
  attachInterrupt(digitalPinToInterrupt(BTN_NEXT), onNextPressed, FALLING);
  attachInterrupt(digitalPinToInterrupt(BTN_PREV), onPrevPressed, FALLING);

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
  Serial.printf("Display size: %d x %d\n", display.width(), display.height());

  drawPage(currentPage);
  display.hibernate();
}

// ── Loop ────────────────────────────────────────────────────────
void loop() {
  int step = pageStep;
  if (step != 0) {
    pageStep = 0;

    int newPage = constrain(currentPage + step, 1, totalPages);
    if (newPage != currentPage) {
      currentPage = newPage;
      saveCurrentPage(currentPage);
      Serial.printf("%s → page %d / %d\n",
                    step > 0 ? "NEXT" : "PREV", currentPage, totalPages);
      drawPage(currentPage);
      display.hibernate();
    }
  }

  delay(15);
}