/*
 * Project: nRF905 Radio Library for Arduino (Ping server example)
 * Author: Zak Kemblae, contact@zakkemble.net
 * Copyright: (C) 2020 by Zak Kemble
 * License: GNU GPL v3 (see License.txt)
 * Web: https://blog.zakkemble.net/nrf905-avrarduino-librarydriver/
 */


/*
 * Listen for packets and send them back
 */

#include "arduino_secrets.h"
#include <nRF905.h>
#include <SPI.h>
#include <WiFiNINA.h>  // Changed from <WiFi.h>
#include <ArduinoBearSSL.h>
#include <ArduinoECCX08.h>
#include <ArduinoMqttClient.h>
#include <ArduinoHttpClient.h>
#include <Arduino.h>

#define RXADDR 0xE7E7E7E7  // Address of this device
#define TXADDR 0xE7E7E7E7  // Address of device to send to

#define PACKET_NONE 0
#define PACKET_OK 1
#define PACKET_INVALID 2

#define LED A5
#define PAYLOAD_SIZE NRF905_MAX_PAYLOAD

#define RADIO_SS_PIN 10

struct NumberNode {
  long number;
  NumberNode* next = nullptr;
};

NumberNode* head = nullptr;

long lastCard;
long newPacketID;

nRF905 transceiver = nRF905();

long nodeOriginID = 2;

const char ssid[] = SECRET_SSID;
const char pass[] = SECRET_PASS;
const char broker[] = SECRET_BROKER;
const char* certificate = SECRET_CERTIFICATE;

const char serverAddress[] = "whxaeal39j.execute-api.us-east-2.amazonaws.com";
int port = 443;  // Use 443 for HTTPS

WiFiClient wifiClient;                // Used for the TCP socket connection
BearSSLClient sslClient(wifiClient);  // Used for SSL/TLS connection, integrates with ECC508
MqttClient mqttClient(sslClient);

unsigned long lastMillis = 0;

static volatile uint8_t packetStatus;

// Don't modify these 2 functions. They just pass the DR/AM interrupt to the correct nRF905 instance.
void nRF905_int_dr() {
  transceiver.interrupt_dr();
}
void nRF905_int_am() {
  transceiver.interrupt_am();
}


// Event function for RX complete
void nRF905_onRxComplete(nRF905* device) {
  packetStatus = PACKET_OK;
  transceiver.standby();
}


// Event function for RX invalid
void nRF905_onRxInvalid(nRF905* device) {
  packetStatus = PACKET_INVALID;
  transceiver.standby();
}


void setup() {
  Serial.begin(115200);

  if (!ECCX08.begin()) {
    Serial.println("No ECCX08 present!");
    while (1)
      ;
  }

  Serial.println(F("Server starting..."));


  // This must be called first
  SPI.begin();

  long seed = analogRead(A0) + analogRead(A1) * 482;

  Serial.println(seed);

  randomSeed(seed);


  transceiver.begin(
    SPI,                // SPI bus to use (SPI, SPI1, SPI2 etc)
    10000000,           // SPI Clock speed (10MHz)
    RADIO_SS_PIN,       // SPI SS
    7,                  // CE (standby)
    9,                  // TRX (RX/TX mode)
    8,                  // PWR (power down)
    4,                  // CD (collision avoid)
    3,                  // DR (data ready)
    NRF905_PIN_UNUSED,  // AM (address match)
    nRF905_int_dr,      // Interrupt function for DR
    nRF905_int_am       // Interrupt function for AM
  );

  transceiver.setBand(NRF905_BAND_433);
  transceiver.setCRC(NRF905_CRC_16);


  // Register event functions
  transceiver.events(
    nRF905_onRxComplete,
    nRF905_onRxInvalid,
    NULL,
    NULL);

  // Set address of this device
  transceiver.setListenAddress(RXADDR);

  // Put into receive mode
  transceiver.RX();


  packetStatus = PACKET_NONE;



  while (WiFi.status() != WL_CONNECTED) {
    Serial.print("Attempting to connect to Network named: ");
    Serial.println(ssid);
    //WiFi.beginEnterprise(ssid, "remmler_889789", pass);
    WiFi.begin(ssid, pass);

    wifiClient.setTimeout(10000);
  }
  Serial.println("Connected to WiFi");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

  ArduinoBearSSL.onGetTime(getTime);
  sslClient.setEccSlot(0, certificate);

  Serial.println(F("Server started"));
}

