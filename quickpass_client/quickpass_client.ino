/*
 * Project: nRF905 Radio Library for Arduino (Ping server example)
 * Author: Zak Kemblae, contact@zakkemble.net
 * Copyright: (C) 2020 by Zak Kemble
 * License: GNU GPL v3 (see License.tx
t)
 * Web: https://blog.zakkemble.net/nrf905-avrarduino-librarydriver/
 */


/*
 * Listen for packets and send them back
 */


#include <nRF905.h>
#include <SPI.h>
#include <MFRC522.h>
#include "pitches.h"
#include <EEPROM.h>


#define RXADDR 0xE7E7E7E7 // Address of this device
#define TXADDR 0xE7E7E7E7 // Address of device to send to


#define PACKET_NONE   0
#define PACKET_OK   1
#define PACKET_INVALID  2


#define LED       A5
#define PAYLOAD_SIZE  NRF905_MAX_PAYLOAD


#define RADIO_SS_PIN         10
#define RFID_RST_PIN         9          // Configurable, see typical pin layout above
#define RFID_SS_PIN          6


struct NumberNode {
  long number;
  NumberNode* next = nullptr;
};


NumberNode* head = nullptr;


long lastCard;
long newPacketID;
long cooldown = 100;
long roomId;


nRF905 transceiver = nRF905();
MFRC522 mfrc522(RFID_SS_PIN, RFID_RST_PIN);


static volatile uint8_t packetStatus;


// Don't modify these 2 functions. They just pass the DR/AM interrupt to the correct nRF905 instance.
void nRF905_int_dr(){transceiver.interrupt_dr();}
void nRF905_int_am(){transceiver.interrupt_am();}


// Event function for RX complete
void nRF905_onRxComplete(nRF905* device)
{
  packetStatus = PACKET_OK;
  transceiver.standby();
}


// Event function for RX invalid
void nRF905_onRxInvalid(nRF905* device)
{
  packetStatus = PACKET_INVALID;
  transceiver.standby();
}


void setup()
{
  Serial.begin(115200);


  Serial.println(F("Server starting..."));
  
  // This must be called first
  SPI.begin();
  long seed = analogRead(A0) + analogRead(A1) * 482;
  Serial.println(seed);
  randomSeed(seed);
  enableRFID();

  mfrc522.PCD_Init();
  delay(4);


  enableRadio();

  transceiver.begin(
    SPI, // SPI bus to use (SPI, SPI1, SPI2 etc)
    10000000, // SPI Clock speed (10MHz)
    RADIO_SS_PIN, // SPI SS
    7, // CE (standby)
    9, // TRX (RX/TX mode)
    8, // PWR (power down)
    4, // CD (collision avoid)
    3, // DR (data ready)
    NRF905_PIN_UNUSED, // AM (address match)
    nRF905_int_dr, // Interrupt function for DR
    nRF905_int_am // Interrupt function for AM
  );

  transceiver.setTransmitPower(NRF905_PWR_10);
  transceiver.setBand(NRF905_BAND_433);
  transceiver.setCRC(NRF905_CRC_16);
 
  // Register event functions
  transceiver.events(
    nRF905_onRxComplete,
    nRF905_onRxInvalid,
    NULL,
    NULL
  );
 
  // Set address of this device
  transceiver.setListenAddress(RXADDR);


  // Put into receive mode
  transceiver.RX();


  enableRFID();


  packetStatus = PACKET_NONE;


  mfrc522.PCD_DumpVersionToSerial();


  Serial.println(F("Server started, printing roomId"));
  setupRoomID();
}

void setupRoomID()
{
  
  uint8_t a = 0;
  EEPROM.get(0, a);
  uint8_t b = 0;
  EEPROM.get(0, b);
  uint8_t c = 0;
  EEPROM.get(0, c);

  if(a == 123 && b == 123 && c == 123){
    Serial.println(F("Room id: "));
    for (int i = 4; i <= 6 ; i++) {
      roomId <<= 8;
      uint8_t val = 0;
      EEPROM.get(i, val);
      Serial.print(val);
      Serial.print(F(" "));
      roomId |= (unsigned long) val;
    }
    Serial.print(roomId);
    return;
  }

  EEPROM.write(0, 123); 
  EEPROM.write(1, 123);
  EEPROM.write(2, 123); 
 

  for (int i = 4; i <= 6 ; i++) {
    uint8_t random = (analogRead(A0) * 251 + analogRead(A1) * 397);
    EEPROM.write(i, random); 
    Serial.println(F("Wrote EEPROM"));
    Serial.println(random, 5);
  }

  uint8_t g = 0;
  Serial.println(F("Set EEPROM..."));
  EEPROM.get(4, g);
  Serial.println(g, 5);
}


