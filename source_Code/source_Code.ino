#include <Wire.h>
#include <SPI.h>
#include "MAX30105.h"
#include "spo2_algorithm.h"
#include "MPU6050.h"
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <HTTPClient.h>

/* ---------------- WIFI ---------------- */
const char* ssid = "YOUR_WIFI_NAME";
const char* password = "YOUR_WIFI_PASSWORD";

/* ---------------- THINGSPEAK ---------------- */
String apiKey = "";
const char* server = "http://api.thingspeak.com/update";

/* ---------------- PINS ---------------- */
#define SDA_PIN 21
#define SCL_PIN 22

#define OLED_MOSI 23
#define OLED_CLK  18
#define OLED_DC   2
#define OLED_RST  4
#define OLED_CS   5

#define ECG_PIN   36
#define LO_PLUS   35
#define LO_MINUS  34

#define BUZZER    27

/* ---------------- OBJECTS ---------------- */
MAX30105 particleSensor;
MPU6050 mpu;
Adafruit_SSD1306 display(128, 64, &SPI, OLED_DC, OLED_RST, OLED_CS);

/* ---------------- STATES ---------------- */
int displayMode = 0;
bool emergency = false;

/* ---------------- SPO2 ---------------- */
#define BUFFER_SIZE 100
uint32_t irBuffer[BUFFER_SIZE];
uint32_t redBuffer[BUFFER_SIZE];

int bufferIndex = 0;
int32_t spo2 = 0, heartRate = 0;
int8_t validSPO2 = 0, validHeartRate = 0;

/* ---------------- ECG ---------------- */
int graphX = 0;
int prevY = 32;
float ecgFiltered = 0;

/* ---------------- TIMERS ---------------- */
unsigned long lastOLED = 0;
unsigned long lastUpload = 0;
unsigned long lastModeChange = 0;

/* ===================================================== */

void connectWiFi()
{
  WiFi.begin(ssid, password);
  Serial.print("Connecting WiFi");

  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nWiFi Connected!");
}

/* ===================================================== */

void sendToThingSpeak(int ecg, int ax, int ay, int az)
{
  if (WiFi.status() == WL_CONNECTED)
  {
    HTTPClient http;

    String url = String(server) + "?api_key=" + apiKey +
                 "&field1=" + String(spo2) +
                 "&field2=" + String(heartRate) +
                 "&field3=" + String(ecg) +
                 "&field4=" + String(ax) +
                 "&field5=" + String(ay) +
                 "&field6=" + String(az);

    http.begin(url);
    int httpCode = http.GET();

    Serial.print("ThingSpeak: ");
    Serial.println(httpCode);

    http.end();
  }
}

/* ===================================================== */

void setup()
{
  Serial.begin(115200);

  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(100000); // stability

  SPI.begin(OLED_CLK, -1, OLED_MOSI, OLED_CS);

  pinMode(OLED_RST, OUTPUT);

  digitalWrite(OLED_RST, LOW);
  delay(50);
  digitalWrite(OLED_RST, HIGH);

  pinMode(BUZZER, OUTPUT);
  pinMode(LO_PLUS, INPUT);
  pinMode(LO_MINUS, INPUT);

  connectWiFi();

  /* OLED INIT */
  if (!display.begin(SSD1306_SWITCHCAPVCC))
  {
    Serial.println("OLED FAIL");
    while (1);
  }

  display.clearDisplay();
  display.setCursor(10, 20);
  display.println("SYSTEM READY");
  display.display();
  delay(1000);

  /* Sensors */
  particleSensor.begin(Wire, I2C_SPEED_STANDARD);
  particleSensor.setup();
  particleSensor.setPulseAmplitudeRed(0x2F);
  particleSensor.setPulseAmplitudeIR(0x2F);

  mpu.initialize();
}

/* ===================================================== */

void readSpO2()
{
  particleSensor.check();

  while (particleSensor.available())
  {
    redBuffer[bufferIndex] = particleSensor.getRed();
    irBuffer[bufferIndex]  = particleSensor.getIR();
    particleSensor.nextSample();

    bufferIndex++;

    if (bufferIndex >= BUFFER_SIZE)
    {
      maxim_heart_rate_and_oxygen_saturation(
        irBuffer, BUFFER_SIZE,
        redBuffer,
        &spo2, &validSPO2,
        &heartRate, &validHeartRate);

      bufferIndex = 0;
    }

    delay(2); // prevent I2C overload
  }
}

/* ===================================================== */

int getECG()
{
  int raw = analogRead(ECG_PIN);

  ecgFiltered = 0.9 * ecgFiltered + 0.1 * raw;

  int centered = ecgFiltered - 2000;
  return centered * 3;
}

/* ===================================================== */

void drawECGGraph()
{
  if (digitalRead(LO_PLUS) || digitalRead(LO_MINUS))
  {
    display.setCursor(20, 30);
    display.print("LEADS OFF");
    return;
  }

  int ecg = getECG();
  int y = map(ecg, -2000, 2000, 63, 0);

  display.drawLine(graphX - 1, prevY, graphX, y, SSD1306_WHITE);

  prevY = y;
  graphX++;

  if (graphX >= 128)
  {
    graphX = 0;
    display.clearDisplay();
  }
}

/* ===================================================== */

void updateOLED()
{
  display.clearDisplay();

  if (displayMode == 0)
  {
    display.setCursor(0,0);
    display.print("SpO2:");
    display.print(validSPO2 ? spo2 : 0);

    display.setCursor(0,10);
    display.print("HR:");
    display.print(validHeartRate ? heartRate : 0);

    display.setCursor(0,20);
    display.print("ECG:");
    display.print(getECG());
  }
  else if (displayMode == 1)
  {
    int16_t ax, ay, az, gx, gy, gz;
    mpu.getMotion6(&ax, &ay, &az, &gx, &gy, &gz);
    delay(5);

    display.setCursor(0,0);
    display.print("AX:"); display.print(ax);

    display.setCursor(0,10);
    display.print("AY:"); display.print(ay);

    display.setCursor(0,20);
    display.print("AZ:"); display.print(az);
  }
  else if (displayMode == 2)
  {
    drawECGGraph();
  }

  display.display();
}

/* ===================================================== */

void loop()
{
  readSpO2();

  /* AUTO MODE CHANGE EVERY 5 SEC */
  if (millis() - lastModeChange > 5000)
  {
    displayMode = (displayMode + 1) % 3;
    display.clearDisplay();
    lastModeChange = millis();

    Serial.print("Auto Mode: ");
    Serial.println(displayMode);
  }

  /* THINGSPEAK SAFE UPLOAD */
  if (millis() - lastUpload > 15000)
  {
    lastUpload = millis();

    if (validSPO2)
    {
      int ecg = getECG();

      int16_t ax, ay, az, gx, gy, gz;
      mpu.getMotion6(&ax, &ay, &az, &gx, &gy, &gz);
      delay(5);

      sendToThingSpeak(ecg, ax, ay, az);
    }
  }

  if (millis() - lastOLED > 25)
  {
    lastOLED = millis();
    updateOLED();
  }
}