void connectMQTT() {
  Serial.print("Attempting to MQTT broker: ");
  Serial.print(broker);
  Serial.println(" ");

  while (!mqttClient.connect(broker, 8883)) {
    // failed, retry
    Serial.print(".");
    delay(5000);
  }
  Serial.println();

  Serial.println("You're connected to the MQTT broker");
  Serial.println();

  // subscribe to a topic
  mqttClient.subscribe("arduino/incoming");
}


void loop() {

  if (!mqttClient.connected()) {
    // MQTT client is disconnected, connect
    connectMQTT();
  }

  mqttClient.poll();

  if (packetStatus != PACKET_NONE) {  // GOT PACKET FROM INTERRUPT
    Serial.println(packetStatus);


    if (packetStatus != PACKET_OK) {
      packetStatus = PACKET_NONE;
      //Serial.println(F("Invalid packet!"));
      transceiver.RX();
    } else {
      packetStatus = PACKET_NONE;


      Serial.println(F("Got packet"));

      // Make buffer for data
      uint8_t buffer[PAYLOAD_SIZE];


      // Read payload
      transceiver.read(buffer, sizeof(buffer));

      int zeros = 0;
      for (int i = 0; i < 32; i++) {
        if (buffer[i] == 0) zeros++;
      }
      if (zeros == 32) {
        Serial.println(F("All Values 0, stopping"));
        transceiver.RX();
        return;
      }

      //extract the packet id
      long packetID;
      memcpy(&packetID, buffer, sizeof(packetID));

      long roomId;
      memcpy(&roomId, buffer + sizeof(long), sizeof(roomId));

      long passCardId;
      memcpy(&passCardId, buffer + sizeof(long) * 2, sizeof(passCardId));


      //if already got a packet with this id, return;
      if (checkIfNumberAlreadyInCache(packetID)) {
        Serial.println(F("Number already in cache"));
        transceiver.RX();
        return;
      }

      Serial.println(F("Not in Cache"));

      addNumber(packetID);

      Serial.println(F("Added to Cache"));

      if (WiFi.status() == WL_CONNECTED) {
        String json = "{\"passId\": \"" + String(packetID) + "\", \"passCardId\": \"" + String(passCardId) + "\"}";
        Serial.println(F("Sending info: "));
        Serial.print(json);
        mqttClient.beginMessage("node/" + String(roomId) + "/data");
        mqttClient.print(json);
        mqttClien t.endMessage();
        transceiver.RX();
      } else {
        Serial.println("WiFi Disconnected");
      }

      for (uint8_t i = 0; i < PAYLOAD_SIZE; i++) {

        Serial.print(F(" "));
        Serial.print(i);
        Serial.print(F(": "));
        Serial.print(buffer[i], DEC);
      }
      Serial.println(F("---"));
    }
  }
}

unsigned long getTime() {
  // get the current time from the WiFi module
  return WiFi.getTime();
}

void addNumber(long newNumber) {
  NumberNode* newNode = new NumberNode();
  if (newNode == nullptr) {
    Serial.println("Error: Failed to allocate memory for a new node.");
  }


  newNode->number = newNumber;
  newNode->next = nullptr;


  if (head == nullptr) {
    head = newNode;
  } else {
    NumberNode* current = head;
    while (current->next != nullptr) {
      current = current->next;
    }
    current->next = newNode;
  }
}


bool checkIfNumberAlreadyInCache(long target) {
  NumberNode* current = head;
  while (current != nullptr) {
    if (current->number == target) {
      return true;
    }
    current = current->next;
  }
  return false;
}