#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BMP280.h>
#include <DHT.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#define OLED_SDA 10
#define OLED_SCL 11
#define BMP_SDA 1
#define BMP_SCL 2
#define DHTPIN 3
#define DHTTYPE DHT11
TwoWire I2CBMP = TwoWire(1); 
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
Adafruit_BMP280 bmp(&I2CBMP);
DHT dht(DHTPIN, DHTTYPE);
float pressureHistory[6]; // Holds 6 readings (1 hour of data at 10 min intervals)
int historyIndex = 0;
bool isCalibrated = false; 
unsigned long lastReadingTime = 0;
const unsigned long READING_INTERVAL = 600000; 

void setup() {
  Serial.begin(115200);

  
  Wire.begin(OLED_SDA, OLED_SCL);
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("SSD1306 OLED failed"));
    while(1);
  }
  display.clearDisplay();
  display.setTextColor(WHITE);

  
  I2CBMP.begin(BMP_SDA, BMP_SCL);
  if (!bmp.begin(0x76)) { // 0x76 is standard, try 0x77 if it fails
    Serial.println(F("BMP280 failed"));
    while(1);
  }

  
  dht.begin();

  
  float startPressure = bmp.readPressure() / 100.0F; // Convert Pa to hPa
  for (int i = 0; i < 6; i++) {
    pressureHistory[i] = startPressure;
  }
  
  updateScreen("Calibrating...", startPressure, dht.readTemperature(), dht.readHumidity());
}

void loop() {
  unsigned long currentMillis = millis();

 
  if (currentMillis - lastReadingTime >= READING_INTERVAL) {
    lastReadingTime = currentMillis;

    
    float currentTempC = dht.readTemperature();
    float currentHum = dht.readHumidity();
    float currentPressure = bmp.readPressure() / 100.0F; 

    
    float pastPressure = pressureHistory[historyIndex];
    float trend = currentPressure - pastPressure; 

    
    pressureHistory[historyIndex] = currentPressure;
    historyIndex++;
    if (historyIndex >= 6) {
      historyIndex = 0;
      isCalibrated = true; 
    }

    
    String forecast = generateForecast(trend, currentPressure, currentTempC, currentHum);
    
    if (!isCalibrated) {
      forecast = "Gathering Data...";
    }

    
    updateScreen(forecast, currentPressure, currentTempC, currentHum);
  }
}

String generateForecast(float trend, float pressure, float temp, float humidity) {
  
  if (trend <= -1.5) {
    if (temp <= 0.0 && humidity > 70) return "SNOW STORM!";
    if (humidity > 70) return "HEAVY STORM!";
    return "HIGH WINDS";
  } 
  
  else if (trend < -0.5) {
    if (temp <= 0.0) return "LIGHT SNOW";
    return "RAIN LIKELY";
  } 
  
  else if (trend >= 0.5) {
    return "CLEARING UP";
  } 
  
  else {
    if (pressure > 1015.0) return "CLEAR / SUNNY";
    return "CLOUDY / STEADY";
  }
}

void updateScreen(String forecast, float pressure, float temp, float hum) {
  display.clearDisplay();
  
 
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("FORECAST (6-12HR):");
  display.setTextSize(2);
  display.println(forecast);

 
  display.setTextSize(1);
  display.setCursor(0, 35);
  display.print("Temp: ");
  display.print(temp);
  display.println(" C");
  
  display.print("Hum:  ");
  display.print(hum);
  display.println(" %");

  display.print("Pres: ");
  display.print(pressure);
  display.println(" hPa");

  display.display();
}