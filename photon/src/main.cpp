/*
Photon Feeder Firmware
Part of the LumenPnP Project
GNU GPL v3
2022
*/

#include "define.h"

#ifdef UNIT_TEST
  #include <ArduinoFake.h>
#else
  #include <Arduino.h>
  #include <HardwareSerial.h>
  #include <OneWire.h>
  #include <ArduinoUniqueID.h>
  #include <rs485/rs485bus.hpp>
#endif // UNIT_TEST

#ifndef MOTOR_DEPS
#define MOTOR_DEPS

#include <RotaryEncoder.h>

#endif 

#include <IndexFeeder.h>
#include <IndexFeederProtocol.h>
#include <IndexNetworkLayer.h>

#include <rs485/rs485bus.hpp>
#include <rs485/bus_adapters/hardware_serial.h>
#include <rs485/protocols/photon.h>
#include <rs485/packetizer.h>

#define BAUD_RATE 57600

//-----
// Global Variables
//-----

byte addr = 0x00;

#ifdef UNIT_TEST
StreamFake ser();
#else
HardwareSerial ser(PA10, PA9);
#endif // ARDUINO

// EEPROM
OneWire oneWire(ONE_WIRE);

// RS485
HardwareSerialBusIO busIO(&ser);
RS485Bus<RS485_BUS_BUFFER_SIZE> bus(busIO, _RE, DE);
PhotonProtocol photon_protocol;
Packetizer packetizer(bus, photon_protocol);

// Encoder
RotaryEncoder encoder(DRIVE_ENC_A, DRIVE_ENC_B, RotaryEncoder::LatchMode::TWO03); 

// PID
double Setpoint, Input, Output;

// Drive mode toggle
bool drive_mode = false;

// Feeder Class Instances
IndexFeeder *feeder;
IndexFeederProtocol *protocol;
IndexNetworkLayer *network;

#define ALL_LEDS_OFF() byte_to_light(0x00)
#define ALL_LEDS_ON() byte_to_light(0xff)

//-------
//FUNCTIONS
//-------

void checkPosition()
{
  encoder.tick(); // just call tick() to check the state.
}

void byte_to_light(byte num){
  digitalWrite(LED_R, !bitRead(num, 0));
  digitalWrite(LED_G, !bitRead(num, 1));
  digitalWrite(LED_B, !bitRead(num, 2));
}

void bootAnimation(){
  for(int i = 0;i<3;i++){
    digitalWrite(LED_R, LOW);
    digitalWrite(LED_G, LOW);
    digitalWrite(LED_B, LOW);

    delay(100);

    digitalWrite(LED_R, HIGH);
    digitalWrite(LED_G, HIGH);
    digitalWrite(LED_B, HIGH);

    delay(100);
  }

}

byte read_floor_address(){
  byte i;
  byte data[32]; 
  
  // reset the 1-wire line, and return false if no chip detected
  if(!oneWire.reset()){
    addr = 0xFF;
    return 0xFF;
  }

  // Send 0x3C to indicate skipping the ROM selection step; there'll only ever be one ROM on the bus
  oneWire.skip(); 

  // array with the commands to initiate a read, DS28E07 device expect 3 bytes to start a read: command,LSB&MSB adresses
  byte leemem[3] = {
    0xF0, 
    0x00,
    0x00
  }; 

  // sending those three bytes
  oneWire.write(leemem[0],1);
  oneWire.write(leemem[1],1);
  oneWire.write(leemem[2],1);

  for ( i = 0; i < 32; i++) {    // Now it's time to read the PROM data itself, each page is 32 bytes so we need 32 read commands
    data[i] = oneWire.read();    // we store each read byte to a different position in the data array
  }

  addr = data[0];

  // return the first byte from returned data
  return data[0];
}

