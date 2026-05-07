#include <LittleFS.h>
#include <FS.h>
#include <SPI.h>
#include <Preferences.h>
#include <GxEPD2_BW.h>
#include <esp_sleep.h>
#include <driver/rtc_io.h>

// ─────────────────────────────────────────────
// PINS
// ─────────────────────────────────────────────
#define EPD_CS      5
#define EPD_DC      17
#define EPD_RST     16
#define EPD_BUSY    4

#define BTN_NEXT    25
#define BTN_PREV    26

// ─────────────────────────────────────────────
// DISPLAY
// ─────────────────────────────────────────────
#define MAX_BUFFER_SIZE 65536ul

#define MAX_HEIGHT(EPD) \
  (EPD::HEIGHT <= MAX_BUFFER_SIZE / (EPD::WIDTH / 8) ? \
   EPD::HEIGHT : \
   MAX_BUFFER_SIZE / (EPD::WIDTH / 8))

GxEPD2_BW<
  GxEPD2_750_GDEY075T7,
  MAX_HEIGHT(GxEPD2_750_GDEY075T7)
> display(
  GxEPD2_750_GDEY075T7(
    EPD_CS,
    EPD_DC,
    EPD_RST,
    EPD_BUSY
  )
);

// ─────────────────────────────────────────────
// RTC DATA
// ─────────────────────────────────────────────
RTC_DATA_ATTR int rtcCurrentPage = 1;

// ─────────────────────────────────────────────
// GLOBALS
// ─────────────────────────────────────────────
Preferences prefs;

volatile int pageDirection = 0;

int currentPage = 1;
int totalPages = 0;

unsigned long lastActivity = 0;
unsigned long wakeTime = 0;

const uint32_t SLEEP_TIMEOUT_MS = 30000;

// Global buffer (avoid stack overflow)
uint8_t rowBuffer[1024];

// ─────────────────────────────────────────────
// BMP HEADER
// ─────────────────────────────────────────────
struct BMPHeader {
  uint32_t dataOffset;
  int32_t width;
  int32_t height;
  uint16_t depth;
  uint32_t rowSize;
  bool flip;
};

// ─────────────────────────────────────────────
// ISR
// ─────────────────────────────────────────────
void IRAM_ATTR nextISR() {
  pageDirection = 1;
}

void IRAM_ATTR prevISR() {
  pageDirection = -1;
}

// ─────────────────────────────────────────────
// UTILS
// ─────────────────────────────────────────────
uint16_t read16(File &f) {
  uint16_t v;
  f.read((uint8_t*)&v, 2);
  return v;
}

uint32_t read32(File &f) {
  uint32_t v;
  f.read((uint8_t*)&v, 4);
  return v;
}

// ─────────────────────────────────────────────
// BMP PARSER
// ─────────────────────────────────────────────
bool parseBMP(File &file, BMPHeader &bmp) {

  if (read16(file) != 0x4D42) {
    Serial.println("Invalid BMP");
    return false;
  }

  read32(file);
  read32(file);

  bmp.dataOffset = read32(file);

  read32(file);

  bmp.width = read32(file);
  bmp.height = read32(file);

  read16(file);

  bmp.depth = read16(file);

  uint32_t compression = read32(file);

  if (compression != 0) {
    Serial.println("Compressed BMP unsupported");
    return false;
  }

  bmp.flip = bmp.height > 0;

  if (bmp.height < 0)
    bmp.height = -bmp.height;

  bmp.rowSize = ((bmp.width * bmp.depth + 31) / 32) * 4;

  return true;
}

// ─────────────────────────────────────────────
// COUNT PAGES
// ─────────────────────────────────────────────
int countPages() {

  int count = 0;
  char path[32];

  while (true) {

    sprintf(path, "/page_%d.bmp", count + 1);

    if (!LittleFS.exists(path))
      break;

    count++;
  }

  return count;
}

