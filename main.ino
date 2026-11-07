/**
 * =========================================================================================
 * Project: An IoT-Based Plant Watering System for Controlled Irrigation Based on 
 *          Plant-Specific Requirements
 * 
 * Authors:
 *   - Ayush Kumar Jha (ayushkumarjha189@gmail.com)
 *   - Saif Ahmed (saifamd65@gmail.com)
 *   - Dev Dharra (Umedsingh9718@gmail.com)
 *   - Ankur Dhankher (ankurdhankher123@gmail.com)
 * Department of Computer Science and Engineering, MRIIRS, India.
 * 
 * Description:
 *   Production-grade embedded firmware implementing closed-loop plant-specific 
 *   smart irrigation. Supports multi-channel soil moisture sensing, plant-specific
 *   threshold comparison (Succulent, Ornamental Rose, Leafy Vegetables/Tulsi), 
 *   relay-driven pump and solenoid valve actuation, flow tracking, safety timeout
 *   protection, and Wi-Fi IoT telemetry via Blynk / ThingSpeak cloud dashboards.
 * =========================================================================================
 */

#include <Arduino.h>

// -----------------------------------------------------------------------------------------
// Board & IoT Platform Configuration
// -----------------------------------------------------------------------------------------
#define BLYNK_TEMPLATE_ID   "TMPL_SAMPLE_ID"
#define BLYNK_TEMPLATE_NAME "SmartIrrigation"
#define BLYNK_AUTH_TOKEN    "YOUR_BLYNK_AUTH_TOKEN"

// Wi-Fi Credentials
const char* ssid     = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";

// Optional ThingSpeak Channel Details
const char* thingSpeakApiKey = "YOUR_THINGSPEAK_WRITE_API_KEY";
const unsigned long thingSpeakChannelId = 0; // Set to channel ID if using ThingSpeak

#if defined(ESP32)
  #include <WiFi.h>
  #include <BlynkSimpleEsp32.h>
#elif defined(ESP8266)
  #include <ESP8266WiFi.h>
  #include <BlynkSimpleEsp8266.h>
#else
  // Standalone / Arduino Uno fallback (Serial Telemetry mode)
  #warning "Compiling in Standalone Arduino Mode without Native Wi-Fi library."
#endif

// -----------------------------------------------------------------------------------------
// Pin Definitions (Configured for ESP32 / Arduino Compatible Pinout)
// -----------------------------------------------------------------------------------------
#if defined(ESP32)
  // Analog Soil Moisture Sensor Inputs (ADC1 pins recommended when Wi-Fi is active)
  #define PIN_SOIL_SUCCULENT     34   // ADC1_CH6
  #define PIN_SOIL_ORNAMENTAL    35   // ADC1_CH7
  #define PIN_SOIL_LEAFY         32   // ADC1_CH4

  // Actuator Relay Outputs (Active LOW relays standard for Arduino/ESP)
  #define PIN_RELAY_MAIN_PUMP    25   // Master DC Submersible Water Pump
  #define PIN_RELAY_VALVE_1      26   // Solenoid Valve: Plant 1 (Succulent)
  #define PIN_RELAY_VALVE_2      27   // Solenoid Valve: Plant 2 (Ornamental / Rose)
  #define PIN_RELAY_VALVE_3      14   // Solenoid Valve: Plant 3 (Leafy / Tulsi)

  // Status Indicators & Manual Control
  #define PIN_LED_STATUS         2    // Onboard Status LED
  #define PIN_MANUAL_BUTTON      13   // Local Push-Button for Manual Watering Override
  #define PIN_FLOW_SENSOR        4    // Digital Flow Sensor Pulse Input (Hall Effect)