byte write_floor_address(byte address){
/*
    write_floor_address()
      success returns programmed address byte
      failure returns 0xFF

This function takes a byte as in input, and flashes it to address 0x0000 in the eeprom (where the floor ID is stored).
The DS28E07 requires a million and one steps to make this happen. Reference the datasheet for details:
https://datasheets.maximintegrated.com/en/ds/DS28E07.pdf
*/


  byte i;                         // This is for the for loops
  //-----
  // Write To Scratchpad
  //-----

  byte data[8] = {address, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  
  // reset the 1-wire line, and return false if no chip detected
  if(!oneWire.reset()){
    return 0xFF;
  }

  // Send 0x3C to indicate skipping the ROM selection step; there'll only ever be one ROM on the bus
  oneWire.skip(); 

  // array with the commands to initiate a write to the scratchpad, DS28E07 device expect 3 bytes to start a read: command,LSB&MSB adresses
  byte leemem[3] = {
    0x0F, 
    0x00,
    0x00
  }; 

  // sending those three bytes
  oneWire.write(leemem[0], 1);
  oneWire.write(leemem[1], 1);
  oneWire.write(leemem[2], 1);

  // Now it's time to actually write the data to the scratchpad
  for ( i = 0; i < 8; i++) {    
    oneWire.write(data[i], 1);
  }

  // read back the CRC
  byte ccrc = oneWire.read();

  //-----
  // Read Scratchpad
  //-----

  // array for the data we'll read back
  byte read_data[11];

  // byte for the ccrc the eeprom will send us
  byte scratchpad_ccrc;

  // reset the 1-wire line, and return failure if no chip detected
  if(!oneWire.reset()){
    return 0xFF;
  }

  // Send 0x3C to indicate skipping the ROM selection step; there'll only ever be one ROM on the bus
  oneWire.skip(); 

  // send read scratchpad command
  oneWire.write(0xAA, 1);

  //read in TA1, TA2, and E/S bytes, then the 8 bytes of data
  for ( i = 0; i < 11; i++) {    
    read_data[i] = oneWire.read(); 
  }

  //read crc
  scratchpad_ccrc = oneWire.read();

  byte ccrc_calc = OneWire::crc8(read_data, 11);

  // TODO need to be checking CCRC. never returns true, even when data is identical.
  // if(scratchpad_ccrc != ccrc_calc){
  //   // do nothing
  // }

  //-----
  // Copy Scratchpad to Memory
  //-----

  // reset the 1-wire line, and return false if no chip detected
  if(!oneWire.reset()){
    return 0xFF;
  }

  // Send 0x3C to indicate skipping the ROM selection step; there'll only ever be one ROM on the bus
  oneWire.skip(); 

  // copy scratchpad command
  oneWire.write(0x55, 1);

  // sending auth bytes from scratchpad read
  oneWire.write(read_data[0],1);
  oneWire.write(read_data[1],1);
  oneWire.write(read_data[2],1);

  // wait for programming, we'll get alternating 1s and 0s when done
  float timer = millis();
  while(true){
    if(oneWire.read() == 0xAA){
      break;
    }
    if( (millis() - timer) > 20 ){ // datasheet says it should only ever take 12ms at most to program
      break;
    }
  }

  // send reset
  if(!oneWire.reset()){
    return 0xFF;
  }

  // check the floor address by reading
  byte written_address = read_floor_address();

  if(written_address == address){
    // set global address varaible to new address
    addr = address;

    //return new address
    return address;
  }

  return 0xFF;

}

bool select_floor_address(){

  // put current address into a variable we'll manipulate
  byte current_selection = addr;

  // turn off LEDs so we're starting with a blank slate
  ALL_LEDS_ON();

  // wait until both buttons have been released
  while(!digitalRead(SW1) || !digitalRead(SW2)){
    //do nothing
  }

  ALL_LEDS_OFF();

  while(true){
    //we stay in here as long as both buttons aren't pressed
    if(!digitalRead(SW1) && current_selection > 1){
      delay(LONG_PRESS_DELAY);
      if(!digitalRead(SW1) && !digitalRead(SW2)){
        break;
      }
      current_selection = current_selection - 1;
      while(!digitalRead(SW1)){
        //do nothing
      }
    }
    if(!digitalRead(SW2) && current_selection < 63){
      delay(LONG_PRESS_DELAY);
      if(!digitalRead(SW1) && !digitalRead(SW2)){
        break;
      }
      current_selection = current_selection + 1;
      while(!digitalRead(SW2)){
        //do nothing
      }
    }
    byte_to_light(current_selection);
  }

  ALL_LEDS_ON();

  // wait for the buttons to come up
  while(!digitalRead(SW1) || !digitalRead(SW2)){
    //do nothing
  }

  byte written_address = write_floor_address(current_selection);

  // if failed to write
  if(written_address == 0x80){
    while(true){
      byte_to_light(0x03);
      delay(100);
      ALL_LEDS_OFF();
      delay(100);
    }
    return false;
  }

  // If the network is configured, update the local address
  // to the newly selected address.
  if (network != NULL) {
    network->setLocalAddress(addr);
  }

  for (int i = 0; i < 8; i++){
    byte_to_light(i);
    delay(30);
  }

  byte_to_light(addr);

  return true;

}

//-------
//SETUP
//-------

void setup() {
  pinMode(LED_R, OUTPUT);
  pinMode(LED_G, OUTPUT);
  pinMode(LED_B, OUTPUT);
  pinMode(SW1, INPUT_PULLUP);
  pinMode(SW2, INPUT_PULLUP);
  
  bootAnimation();

  // put current floor address on the leds if detected

  addr = read_floor_address();

  if(addr == 0xFF){ // not detected, turn red
    byte_to_light(0x01);
  }
  else if (addr == 0x00){ //not programmed, turn blue
    byte_to_light(0x04);
  }

  //Starting rs-485 serial
  ser.begin(BAUD_RATE);

  // attach interrupts for encoder pins
  attachInterrupt(digitalPinToInterrupt(DRIVE_ENC_A), checkPosition, CHANGE);
  attachInterrupt(digitalPinToInterrupt(DRIVE_ENC_B), checkPosition, CHANGE);

  // Setup Feeder
  feeder = new IndexFeeder(DRIVE1, DRIVE2, PEEL1, PEEL2, &encoder);
  protocol = new IndexFeederProtocol(feeder, UniqueID, UniqueIDsize);
  network = new IndexNetworkLayer(&packetizer, &bus, addr, protocol);
}

//------
//  MAIN CONTROL LOOP
//------

void loop() {

  // Checking SW1 status to go backward, or initiate settings mode
  if(!digitalRead(SW1)){
    delay(LONG_PRESS_DELAY);

    if(!digitalRead(SW1)){

      if(!digitalRead(SW2)){
        //both are pressed, entering settings mode
        byte_to_light(0x04);
        if(drive_mode){
          drive_mode = false;
        }
        else{
          drive_mode = true;
        }
        while(!digitalRead(SW1) || !digitalRead(SW2)){
          //do nothing while waiting for debounce
        }
        byte_to_light(0x00);
      }
      else{
        //we've got a long press, lets reverse 
        while(!digitalRead(SW1)){

          if(drive_mode){
            feeder->peel(50, false);
          }
          else{
            feeder->driveTape(false);
          }
        }
        feeder->brakeDrive(200);
        feeder->stop();
        feeder->setEncoderPosition(0);
        feeder->setMmPosition(0);
        
      }
      
    }
    else{
      //feeder->feedDistance(40, false);
      feeder->checkLoaded();
      
    }
  }

  // Checking SW2 status to go forward
  if(!digitalRead(SW2)){
    delay(LONG_PRESS_DELAY);

    if(!digitalRead(SW2)){

      if(!digitalRead(SW1)){
        //both are pressed, entering settings mode
        byte_to_light(0x04);
        if(drive_mode){
          drive_mode = false;
        }
        else{
          drive_mode = true;
        }
        while(!digitalRead(SW1) || !digitalRead(SW2)){
          //do nothing while waiting for debounce
        }
        byte_to_light(0x00);
      }
      else{
        //we've got a long press, lets peel film 
        while(!digitalRead(SW2)){
          if(drive_mode){
            feeder->peel(50, true);
          }
          else{feeder->driveTape(true);}
        }
        feeder->brakeDrive(200);
        feeder->stop();
        feeder->setEncoderPosition(0);
        feeder->setMmPosition(0);
      }
      
    }
    else{
      byte_to_light(0x02);
      if(feeder->feedDistance(40, true) == Feeder::FeedResult::SUCCESS){
        byte_to_light(0x00);
      }
      else{
        byte_to_light(0x01);
      }
      
    }  
  }
  
  //listening on rs-485 for a command
  if (network != NULL) {
    // byte_to_light(bus.available());
    // delay(1000);
    bus.fetch();
    uint8_t id = network->tick();
    //byte_to_light(id);


    // if(id == 0x0F) {
    //   delay(1000);
    // }
    // byte_to_light(id);
    // bus.fetch();

    // byte led_status = 0x40;
    // for(size_t i=0; i < bus.available(); i++) {
    //   IsPacketResult result = photon_protocol.isPacket(bus, i, bus.available()-1);
    //   if (result.status == PacketStatus::YES) {
    //     led_status |= (1 << i);
    //   }
    // }
    // byte_to_light(led_status);
    // while(true) {}
  }



  // this chunk just reads in bytes and puts them on the leds
  // byte buffer[1];
  // while(ser.available()){
  //   ser.readBytes(buffer, 1);
  //   byte_to_light(buffer[0]);
  // }

  // end main loop
}