void loop()
{
  if(cooldown > 0) cooldown--;


  if(packetStatus != PACKET_NONE){// GOT PACKET FROM INTERRUPT    


    if(packetStatus != PACKET_OK){
      packetStatus = PACKET_NONE;
      //Serial.println(F("Invalid packet!"));
      transceiver.RX();
    } else {  
      packetStatus = PACKET_NONE;


      Serial.println(F("\n---\nGot packet!\n---"));  


      enableRadio();


      // Make buffer for data
      uint8_t buffer[PAYLOAD_SIZE];


      // Read payload
      transceiver.read(buffer, sizeof(buffer));

      int zeros = 0;
      for(int i=0; i<32; i++){
        if(buffer[i] == 0) zeros++;
      }
      if(zeros ==32){
        Serial.println(F("All Values 0, stopping"));  
        transceiver.RX();
        return;
      }


      //extract the packet id
      long packetID;
      memcpy(&packetID, buffer, sizeof(packetID));  


      //if already got a packet with this id, return;
      if(checkIfNumberAlreadyInCache(packetID)){
        Serial.println(F("Number already in cache"));  
        transceiver.RX();
        return;
      }


      Serial.println(F("Not in Cache"));  


      addNumber(packetID);


      Serial.println(F("Added to Cache"));  


      // Write reply data and destination address to radio
      transceiver.write(TXADDR, buffer, sizeof(buffer));


      // Send the reply data, once the transmission has completed go into receive mode
      while(!transceiver.TX(NRF905_NEXTMODE_RX, true));


      enableRFID();


      Serial.println(F("Reply sent"));

      tone(5, NOTE_GS6, 250);
      tone(5, NOTE_GS4, 250);
      tone(5, NOTE_GS5, 250);


     


      // Show received data
      Serial.print(F("size:"));
      Serial.print(PAYLOAD_SIZE);


      for(uint8_t i=0;i<PAYLOAD_SIZE;i++)
      {


        Serial.print(F(" "));
        Serial.print(i);
        Serial.print(F(": "));
        Serial.print(buffer[i], DEC);
      }
      Serial.println(F("---"));
    }
  }


  if ( ! mfrc522.PICC_IsNewCardPresent()) {
    return;
  }

  // Select one of the cards
  if ( !mfrc522.PICC_ReadCardSerial()) {
    Serial.print(F("Didn't Read"));
    return;
  }


  // Dump debug info about the card; PICC_HaltA() is automatically called
  // mfrc522.PICC_DumpToSerial(&(mfrc522.uid));

  uint8_t sendingBuffer[PAYLOAD_SIZE] = {0};

  Serial.println(F("cooldown"));
  Serial.println(cooldown);



  if(lastCard == &(mfrc522.uid.uidByte) && lastCard != 0 && cooldown > 0){
    return;
  }


  newPacketID = random(4L, 2147483646L);

  addNumber(newPacketID);

  cooldown = 100;

  lastCard = &(mfrc522.uid.uidByte);

  memcpy(sendingBuffer, &newPacketID, sizeof(long));  
  memcpy(sendingBuffer + sizeof(long), &roomId, sizeof(long));
  memcpy(sendingBuffer + sizeof(long) * 2, &(mfrc522.uid.uidByte), sizeof(mfrc522.uid.uidByte));  

  tone(5, NOTE_GS7, 250);

  enableRadio();
  Serial.println(F("sending packet"));
  delay(1);

  transceiver.write(TXADDR, sendingBuffer, sizeof(sendingBuffer));
  while(!transceiver.TX(NRF905_NEXTMODE_RX, true));

  Serial.println(F("sent packet"));

  enableRFID();
 
  Serial.println(F("Sending: "));


  Serial.println(F(""));
 
  for(int i=0; i<32; i++){
    Serial.print(F(" :"));
    Serial.print(i);
    Serial.print(F(" - "));
    Serial.print(sendingBuffer[i]);
  }

  Serial.println(F(""));
  Serial.println(F("---"));
}


void addNumber(long newNumber){
  NumberNode* newNode = new NumberNode();
  if(newNode == nullptr){
    Serial.println("Error: Failed to allocate memory for a new node.");
  }


  newNode->number = newNumber;
  newNode->next = nullptr;


  if(head == nullptr){
    head = newNode;
  } else {
    NumberNode* current = head;
    while(current->next != nullptr){
      current = current->next;
    }
    current->next = newNode;
  }
}


bool checkIfNumberAlreadyInCache(long target){
  NumberNode* current = head;
  while(current != nullptr){
    if(current->number == target){
      return true;
    }
    current = current->next;
  }
  return false;
}


void enableRadio(){
  digitalWrite(RADIO_SS_PIN, LOW);
  digitalWrite(RFID_SS_PIN, HIGH);
}


void enableRFID(){
  digitalWrite(RADIO_SS_PIN, HIGH);
  digitalWrite(RFID_SS_PIN, LOW);
}