#else
  // Standard Arduino Uno / Nano Pinout
  #define PIN_SOIL_SUCCULENT     A0
  #define PIN_SOIL_ORNAMENTAL    A1
  #define PIN_SOIL_LEAFY         A2

  #define PIN_RELAY_MAIN_PUMP    7
  #define PIN_RELAY_VALVE_1      6
  #define PIN_RELAY_VALVE_2      5
  #define PIN_RELAY_VALVE_3      4

  #define PIN_LED_STATUS         13
  #define PIN_MANUAL_BUTTON      2
  #define PIN_FLOW_SENSOR        3
#endif

// Relay Logic Configuration
#define RELAY_ON   LOW
#define RELAY_OFF  HIGH

// -----------------------------------------------------------------------------------------
// Plant Profile Data Structure & Calibration
// -----------------------------------------------------------------------------------------
struct PlantChannel {
  const char* plantName;
  uint8_t analogPin;
  uint8_t valveRelayPin;
  float moistureThreshold;   // Threshold percentage below which irrigation activates
  float targetMoisture;      // Target percentage to achieve
  unsigned long maxDoseTimeMs;// Safety timeout per watering cycle (ms)
  
  // Calibration points (Raw ADC values: dry air vs fully wet soil)
  int adcDry;
  int adcWet;

  // Runtime State
  float currentMoisture;
  bool isWatering;
  unsigned long wateringStartTime;
};

// Define 3 Plant Channels based on research paper experimental setup:
// 1. Succulent (e.g. Aloe Vera) -> Low water needs (~25%)
// 2. Ornamental (e.g. Rose)     -> Moderate water needs (~45%)
// 3. Leafy Vegetable (e.g. Tulsi)-> High water needs (~65%)
PlantChannel plants[3] = {
  {
    "Succulent (Aloe Vera)",
    PIN_SOIL_SUCCULENT,
    PIN_RELAY_VALVE_1,
    25.0f, // Threshold (%)
    35.0f, // Target (%)
    8000,  // Max dose: 8 seconds safety timeout
    #if defined(ESP32)
    3300, 1300 // ESP32 12-bit ADC calibration (Air, Submerged)
    #else
    850, 350   // Arduino 10-bit ADC calibration
    #endif
  },
  {
    "Ornamental (Rose)",
    PIN_SOIL_ORNAMENTAL,
    PIN_RELAY_VALVE_2,
    45.0f, // Threshold (%)
    55.0f, // Target (%)
    12000, // Max dose: 12 seconds safety timeout
    #if defined(ESP32)
    3300, 1300
    #else
    850, 350
    #endif
  },
  {
    "Leafy Plant (Tulsi)",
    PIN_SOIL_LEAFY,
    PIN_RELAY_VALVE_3,
    65.0f, // Threshold (%)
    75.0f, // Target (%)
    15000, // Max dose: 15 seconds safety timeout
    #if defined(ESP32)
    3300, 1300
    #else
    850, 350
    #endif
  }
};

const int NUM_PLANTS = 3;

// -----------------------------------------------------------------------------------------
// Global Timing & Flow State Variables
// -----------------------------------------------------------------------------------------
unsigned long lastSensorSampleTime = 0;
const unsigned long SENSOR_SAMPLE_INTERVAL = 3000; // Sample every 3 seconds

unsigned long lastTelemetrySyncTime = 0;
const unsigned long TELEMETRY_INTERVAL = 5000;    // Sync to cloud every 5 seconds

// Flow Sensor Pulse Tracking
volatile unsigned long flowPulseCount = 0;
float totalLitersDelivered = 0.0;
const float FLOW_CALIBRATION_FACTOR = 450.0; // Pulses per liter for YF-S201

// Manual Override Flags
bool manualOverrideActive = false;
int manualOverrideChannel = -1;

#if defined(ESP32) || defined(ESP8266)
  BlynkTimer blynkTimer;
#endif

// -----------------------------------------------------------------------------------------
// Interrupt Service Routine for Flow Sensor
// -----------------------------------------------------------------------------------------
#if defined(ESP32)
void IRAM_ATTR flowPulseISR() {
  flowPulseCount++;
}
#else
void flowPulseISR() {
  flowPulseCount++;
}
#endif

