#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <freertos/FreeRTOS.h>
#include <string.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_ADXL345_U.h>

//inference
#include <ADXL_classification_inferencing.h>

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

// Connection initialization
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);
Adafruit_ADXL345_Unified accelerometer = Adafruit_ADXL345_Unified(12345);

// Communication variables
QueueHandle_t mqttQueue;
QueueHandle_t sensorQueue;

//sensor task stores INPUT_FRAME_SIZE in CLASSIFIER_INTERVAL time
//once it has the INPUT_FRAME_SIZE it sendes the value to the buffer
// when the inference task receives the buffer it receives the data and inferes de result
// when it finishes it sends the data to the mqtt task using a queue
// the mqtt data sends the data throguh mqtt 

// Assignment function definition
void sensorTask(void *parameter); //sensor reading task for inference configuration
void inferenceTask(void *parameter); //inference task
void mqttTask(void *parameter); // data publication through mqtt

// Dataset generation function definition
void dataStreamTask(void *parameter); 

// Robustness methods
void connectWifi();
void connectMQTT();
bool configureAccelerometer();

// Support data structures and classes
enum class Orientation
{
  UP,
  DOWN,
  LEFT,
  RIGHT,
  IDLE
};

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
  case Orientation::IDLE:
    return "IDLE";
  default:
    return "UNKNOWN";
  }
}

struct InferenceResult {
  const char *label;
  float value;
};

struct AccelSample {
  float x;
  float y;
  float z;
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

  mqttQueue = xQueueCreate(10, sizeof(InferenceResult));
  if (mqttQueue == nullptr) {
    Serial.println("There was an error creating the mqtt queue");
    return;
  }

  sensorQueue = xQueueCreate(100, sizeof(AccelSample));
  if (sensorQueue == nullptr) {
    Serial.println("There was an error creating the sensor queue");
    return;
  }

  // Task initialization
  #if defined(DATA_STREAM)
  xTaskCreate(dataStreamTask, "SensorTask", 4096, nullptr, 1, nullptr);
  #else
  // Initialize external network connections
  connectWifi();
  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
  connectMQTT();

  BaseType_t ok;
  ok = xTaskCreate(sensorTask, "SensorTask", 4096, nullptr, 3, nullptr);
  configASSERT(ok == pdPASS);

  ok = xTaskCreate(inferenceTask, "InferenceTask", 8192, nullptr, 2, nullptr);
  configASSERT(ok == pdPASS);

  ok = xTaskCreate(mqttTask, "MqttTask", 8192, nullptr, 1, nullptr);
  configASSERT(ok == pdPASS);

  #endif
}

void loop()
{
  // put your main code here, to run repeatedly:
  vTaskDelay(pdMS_TO_TICKS(1000));
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
      vTaskDelayUntil(&lastWakeTime,  pdMS_TO_TICKS(EI_CLASSIFIER_INTERVAL_MS));
      continue;
    }

    AccelSample sample = {
      event.acceleration.x,
      event.acceleration.y,
      event.acceleration.z
    };

    if (xQueueSend(sensorQueue, &sample, 0) != pdPASS) {
      //TODO: handle queue send error
    }

    // Wait untill next sample
    vTaskDelayUntil(&lastWakeTime,  pdMS_TO_TICKS(EI_CLASSIFIER_INTERVAL_MS));
  }
}

void inferenceTask(void *parameter) {

  static float buffer[EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE];
  size_t used = 0;
  AccelSample sample;
  size_t repeatedResults = 0;
  const char *previousResult = nullptr;

  for(;;) {

    if (xQueueReceive(sensorQueue, &sample, portMAX_DELAY) != pdPASS) {
      //TODO: handle queue error
      continue;
    }
    
    buffer[used++] = sample.x;
    buffer[used++] = sample.y;
    buffer[used++] = sample.z;

    if (used != EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE)  {
      continue;
    }

    used = 0;
    signal_t signal;
    int err = numpy::signal_from_buffer(buffer, EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE, &signal);
    if (err != 0) {
      // TODO: handle error with inference model
      continue;
    }

    ei_impulse_result_t result = {};
    EI_IMPULSE_ERROR status = run_classifier(&signal, &result, false);

    if (status == EI_IMPULSE_OK) {
      // TODO: handle error with inference model
      size_t bestIdx = 0;
      for(size_t j = 0; j < EI_CLASSIFIER_LABEL_COUNT; j++) {
        if (result.classification[j].value > 
        result.classification[bestIdx].value) {
          bestIdx = j;
        }
      }

      InferenceResult infResult = {
        result.classification[bestIdx].label,
        result.classification[bestIdx].value
      };

      if (previousResult != nullptr && strcmp(previousResult, infResult.label) == 0)  {
        repeatedResults++;
      } else {
        repeatedResults = 1;
        previousResult = infResult.label;
      }

      if (repeatedResults >= 3) {
        Serial.printf("Inference result %s: %.3f\n",
          infResult.label,
          infResult.value
        );

        if (xQueueSend(mqttQueue, &infResult, 0) != pdPASS) {
          // TODO: handle q send error;
        }
        
      }

    }
    
  }
}
/**
 * Task whose purpouse is to publish mqtt information and keep the connection alive
 */
void mqttTask(void *parameter)
{
  InferenceResult current{};

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
    if (xQueueReceive(mqttQueue, &current, pdMS_TO_TICKS(SAMPLE_PERIOD)) == pdTRUE)
    {
  
      char payload[100];
      snprintf(
        payload,
        sizeof(payload),
        "{\"movement_type\":\"%s\", \"confidence\": %.3f}",
        current.label, current.value
      );

      if (mqttClient.publish("ESP32/ADXL345/", payload))
      {
        Serial.println("MQTT message sent!");
      }

    }

    vTaskDelay(pdMS_TO_TICKS(TASK_DELAY));
  }
}


// DATASET generation task
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
    
    Serial.printf("%.2f,%.2f,%.2f\n", event.acceleration.x, event.acceleration.y, event.acceleration.z);

    vTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(INTERVAL_MS));
  }
}

// HELPER FUNCTIONS

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
