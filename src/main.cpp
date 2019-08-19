#include <Arduino.h>
#include <ESP8266WiFi.h>

// ===== Settings ===== //
const uint8_t channels[] = {1, 6, 11}; // used Wi-Fi channels (available: 1-14)
const bool wpa2 = false;               // WPA2 networks

const String ssids[] = {
    "Never gonna give you up",
    "Never gonna let you down",
    "Never gonna run around and desert you",
    "Never gonna make you cry",
    "Never gonna say goodbye",
    "Never gonna tell a lie and hurt you"};

extern "C"
{
#include "user_interface.h"
  typedef void (*freedom_outside_cb_t)(uint8 status);
  int wifi_register_send_pkt_freedom_cb(freedom_outside_cb_t cb);
  void wifi_unregister_send_pkt_freedom_cb(void);
  int wifi_send_pkt_freedom(uint8 *buf, int len, bool sys_seq);
}

uint8_t channelIndex = 0;
uint8_t macAddr[6];
uint8_t wifi_channel = 1;
uint32_t packetSize = 0;
uint32_t packetCounter = 0;
uint32_t lastTime = 0;
uint32_t packetRateTime = 0;

// beacon frame definition
uint8_t beaconPacket[109] = {
    /*  0 - 3  */ 0x80, 0x00, 0x00, 0x00,             // Type/Subtype: managment beacon frame
    /*  4 - 9  */ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, // Destination: broadcast
    /* 10 - 15 */ 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, // Source
    /* 16 - 21 */ 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, // Source

    // Fixed parameters
    /* 22 - 23 */ 0x00, 0x00,                                     // Fragment & sequence number (will be done by the SDK)
    /* 24 - 31 */ 0x83, 0x51, 0xf7, 0x8f, 0x0f, 0x00, 0x00, 0x00, // Timestamp
    /* 32 - 33 */ 0xe8, 0x03,                                     // Interval: 0x64, 0x00 => every 100ms - 0xe8, 0x03 => every 1s
    /* 34 - 35 */ 0x31, 0x00,                                     // capabilities Tnformation

    // Tagged parameters

    // SSID parameters
    /* 36 - 37 */ 0x00, 0x20, // Tag: Set SSID length, Tag length: 32
    /* 38 - 69 */ 0x20, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20, // SSID

    // Supported Rates
    /* 70 - 71 */ 0x01, 0x08, // Tag: Supported Rates, Tag length: 8
    /* 72 */ 0x82,            // 1(B)
    /* 73 */ 0x84,            // 2(B)
    /* 74 */ 0x8b,            // 5.5(B)
    /* 75 */ 0x96,            // 11(B)
    /* 76 */ 0x24,            // 18
    /* 77 */ 0x30,            // 24
    /* 78 */ 0x48,            // 36
    /* 79 */ 0x6c,            // 54

    // Current Channel
    /* 80 - 81 */ 0x03, 0x01, // Channel set, length
    /* 82 */ 0x01,            // Current Channel

    // RSN information
    /*  83 -  84 */ 0x30, 0x18,
    /*  85 -  86 */ 0x01, 0x00,
    /*  87 -  90 */ 0x00, 0x0f, 0xac, 0x02,
    /*  91 -  92 */ 0x02, 0x00,
    /*  93 - 100 */ 0x00, 0x0f, 0xac, 0x04, 0x00, 0x0f, 0xac, 0x04, /*Fix: changed 0x02(TKIP) to 0x04(CCMP) is default. WPA2 with TKIP not supported by many devices*/
    /* 101 - 102 */ 0x01, 0x00,
    /* 103 - 106 */ 0x00, 0x0f, 0xac, 0x02,
    /* 107 - 108 */ 0x00, 0x00};

// goes to next channel
void nextChannel()
{
  if (sizeof(channels) > 1)
  {
    wifi_channel = channels[channelIndex++];
    if (channelIndex > sizeof(channels))
      channelIndex = 0;
      wifi_set_channel(wifi_channel);
  }
}

// generates random MAC
void randomMac()
{
  for (int i = 0; i < 6; i++)
    macAddr[i] = random(256);
}

void setup()
{
  // for random generator
  randomSeed(os_random());

  // set packetSize
  packetSize = sizeof(beaconPacket);
  if (wpa2)
    beaconPacket[34] = 0x31;
  else
  {
    beaconPacket[34] = 0x21;
    packetSize -= 26;
  }

  // generate random mac address
  randomMac();

  // start serial
  Serial.begin(115200);
  Serial.println();

  // start WiFi
  WiFi.mode(WIFI_OFF);
  wifi_set_opmode(STATION_MODE);

  // set channel
  wifi_set_channel(channels[0]);
}

void loop()
{
  auto currentTime = millis();
  if (currentTime - lastTime < 100)
    return;

  lastTime = currentTime;

  // go to next channel
  nextChannel();

  auto ssidNum = 0;
  while (ssidNum < sizeof(ssids) / sizeof(ssids[0]))
  {
    const char* emptySsid = "                                ";
    auto ssid = (String(ssidNum) + " " + ssids[ssidNum] + emptySsid).substring(0, 32);

    // set MAC address
    macAddr[5] = ssidNum++;

    // write MAC address into beacon frame
    memcpy(&beaconPacket[10], macAddr, 6);
    memcpy(&beaconPacket[16], macAddr, 6);

    // write new SSID into beacon frame
    memcpy_P(&beaconPacket[38], ssid.c_str(), 32);

    // set channel for beacon frame
    beaconPacket[82] = wifi_channel;

    // send packet
    for (int k = 0; k < 3; k++)
    {
      packetCounter += wifi_send_pkt_freedom(beaconPacket, packetSize, 0) == 0;
      delay(1);
    }
  }

  // show packet-rate each second
  if (currentTime - packetRateTime > 1000)
  {
    packetRateTime = currentTime;
    Serial.print("Packets/s: ");
    Serial.println(packetCounter);
    packetCounter = 0;
  }
}