// -----------------------------------------------------------------------------------------
// Helper: Convert Raw ADC reading into Calibrated Moisture Percentage
// -----------------------------------------------------------------------------------------
float readSoilMoisturePercent(PlantChannel &channel) {
  const int NUM_SAMPLES = 8;
  long adcSum = 0;

  for (int i = 0; i < NUM_SAMPLES; i++) {
    adcSum += analogRead(channel.analogPin);
    delay(5);
  }
  float avgAdc = (float)adcSum / NUM_SAMPLES;

  // Inverted mapping: Higher ADC = Drier Soil (Resistive / Capacitive probe)
  float moisture = map(avgAdc, channel.adcDry, channel.adcWet, 0, 100);
  moisture = constrain(moisture, 0.0f, 100.0f);
  return moisture;
}

// -----------------------------------------------------------------------------------------
// Actuator Control Helpers
// -----------------------------------------------------------------------------------------
void stopAllWatering() {
  digitalWrite(PIN_RELAY_MAIN_PUMP, RELAY_OFF);
  for (int i = 0; i < NUM_PLANTS; i++) {
    digitalWrite(plants[i].valveRelayPin, RELAY_OFF);
    plants[i].isWatering = false;
  }
  digitalWrite(PIN_LED_STATUS, LOW);
  manualOverrideActive = false;
  manualOverrideChannel = -1;
  Serial.println(F("[PUMP & VALVES] All irrigation lines turned OFF."));
}

void startWatering(int channelIndex) {
  if (channelIndex < 0 || channelIndex >= NUM_PLANTS) return;

  // Open the targeted plant's solenoid valve first
  digitalWrite(plants[channelIndex].valveRelayPin, RELAY_ON);
  delay(100); // Allow valve seat to open

  // Turn on main water pump
  digitalWrite(PIN_RELAY_MAIN_PUMP, RELAY_ON);
  digitalWrite(PIN_LED_STATUS, HIGH);

  plants[channelIndex].isWatering = true;
  plants[channelIndex].wateringStartTime = millis();

  Serial.print(F("[IRRIGATION TRIGGERED] Watering plant: "));
  Serial.print(plants[channelIndex].plantName);
  Serial.print(F(" | Current: "));
  Serial.print(plants[channelIndex].currentMoisture);
  Serial.print(F("% | Target: "));
  Serial.print(plants[channelIndex].targetMoisture);
  Serial.println(F("%"));
}

void checkWateringStatus() {
  bool anyChannelActive = false;

  for (int i = 0; i < NUM_PLANTS; i++) {
    if (plants[i].isWatering) {
      anyChannelActive = true;
      unsigned long elapsed = millis() - plants[i].wateringStartTime;

      // Re-sample current moisture
      plants[i].currentMoisture = readSoilMoisturePercent(plants[i]);

      // Check condition: Target moisture achieved OR Safety max dose timeout reached
      bool targetReached = (plants[i].currentMoisture >= plants[i].targetMoisture);
      bool timeoutReached = (elapsed >= plants[i].maxDoseTimeMs);

      if (targetReached || timeoutReached) {
        Serial.print(F("[IRRIGATION COMPLETE] Finished watering "));
        Serial.print(plants[i].plantName);
        Serial.print(F(" (Elapsed: "));
        Serial.print(elapsed / 1000.0);
        Serial.print(F("s, Moisture: "));
        Serial.print(plants[i].currentMoisture);
        Serial.println(timeoutReached ? F("%, Reason: Timeout Cutoff)") : F("%, Reason: Target Reached)"));

        digitalWrite(plants[i].valveRelayPin, RELAY_OFF);
        plants[i].isWatering = false;
      }
    }
  }

  // If no valves are active, ensure master pump is turned off
  if (!anyChannelActive) {
    digitalWrite(PIN_RELAY_MAIN_PUMP, RELAY_OFF);
    digitalWrite(PIN_LED_STATUS, LOW);
  }
}

