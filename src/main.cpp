#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <freertos/FreeRTOS.h>

#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_ADXL345_U.h>

constexpr const int TASK_DELAY = 1000;
constexpr const int RETRY_DELAY = 2000;
constexpr const int WIFI_RETRY_DELAY = 500;
constexpr const int SAMPLE_PERIOD = 500;

constexpr const u_int8_t FREQUENCY_HZ = 50;
constexpr const u_int8_t INTERVAL_MS = (1000 / (FREQUENCY_HZ + 1));

#if defined(ENV_WOKWI)
// WiFi constants
constexpr const char *WIFI_SSID = "Wokwi-GUEST";
constexpr const char *WIFI_PASSWORD = "";
constexpr const int WIFI_CHANNEL = 6;

// MQTT constants
constexpr const char *MQTT_SERVER = "host.wokwi.internal";
constexpr const int MQTT_PORT = 1883;
constexpr const char *MQTT_USER = "";
constexpr const char *MQTT_PASSWORD = "";
#elif defined(ENV_REAL)
#include "secrets.h"
// Read README.md on how to update the real connection information
constexpr const int WIFI_CHANNEL = 0;

constexpr const char *MQTT_SERVER = "192.168.0.30";
constexpr const int MQTT_PORT = 1883;
#endif

WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

Adafruit_ADXL345_Unified accelerometer = Adafruit_ADXL345_Unified(12345);

QueueHandle_t queue;

// Function definition
void mqttTask(void *parameter);
void sensorTask(void *parameter);
void dataStreamTask(void *parameter);

void connectWifi();
void connectMQTT();
bool configureAccelerometer();

// Support data structures and classes
enum class Orientation
{
  UP,
  DOWN,
  LEFT,
  RIGHT
};

Orientation classifyOrientation(float x, float y)
{

  if (fabsf(x) > fabsf(y))
  {
    return x < 0 ? Orientation::LEFT : Orientation::RIGHT;
  }

  return y > 0 ? Orientation::UP : Orientation::DOWN;
}

const char *orientationName(Orientation orientation)
{
  switch (orientation)
  {
  case Orientation::UP:
    return "UP";
  case Orientation::DOWN:
    return "DOWN";
  case Orientation::LEFT:
    return "LEFT";
  case Orientation::RIGHT:
    return "RIGHT";
  default:
    return "UNKNOWN";
  }
}

struct SensorData
{
  float x;
  float y;
  float z;
  Orientation orientation;
};

void setup()
{
  // put your setup code here, to run once:
  Serial.begin(115200);

  // Initialize wire connecitons
  // I²C del ESP32: SDA = GPIO 21, SCL = GPIO 22
  Wire.begin(21, 22);

  while (!configureAccelerometer())
  {
    delay(1000);
  }

  queue = xQueueCreate(10, sizeof(SensorData));
  if (queue == nullptr)
  {
    Serial.println("There was an error creating the queue");
    return;
  }

  // Initialize external network connections
  connectWifi();
  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
  connectMQTT();

  // Task initialization
  #if defined(DATA_STREAM)
  xTaskCreate(dataStreamTask, "SensorTask", 4096, nullptr, 1, nullptr);
  #else
  xTaskCreate(sensorTask, "SensorTask", 4096, nullptr, 1, nullptr);
  #endif
  // MQTT/NETWORK task pinnned to core 0 alongside WI-FI stack (Priority 1)
  xTaskCreatePinnedToCore(mqttTask, "MqttTask", 8192, NULL, 1, NULL, 0);
}

void loop()
{
  // put your main code here, to run repeatedly:
  vTaskDelay(pdMS_TO_TICKS(1000));
}

/**
 * Task whose purpouse is to publish mqtt information and keep the connection alive
 */
