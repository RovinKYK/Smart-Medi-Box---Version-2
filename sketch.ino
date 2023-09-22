#include <PubSubClient.h>
#include <WiFi.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include "DHTesp.h"
#include <ESP32Servo.h>

//Define pins
#define dht_pin 15
#define buzzer_pin 18
#define ldr_pin 33
#define servo_pin 13

//Create instances of classes
WiFiClient espClient;
PubSubClient mqttClient(espClient);
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP);
DHTesp dhtSensor;
Servo servoMotor;

//LDR sensor parameters
const float GAMMA = 0.7;
const float RL10 = 50;

//Min, Max values observed for lux value calculation using ldr input
double min_lux = 0.12;
double long max_lux = 100916.59;

//Parameters controlling servo motor angle
float min_angle = 30;
float control_factor = 0.75;

//Variables to store important values
char temp_arr[6];
bool is_scheduled_on = false;
unsigned long scheduled_on_time;
double light_intensity;
char light_intensity_arr[5];
float servo_angle;

void setup() {
  Serial.begin(115200);
  setupWifi();
  setupMqtt();

  timeClient.begin();
  timeClient.setTimeOffset(5.5 * 3600); 

  dhtSensor.setup(dht_pin, DHTesp::DHT22);
  servoMotor.attach(servo_pin);
  pinMode(buzzer_pin, OUTPUT);
  digitalWrite(buzzer_pin, LOW);      
  pinMode(ldr_pin, INPUT);

  //Connect to broker to set the initial values of min_angle and control_factor in the dashboard
  connectToBroker();
  mqttClient.publish("Medibox-Servo_min_angle_esp", "30");
  mqttClient.publish("Medibox-Servo_control_factor_esp", "0.75");
}

void loop() {
  if (!mqttClient.connected()) {
    connectToBroker();
  }
  Serial.println();
  mqttClient.loop();

  //Update sensor readings and adjust the servo motor
  updateTemperature();
  updateLightIntensity();
  adjustSlidingWindow();

  //Publish sensor values in the dashboard
  mqttClient.publish("Medibox-Temperature", temp_arr);
  mqttClient.publish("Medibox-LDR_Val", light_intensity_arr);

  //Check whether buzzer scheduled time reached
  checkSchedule();
  delay(500);
}

void setupWifi() {
  WiFi.begin("Wokwi-GUEST", "");
  Serial.print("Connecting to Wifi");

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("Connected");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
  Serial.println();
}

void setupMqtt() {
  mqttClient.setServer("test.mosquitto.org", 1883);
  mqttClient.setCallback(receiveCallback);
}

void connectToBroker() {
  while (!mqttClient.connected()) {
    Serial.print("Attempting MQTT connection...");
    if (mqttClient.connect("ESP32-12345")) {
      Serial.println("Connected");

      //Subscribe to the topics to listen from dashboard
      mqttClient.subscribe("Medibox-Buzzer_On-Off"); 
      mqttClient.subscribe("Medibox-Buzzer_schedule");
      mqttClient.subscribe("Medibox-Servo_min_angle"); 
      mqttClient.subscribe("Medibox-Servo_control_factor"); 

    } else {
      Serial.print("Failed ");
      Serial.println(mqttClient.state());
      delay(5000);
    }
  }
}

void receiveCallback(char* topic, byte* payload, unsigned int length){
  Serial.print("Message arrived [");
  Serial.print(topic);  
  Serial.print("] : ");

  char payload_char_arr[length+1];

  for(int i = 0; i < length; i++) {
    Serial.print((char) payload[i]);
    payload_char_arr[i] = (char) payload[i];  
  }

  /*Null charactor added to represent the end of character array. Else created errors for some 
  inputs by reading garbage data in consecutive memory locations*/
  payload_char_arr[length] = '\0'; 
  Serial.println();
  
  if(strcmp(topic, "Medibox-Buzzer_On-Off") == 0) {
    buzzerOn(payload_char_arr[0] == '1');
  }
  else if(strcmp(topic, "Medibox-Buzzer_schedule") == 0) {
    if(payload_char_arr[0] == 'N'){
      is_scheduled_on = false;
    }
    else{
      is_scheduled_on = true;
      scheduled_on_time = atol(payload_char_arr);   
    }
  }  
  else if(strcmp(topic, "Medibox-Servo_min_angle") == 0) {
    min_angle = atof(payload_char_arr);
  }
  else if(strcmp(topic, "Medibox-Servo_control_factor") == 0) {
    control_factor = atof(payload_char_arr);
  }
}

void updateTemperature(){
  TempAndHumidity data = dhtSensor.getTempAndHumidity();
  String(data.temperature, 2).toCharArray(temp_arr, 6); 
  Serial.print("Temperature : "); 
  Serial.println(temp_arr);
}

void buzzerOn(bool on){
  if(on){
    tone(buzzer_pin,256);
  }
  else{
    noTone(buzzer_pin);
  }
}

unsigned long getTime(){
  timeClient.update();
  return timeClient.getEpochTime();
}

void checkSchedule(){
  if(is_scheduled_on){
    unsigned long currentTime = getTime();
    if(currentTime > scheduled_on_time) {
      buzzerOn(true);
      is_scheduled_on = false;

      mqttClient.publish("Medibox-Buzzer_On-Off_esp", "1");
      mqttClient.publish("Medibox-Buzzer_schedule_esp", "0");

      Serial.println("Scheduled ON");
    }
  }
}

void updateLightIntensity() {
  int ldr_val = analogRead(ldr_pin);

  /*Following convertion of ldr sensor input to lux value is for a arduino board which 
  gets analog input in 0-1024 range. But esp32 gets analog input in 0-4096 range. Hence
  before converting to lux value we should scale down input to 0-1024 range*/
  float scaled_val = ldr_val / 4;

  //Convert the ldr input value into lux value
  float voltage = scaled_val / 1024. * 5;
  float resistance = 2000 * voltage / (1 - voltage / 5);
  float lux = pow(RL10 * 1e3 * pow(10, GAMMA) / resistance, (1 / GAMMA));
  
  //Scale down light intensity to 0-1 range using linear interpolation
  light_intensity = abs((lux - min_lux)) * (1 - 0) / (max_lux - min_lux) + 0;

  String(light_intensity, 2).toCharArray(light_intensity_arr, 5);
  Serial.print("Light intensity : ");
  Serial.println(light_intensity_arr);
}

void adjustSlidingWindow() {
  servo_angle = min_angle + (180 - min_angle) * light_intensity * control_factor;
  servoMotor.write(servo_angle);
  Serial.print("Servo motor angle : ");
  Serial.println(servo_angle);
}