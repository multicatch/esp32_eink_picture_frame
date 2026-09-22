#include <GxEPD2_7C.h>

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>

//
// EPD/eInk config
//
#define EPD_SDI   10
#define EPD_SCK   4
#define EPD_CS    3
#define EPD_DC    2
#define EPD_RES   1
#define EPD_BUSY  0

//
// Maximum image chunk size (in single BLE packet)
// A chunk contains line number (2 bytes) on the screen + the line image data (in Spectra 6 raw binary format)
// Ensure the chunk is big enough to store a line (in case of an 800x480 screen, a 800px line will take 400B)
//
#define CHUNK_SIZE 402

// Status LED
#define LED_PIN   8

//
// The ESP32 will start up, wait PENDING_BLUETOOTH_TIMEOUT and then go to sleep for SLEEP_TIME to save battery
//
// how long to wait for bluetooth connection before going to sleep
#define PENDING_BLUETOOTH_TIMEOUT 30000
// how long to sleep
#define SLEEP_TIME                2
// assume TIME_TO_SLEEP is minutes
#define SLEEP_TIME_CONV_FACTOR    60*1000000ULL

//
// GxEPD2 initialization
// This initialization is specific for Spectra 6 GDEP073E01 (a 6-color 7.3" eInk screen)
//
GxEPD2_7C<
    GxEPD2_730c_GDEP073E01,
    GxEPD2_730c_GDEP073E01::HEIGHT / 6
> display(
    GxEPD2_730c_GDEP073E01(
        EPD_CS,
        EPD_DC,
        EPD_RES,
        EPD_BUSY
    )
);

//
// Bluetooth utils
//

#define BLE_DEV_NAME "ESP32 Picture Frame"
// id of main BLE GATT service
#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
// id of image transfer BLE GATT service
#define IMAGE_TRANSFER_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"
// id of lines written feedback (a BLE GATT service)
#define LINES_WRITTEN_UUID  "beb5483e-36e1-4688-b7f5-ea07361b26a9"

BLEServer* bleServer;
BLECharacteristic* imageTransferCharacteristic;
BLECharacteristic* linesWrittenCharacteristic;

bool deviceConnected = false;
uint16_t linesWritten = 0;
int64_t deviceDisconnectTime = 0;

class BLEConnectionCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* server) override {

    deviceConnected = true;
    linesWritten = 0;

    Serial.println("BLE client connected");
  }

  void onDisconnect(BLEServer* server) override {
    deviceConnected = false;
    deviceDisconnectTime = millis();

    digitalWrite(LED_PIN, HIGH);
    Serial.println("BLE client disconnected");

    delay(100);
    server->startAdvertising();
    Serial.println("Advertising restarted");
  }
};

class LinesWrittenCallbacks : public BLECharacteristicCallbacks {
  void onRead(BLECharacteristic* characteristic) override {
    uint8_t bytes[2];

    bytes[0] = linesWritten & 0xFF;
    bytes[1] = (linesWritten >> 8) & 0xFF;

    characteristic->setValue(bytes, sizeof(bytes));
  }
};

class ImageTransferCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* characteristic) override {
    String value = characteristic->getValue();

    char data[CHUNK_SIZE] = { 0 };
    size_t length = value.length() < CHUNK_SIZE ? value.length() : CHUNK_SIZE;
    memcpy(data, value.c_str(), length); // fucking null-terminated strings, to not use toCharArray!

    Serial.print("Received ");
    Serial.print(length);
    Serial.println(" bytes");

    // Process this raw chunk
    processChunk(data, length);
  }

  void processChunk(const char* data, size_t length) {
    //Serial.print("HEX: ");

    const int16_t dh = display.height();
    const int16_t dw = display.width();

    int lineWidth = dw / 2; // 4bnpp

    uint8_t offset = 2;
    uint16_t expectedLength = lineWidth + offset;
    if (expectedLength != length) {
        Serial.print("Rejecting data, it has length ");
        Serial.print(length);
        Serial.print(", expected ");
        Serial.println(expectedLength);
        return; // rejecting this buffer
    }

    uint8_t line[lineWidth] = { 0 };

    uint16_t lineNo = (((uint16_t) data[0] & 0xFF) << 8) + ((uint16_t) data[1] & 0xFF);

    Serial.print("Writing line: ");
    Serial.println(lineNo);

    for (size_t i = offset; i < length; i++) {
      line[i - offset] = (uint8_t) (data[i] & 0xFF);
    }

    display.writeNative(
        /* data1 = */ line, 
        /* data2 = */ nullptr,
        /* x = */ 0, 
        /* y = */ lineNo, 
        /* w = */ dw, 
        /* h = */ 1,
        /* invert = */ false,
        /* mirror_y = */ false,
        /* pgm = */ false
    );
    Serial.print("Line written: ");
    Serial.println(lineNo);

    // blink LED to show transfer is ongoing
    if ((lineNo % 2) == 0) {
      digitalWrite(LED_PIN, LOW);
    } else {
      digitalWrite(LED_PIN, HIGH);
    }

    linesWritten++;

    if (linesWritten == dh) {
        digitalWrite(LED_PIN, HIGH);
        Serial.println("Refreshing display");
        display.refresh();
    }
  }
};

void setup() {
  Serial.begin(115200);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH); // reset the LED

  initDisplay();
  initBLE();
}

void initDisplay() {
  SPI.begin(
      EPD_SCK,
      -1,   // MISO
      EPD_SDI,
      EPD_CS
  );

  display.init(115200);
  display.setRotation(0);
  display.setFullWindow();
}

void initBLE() {
  BLEDevice::init(BLE_DEV_NAME);

  // needed for image transfer
  BLEDevice::setMTU(CHUNK_SIZE);

  bleServer = BLEDevice::createServer();
  bleServer->setCallbacks(new BLEConnectionCallbacks());

  BLEService* service = bleServer->createService(SERVICE_UUID);

  imageTransferCharacteristic = service->createCharacteristic(
    IMAGE_TRANSFER_UUID,
    BLECharacteristic::PROPERTY_WRITE |
    BLECharacteristic::PROPERTY_WRITE_NR
  );

  imageTransferCharacteristic->setCallbacks(new ImageTransferCallbacks());

  linesWrittenCharacteristic = service->createCharacteristic(
    LINES_WRITTEN_UUID,
    BLECharacteristic::PROPERTY_READ
  );

  linesWrittenCharacteristic->setCallbacks(new LinesWrittenCallbacks());

  deviceDisconnectTime = millis();
  service->start();

  BLEAdvertising* advertising = BLEDevice::getAdvertising();
  advertising->setName(BLE_DEV_NAME);
  advertising->addServiceUUID(SERVICE_UUID);
  advertising->setScanResponse(true);

  BLEDevice::startAdvertising();

  Serial.println("BLE server started");
  Serial.println("Waiting for a connection...");
}

void loop() {
  delay(10);

  if (!deviceConnected && elapsed(deviceDisconnectTime, PENDING_BLUETOOTH_TIMEOUT)) {
    Serial.flush();
    esp_sleep_enable_timer_wakeup(SLEEP_TIME * SLEEP_TIME_CONV_FACTOR);
    esp_deep_sleep_start();
  }
}

bool elapsed(int64_t start, uint64_t delay) {
  int64_t now = millis();
  int64_t end = start + delay;
  if (end < start) { // overflow
    return now <= start && now > end;
  } else {
    return now > end;
  }
}