// -----------------------------------------------------------------------------------------
// Closed-Loop Control Routine
// -----------------------------------------------------------------------------------------
void evaluateIrrigationNeeds() {
  // If manual override is active or already watering a channel, skip automated trigger
  for (int i = 0; i < NUM_PLANTS; i++) {
    if (plants[i].isWatering) return;
  }

  for (int i = 0; i < NUM_PLANTS; i++) {
    plants[i].currentMoisture = readSoilMoisturePercent(plants[i]);

    // Check against plant-specific threshold
    if (plants[i].currentMoisture < plants[i].moistureThreshold) {
      Serial.print(F("[ALERT] "));
      Serial.print(plants[i].plantName);
      Serial.print(F(" moisture ("));
      Serial.print(plants[i].currentMoisture);
      Serial.print(F("%) fell below threshold ("));
      Serial.print(plants[i].moistureThreshold);
      Serial.println(F("%). Initiating watering cycle..."));

      startWatering(i);
      break; // Water one plant zone at a time to maintain hydraulic pressure
    }
  }
}

// -----------------------------------------------------------------------------------------
// Telemetry & Serial Logging
// -----------------------------------------------------------------------------------------
void logSystemTelemetry() {
  Serial.println(F("\n================ SYSTEM TELEMETRY ================"));
  for (int i = 0; i < NUM_PLANTS; i++) {
    Serial.print(F("  [Zone "));
    Serial.print(i + 1);
    Serial.print(F("] "));
    Serial.print(plants[i].plantName);
    Serial.print(F(": "));
    Serial.print(plants[i].currentMoisture, 1);
    Serial.print(F("% (Set: "));
    Serial.print(plants[i].moistureThreshold, 0);
    Serial.print(F("%) | Valve: "));
    Serial.println(plants[i].isWatering ? F("OPEN") : F("CLOSED"));
  }
  Serial.print(F("  Master Pump: "));
  Serial.println((digitalRead(PIN_RELAY_MAIN_PUMP) == RELAY_ON) ? F("RUNNING") : F("IDLE"));
  Serial.print(F("  Total Water Delivered: "));
  Serial.print(totalLitersDelivered, 2);
  Serial.println(F(" L"));
  Serial.println(F("=================================================="));

#if defined(ESP32) || defined(ESP8266)
  if (WiFi.status() == WL_CONNECTED) {
    // Sync telemetry to Blynk Virtual Pins
    // V0: Succulent Moisture | V1: Ornamental Moisture | V2: Leafy Moisture
    // V3: Master Pump Status | V4: Total Liters | V5: Manual Trigger
    Blynk.virtualWrite(V0, plants[0].currentMoisture);
    Blynk.virtualWrite(V1, plants[1].currentMoisture);
    Blynk.virtualWrite(V2, plants[2].currentMoisture);
    Blynk.virtualWrite(V3, (digitalRead(PIN_RELAY_MAIN_PUMP) == RELAY_ON) ? 1 : 0);
    Blynk.virtualWrite(V4, totalLitersDelivered);
  }
#endif
}

// -----------------------------------------------------------------------------------------
// Blynk Virtual Pin Handlers (Cloud Remote Control)
// -----------------------------------------------------------------------------------------
#if defined(ESP32) || defined(ESP8266)
BLYNK_WRITE(V5) { // Virtual pin 5: Manual override toggle from Mobile App
  int buttonValue = param.asInt();
  if (buttonValue == 1) {
    Serial.println(F("[BLYNK] Cloud manual override activated for Zone 1"));
    startWatering(0);
  } else {
    Serial.println(F("[BLYNK] Cloud manual override turned off"));
    stopAllWatering();
  }
}
#endif