// ─────────────────────────────────────────────
// DRAW PAGE
// ─────────────────────────────────────────────
bool drawPage(int pageNum) {

  char path[32];
  sprintf(path, "/page_%d.bmp", pageNum);

  File file = LittleFS.open(path, "r");

  if (!file) {
    Serial.println("Page open failed");
    return false;
  }

  BMPHeader bmp;

  if (!parseBMP(file, bmp)) {
    file.close();
    return false;
  }

  Serial.printf(
    "BMP %dx%d depth:%d\n",
    bmp.width,
    bmp.height,
    bmp.depth
  );

  if (bmp.depth != 1) {
    Serial.println("Only 1-bit BMP supported");
    file.close();
    return false;
  }

  display.setRotation(3);
  display.setFullWindow();

  uint32_t rowPosition;

  display.firstPage();

  do {

    for (int32_t y = 0; y < bmp.height; y++) {

      rowPosition = bmp.flip ?
        (bmp.height - 1 - y) :
        y;

      file.seek(
        bmp.dataOffset +
        rowPosition * bmp.rowSize
      );

      file.read(rowBuffer, bmp.rowSize);

      for (int32_t x = 0; x < bmp.width; x++) {

        bool white =
          rowBuffer[x / 8] &
          (0x80 >> (x % 8));

        display.drawPixel(
          x,
          y,
          white ? GxEPD_WHITE : GxEPD_BLACK
        );
      }
    }

  } while (display.nextPage());

  file.close();

  Serial.printf(
    "Rendered page %d\n",
    pageNum
  );

  return true;
}

// ─────────────────────────────────────────────
// SAVE STATE
// ─────────────────────────────────────────────
void savePage() {

  rtcCurrentPage = currentPage;

  prefs.begin("ebook", false);

  prefs.putInt("page", currentPage);

  prefs.end();
}

// ─────────────────────────────────────────────
// LOAD STATE
// ─────────────────────────────────────────────
void loadPage() {

  prefs.begin("ebook", true);

  currentPage =
    prefs.getInt("page", rtcCurrentPage);

  prefs.end();

  currentPage =
    constrain(currentPage, 1, totalPages);
}

// ─────────────────────────────────────────────
// DEEP SLEEP
// ─────────────────────────────────────────────
void enterSleep() {

  Serial.println("Entering deep sleep");

  display.hibernate();

  rtc_gpio_init(GPIO_NUM_25);
  rtc_gpio_init(GPIO_NUM_26);

  rtc_gpio_set_direction(
    GPIO_NUM_25,
    RTC_GPIO_MODE_INPUT_ONLY
  );

  rtc_gpio_set_direction(
    GPIO_NUM_26,
    RTC_GPIO_MODE_INPUT_ONLY
  );

  rtc_gpio_pullup_en(GPIO_NUM_25);
  rtc_gpio_pullup_en(GPIO_NUM_26);

  uint64_t mask =
    (1ULL << BTN_NEXT) |
    (1ULL << BTN_PREV);

  esp_sleep_enable_ext1_wakeup(
    mask,
    ESP_EXT1_WAKEUP_ALL_LOW
  );

  delay(100);

  esp_deep_sleep_start();
}

// ─────────────────────────────────────────────
// SETUP
// ─────────────────────────────────────────────
void setup() {

  Serial.begin(115200);

  delay(200);

  wakeTime = millis();

  pinMode(BTN_NEXT, INPUT_PULLUP);
  pinMode(BTN_PREV, INPUT_PULLUP);

  attachInterrupt(
    BTN_NEXT,
    nextISR,
    FALLING
  );

  attachInterrupt(
    BTN_PREV,
    prevISR,
    FALLING
  );

  // Explicit SPI init
  SPI.begin(
    18,   // SCK
    -1,   // MISO
    23,   // MOSI
    EPD_CS
  );

  pinMode(EPD_BUSY, INPUT);

  if (!LittleFS.begin(true)) {

    Serial.println("LittleFS failed");

    while (1)
      delay(1000);
  }

  totalPages = countPages();

  if (totalPages == 0) {

    Serial.println("No pages found");

    while (1)
      delay(1000);
  }

  loadPage();

  Serial.printf(
    "Pages: %d Current: %d\n",
    totalPages,
    currentPage
  );

  display.init(
    115200,
    true,
    20,
    false
  );

  delay(200);

  drawPage(currentPage);

  display.hibernate();

  lastActivity = millis();
}

// ─────────────────────────────────────────────
// LOOP
// ─────────────────────────────────────────────
void loop() {

  if (
    millis() - wakeTime <
    500
  ) {
    delay(10);
    return;
  }

  int dir = pageDirection;

  if (dir != 0) {

    pageDirection = 0;

    int newPage =
      constrain(
        currentPage + dir,
        1,
        totalPages
      );

    if (newPage != currentPage) {

      currentPage = newPage;

      Serial.printf(
        "Page %d\n",
        currentPage
      );

      savePage();

      drawPage(currentPage);

      display.hibernate();
    }

    lastActivity = millis();
  }

  if (
    millis() - lastActivity >
    SLEEP_TIMEOUT_MS
  ) {

    enterSleep();
  }

  delay(20);
}