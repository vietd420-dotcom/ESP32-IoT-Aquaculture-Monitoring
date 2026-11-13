
#include <WiFi.h>
#include <PubSubClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>

// ================= WIFI =================
const char* WIFI_SSID = "Wokwi-GUEST";
const char* WIFI_PASSWORD = "";

// ================= MQTT =================
const char* MQTT_SERVER = "test.mosquitto.org";
const int MQTT_PORT = 1883;

const char* TOPIC_TELEMETRY = "aquaculture/vietd420/telemetry";
const char* TOPIC_CONTROL   = "aquaculture/vietd420/control";

// ================= GPIO =================
#define TRIG_PIN   5
#define ECHO_PIN   18
#define TEMP_PIN   4
#define PUMP_PIN   26
#define VALVE_PIN  27

// ================= TANK =================
#define TANK_HEIGHT_CM 200.0

// ================= OBJECTS =================
WiFiClient espClient;
PubSubClient mqtt(espClient);

OneWire oneWire(TEMP_PIN);
DallasTemperature temperatureSensor(&oneWire);

// ================= DATA STRUCTURE =================
struct SensorData {
  float distance;
  float waterLevel;
  float temperature;
};

QueueHandle_t dataQueue;
QueueHandle_t alertQueue;

// ================= CONTROL STATE =================
volatile bool manualMode = false;
volatile bool manualPump = false;
volatile bool manualValve = false;

int currentZone = -1;

// ================= READ ULTRASONIC =================
float readDistance() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);

  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  long duration = pulseIn(ECHO_PIN, HIGH, 30000);

  if (duration == 0) return -1;

  return duration * 0.0343 / 2.0;
}

// ================= MQTT CALLBACK =================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String command = "";

  for (unsigned int i = 0; i < length; i++) {
    command += (char)payload[i];
  }

  command.trim();

  Serial.print("MQTT command: ");
  Serial.println(command);

  if (command == "MANUAL") {
    manualMode = true;
  }
  else if (command == "AUTO") {
    manualMode = false;
    manualPump = false;
    manualValve = false;
    analogWrite(PUMP_PIN, 0);
    digitalWrite(VALVE_PIN, LOW);
  }
  else if (command == "PUMP_ON") {
    manualMode = true;
    manualPump = true;
    analogWrite(PUMP_PIN, 255);
  }
  else if (command == "PUMP_OFF") {
    manualPump = false;
    analogWrite(PUMP_PIN, 0);
  }
  else if (command == "VALVE_ON") {
    manualMode = true;
    manualValve = true;
    digitalWrite(VALVE_PIN, HIGH);
  }
  else if (command == "VALVE_OFF") {
    manualValve = false;
    digitalWrite(VALVE_PIN, LOW);
  }
}

// ================= MQTT CONNECTION =================
void connectMQTT() {
  while (!mqtt.connected()) {
    String clientId = "ESP32-Aqua-";
    clientId += String((uint32_t)ESP.getEfuseMac(), HEX);

    Serial.println("Connecting MQTT...");

    if (mqtt.connect(clientId.c_str())) {
      Serial.println("MQTT connected!");
      mqtt.subscribe(TOPIC_CONTROL);
    } else {
      Serial.print("MQTT error: ");
      Serial.println(mqtt.state());
      vTaskDelay(pdMS_TO_TICKS(3000));
    }
  }
}

// =====================================================
// TASK 1: READ SENSOR - CORE 1
// =====================================================
void TaskDocDuLieu(void *parameter) {

  while (true) {

    SensorData data;

    float distance = readDistance();

    temperatureSensor.requestTemperatures();
    vTaskDelay(pdMS_TO_TICKS(750));

    float temp = temperatureSensor.getTempCByIndex(0);

    if (distance > 0 && temp != DEVICE_DISCONNECTED_C) {

      data.distance = distance;
      data.waterLevel = TANK_HEIGHT_CM - distance;
      data.waterLevel = constrain(data.waterLevel, 0, TANK_HEIGHT_CM);
      data.temperature = temp;

      Serial.println("\n===== SENSOR DATA =====");
      Serial.print("Distance: ");
      Serial.print(data.distance);
      Serial.println(" cm");

      Serial.print("Water level: ");
      Serial.print(data.waterLevel);
      Serial.println(" cm");

      Serial.print("Temperature: ");
      Serial.print(data.temperature);
      Serial.println(" C");

      // Send complete data to dataQueue
      xQueueSend(dataQueue, &data, 0);

      // Send water-level data to dataAlert
      xQueueOverwrite(alertQueue, &data);
    }
    else {
      Serial.println("Sensor reading error!");
    }

    vTaskDelay(pdMS_TO_TICKS(250));
  }
}