// -----------------------------------------------------------------------------------------
// Setup Function
// -----------------------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println(F("=================================================="));
  Serial.println(F(" IoT-Based Plant-Specific Smart Irrigation System "));
  Serial.println(F(" Authors: Ayush Kumar Jha, Saif Ahmed, Dev Dharra, "));
  Serial.println(F("          Ankur Dhankher (MRIIRS)                  "));
  Serial.println(F("=================================================="));

  // Initialize Relays (Default OFF)
  pinMode(PIN_RELAY_MAIN_PUMP, OUTPUT);
  digitalWrite(PIN_RELAY_MAIN_PUMP, RELAY_OFF);

  for (int i = 0; i < NUM_PLANTS; i++) {
    pinMode(plants[i].valveRelayPin, OUTPUT);
    digitalWrite(plants[i].valveRelayPin, RELAY_OFF);
    pinMode(plants[i].analogPin, INPUT);
    plants[i].isWatering = false;
  }

  pinMode(PIN_LED_STATUS, OUTPUT);
  digitalWrite(PIN_LED_STATUS, LOW);

  pinMode(PIN_MANUAL_BUTTON, INPUT_PULLUP);
  pinMode(PIN_FLOW_SENSOR, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_FLOW_SENSOR), flowPulseISR, RISING);

  // Initialize Wi-Fi and IoT Platform if running on ESP
#if defined(ESP32) || defined(ESP8266)
  Serial.print(F("Connecting to Wi-Fi SSID: "));
  Serial.println(ssid);
  WiFi.begin(ssid, password);

  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 10000) {
    delay(500);
    Serial.print(F("."));
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(F("\n[Wi-Fi] Connected successfully!"));
    Serial.print(F("[Wi-Fi] IP Address: "));
    Serial.println(WiFi.localIP());
    Blynk.config(BLYNK_AUTH_TOKEN);
    Blynk.connect();
  } else {
    Serial.println(F("\n[Wi-Fi] Offline mode - Running standalone closed-loop control."));
  }
#else
  Serial.println(F("[SYSTEM] Running on Arduino Microcontroller mode."));
#endif

  Serial.println(F("[SYSTEM] Initialization Complete. Starting Closed-Loop Monitoring..."));
}

// -----------------------------------------------------------------------------------------
// Main Infinite Loop
// -----------------------------------------------------------------------------------------
void loop() {
  unsigned long currentMillis = millis();

#if defined(ESP32) || defined(ESP8266)
  if (Blynk.connected()) {
    Blynk.run();
  }
#endif

  // Check Local Manual Push Button (Active LOW)
  if (digitalRead(PIN_MANUAL_BUTTON) == LOW) {
    delay(50); // Debounce
    if (digitalRead(PIN_MANUAL_BUTTON) == LOW) {
      Serial.println(F("[BUTTON] Local manual override pressed. Toggling Zone 2..."));
      if (!plants[1].isWatering) {
        startWatering(1);
      } else {
        stopAllWatering();
      }
      while (digitalRead(PIN_MANUAL_BUTTON) == LOW); // Wait for release
    }
  }

  // Active watering supervision (Target check and timeout cutoff)
  checkWateringStatus();

  // Periodic sensor sampling & closed-loop decision
  if (currentMillis - lastSensorSampleTime >= SENSOR_SAMPLE_INTERVAL) {
    lastSensorSampleTime = currentMillis;
    evaluateIrrigationNeeds();
  }

  // Periodic Telemetry Publishing
  if (currentMillis - lastTelemetrySyncTime >= TELEMETRY_INTERVAL) {
    lastTelemetrySyncTime = currentMillis;

    // Calculate volume from flow sensor pulses
    if (flowPulseCount > 0) {
      float currentLiters = (float)flowPulseCount / FLOW_CALIBRATION_FACTOR;
      totalLitersDelivered += currentLiters;
      flowPulseCount = 0;
    }

    logSystemTelemetry();
  }
}