void mqttTask(void *parameter)
{

  SensorData prev{};
  bool hasPrev = false;
  SensorData current{};

  for (;;)
  {
    if (WiFi.status() != WL_CONNECTED)
    {
      connectWifi();
    }

    if (!mqttClient.connected())
    {
      connectMQTT();
    }

    // Mantaining MQTT connection open
    mqttClient.loop();

    // if sensor data has been received
    if (xQueueReceive(queue, &current, pdMS_TO_TICKS(SAMPLE_PERIOD)) == pdTRUE)
    {
      if (hasPrev && prev.orientation != current.orientation)
      {

        char payload[100];
        snprintf(
            payload,
            sizeof(payload),
            "{\"orientation\":\"%s\", \"x\": %.2f, \"y\": %.2f, \"z\": %.2f}",
            orientationName(current.orientation), current.x, current.y, current.z);

        if (mqttClient.publish("ESP32/ADXL345/", payload))
        {
          Serial.println("MQTT message sent!");
        }
      }
      prev = current;
      hasPrev = true;
    }

    vTaskDelay(pdMS_TO_TICKS(TASK_DELAY));
  }
}

/**
 * Blocking method to reconnect to wifi 
 */
void connectWifi()
{
  Serial.print("Connecting to Wi-Fi");
  if (WIFI_CHANNEL == 6)
  {
    // configuration for wokwi simulation
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD, WIFI_CHANNEL);
  }
  else
  {
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }

  while (WiFi.status() != WL_CONNECTED)
  {
    vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY));
    Serial.print(".");
  }

  Serial.print("\nWi-Fi connected - IP: ");
  Serial.println(WiFi.localIP());
}

/**
 * Blocking method to connect to the mqtt broker, it solves wifi connection as well
 */
void connectMQTT()
{

  while (!mqttClient.connected())
  {
    Serial.println("Connecting to Broker MQTT");

    char *clienntId = "ESP32";

    bool connected;
    if (MQTT_USER[0] != '\0')
    {
      connected = mqttClient.connect(clienntId, MQTT_USER, MQTT_PASSWORD);
    }
    else
    {
      connected = mqttClient.connect(clienntId);
    }

    if (connected)
    {
      Serial.println("ESP-032 Connected to MQTT Broker!");
    }
    else
    {
      Serial.print("Connection failed: ");
      Serial.println(mqttClient.state());
      if(WiFi.status() != WL_CONNECTED)
      {
        connectWifi();
      }
      vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY));
    }
  }
}

/**
 * Task that reads the AXL345 information
 */
void sensorTask(void *parameter)
{

  TickType_t lastWakeTime = xTaskGetTickCount();

  for (;;)
  {
    // Reading event verification callback;
    sensors_event_t event;
    if (!accelerometer.getEvent(&event)) {
      Serial.println("Failed to read ADXL345");
      configureAccelerometer();
      vTaskDelayUntil(&lastWakeTime,  pdMS_TO_TICKS(SAMPLE_PERIOD));
      continue;
    }

    Orientation orientation = classifyOrientation(
        event.acceleration.x,
        event.acceleration.y);

    SensorData data = {
        event.acceleration.x,
        event.acceleration.y,
        event.acceleration.z,
        orientation};

    xQueueSend(queue, &data, pdMS_TO_TICKS(SAMPLE_PERIOD));

    // Wait untill next sample
    vTaskDelayUntil(&lastWakeTime,  pdMS_TO_TICKS(SAMPLE_PERIOD));
  }
}

/**
 * Setting up connection to the accelerometer
 */
bool configureAccelerometer() {
  if (!accelerometer.begin()){
    Serial.println("ADXL345 not found");
    return false;
  }

  accelerometer.setRange(ADXL345_RANGE_2_G);
  Serial.println("ADXL345 ready");
  return true;
}

void dataStreamTask(void *parameter) {
  TickType_t lastWakeTime = xTaskGetTickCount();

  for (;;)
  {
    // Reading event verification callback;
    sensors_event_t event;
    if (!accelerometer.getEvent(&event)) {
      Serial.println("Failed to read ADXL345");
      configureAccelerometer();
      continue;
    }
    
    Serial.printf("%.2f,%.2f,%.2f\n", event.acceleration.x, event.acceleration.y, event.acceleration.y);

    vTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(INTERVAL_MS));
  }
}