// =====================================================
// TASK 2: AUTOMATIC WARNING AND CONTROL - CORE 1
// =====================================================
void TaskCanhBao(void *parameter) {

  SensorData data;

  while (true) {

    if (xQueueReceive(alertQueue, &data, portMAX_DELAY) == pdTRUE) {

      if (manualMode) {
        continue;
      }

      int zone = 0;

      if (data.waterLevel >= 100 && data.waterLevel < 150) {
        zone = 1;
      }
      else if (data.waterLevel >= 150 && data.waterLevel <= 200) {
        zone = 2;
      }
      else if (data.waterLevel < 50) {
        zone = 3;
      }

      // Only activate when entering a new zone
      if (zone != currentZone) {

        currentZone = zone;

        if (zone == 1) {
          Serial.println("ALERT: PUMP 50% FOR 10 SECONDS");

          digitalWrite(VALVE_PIN, LOW);
          analogWrite(PUMP_PIN, 128);

          vTaskDelay(pdMS_TO_TICKS(10000));

          analogWrite(PUMP_PIN, 0);
        }

        else if (zone == 2) {
          Serial.println("ALERT: PUMP 100% FOR 20 SECONDS");

          digitalWrite(VALVE_PIN, LOW);
          analogWrite(PUMP_PIN, 255);

          vTaskDelay(pdMS_TO_TICKS(20000));

          analogWrite(PUMP_PIN, 0);
        }

        else if (zone == 3) {
          Serial.println("ALERT: OPEN DRAIN VALVE FOR 10 SECONDS");

          analogWrite(PUMP_PIN, 0);
          digitalWrite(VALVE_PIN, HIGH);

          vTaskDelay(pdMS_TO_TICKS(10000));

          digitalWrite(VALVE_PIN, LOW);
        }

        else {
          analogWrite(PUMP_PIN, 0);
          digitalWrite(VALVE_PIN, LOW);
          Serial.println("Water level in normal range.");
        }
      }
    }
  }
}

// =====================================================
// TASK 3: WIFI + MQTT DATA TRANSMISSION - CORE 0
// =====================================================
void TaskTruyenDuLieu(void *parameter) {

  SensorData data;

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("Connecting WiFi");

  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    vTaskDelay(pdMS_TO_TICKS(500));
  }

  Serial.println("\nWiFi connected!");
  Serial.println(WiFi.localIP());

  mqtt.setServer(MQTT_SERVER, MQTT_PORT);
  mqtt.setCallback(mqttCallback);

  while (true) {

    if (WiFi.status() != WL_CONNECTED) {
      WiFi.reconnect();
    }

    if (!mqtt.connected()) {
      connectMQTT();
    }

    mqtt.loop();

    if (xQueueReceive(dataQueue, &data, pdMS_TO_TICKS(200)) == pdTRUE) {

      char payload[200];

      snprintf(payload, sizeof(payload),
        "{\"distance\":%.2f,\"waterLevel\":%.2f,\"temperature\":%.2f}",
        data.distance,
        data.waterLevel,
        data.temperature
      );

      if (mqtt.publish(TOPIC_TELEMETRY, payload)) {
        Serial.println("Telemetry sent to MQTT:");
        Serial.println(payload);
      }
    }

    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// ================= SETUP =================
void setup() {

  Serial.begin(115200);

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(PUMP_PIN, OUTPUT);
  pinMode(VALVE_PIN, OUTPUT);

  digitalWrite(TRIG_PIN, LOW);
  digitalWrite(PUMP_PIN, LOW);
  digitalWrite(VALVE_PIN, LOW);

  temperatureSensor.begin();

  // Create FreeRTOS queues
  dataQueue = xQueueCreate(10, sizeof(SensorData));
  alertQueue = xQueueCreate(1, sizeof(SensorData));

  if (dataQueue == NULL || alertQueue == NULL) {
    Serial.println("Queue creation failed!");
    while (true) {
      delay(1000);
    }
  }

  // Create FreeRTOS tasks
  xTaskCreatePinnedToCore(
    TaskDocDuLieu,
    "Task Doc Du Lieu",
    4096,
    NULL,
    2,
    NULL,
    1
  );

  xTaskCreatePinnedToCore(
    TaskCanhBao,
    "Task Canh Bao",
    4096,
    NULL,
    1,
    NULL,
    1
  );

  xTaskCreatePinnedToCore(
    TaskTruyenDuLieu,
    "Task Truyen Du Lieu",
    6144,
    NULL,
    2,
    NULL,
    0
  );

  Serial.println("Aquaculture Monitoring System Started!");
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}
