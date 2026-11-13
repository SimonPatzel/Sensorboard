#include <stdint.h>
#include "mbedtls/aes.h"
#include <WiFi.h>
#include <WiFiUdp.h>

#define LIDAR_RX 4   // LiDAR RX -> ESP32 TX
#define LIDAR_TX 5   // LiDAR TX -> ESP32 RX
#define Serial_Lidar Serial1

#define LIDAR_RESPONSE_MAX 7     //längste erwartete Antwort (Response-Descriptor)
#define LIDAR_TIMEOUT_MS 5000    //maximale Wartezeit auf eine Antwort des LiDARs
#define LIDAR_SYNC_ATTEMPTS 10   //maximale Anzahl verworfener Bytes, bevor der Scan neu gestartet wird

const int RPLIDAR_BAUD = 115200;

uint8_t radarMotorPin = 26;      //Pin, an dem der Radar Motor angeschlossen ist
uint8_t lidarMotorPin = 16;      //Pin, dan dem der LiDAR Motor angeschlossen ist
uint8_t sliderSize = 255;        //Slider-Größe in der GUI
float dutyCycle;

bool scanActive = false;                 //gibt an, ob aktuell der Scan des LiDARs läuft
bool scanDescriptorReceived = false;     //gibt an, ob der Descriptor nach dem Start des Scans schon angekommen ist

const uint8_t qualityThreshold = 0;      //quality geht von 0 bis 63, alles unter Threshold wird weggeworfen


static const size_t POINTS_PER_BLOCK = 4;                     //muss vielfaches von 4 sein, Punkte pro Verschlüsselungs, bzw. Sendepacket
static const size_t BLOCK_SIZE       = 4 * POINTS_PER_BLOCK;  //Bytegröße des Packets (4 Byte pro Punkt)

uint16_t pointsToEncryptBuffer[POINTS_PER_BLOCK][2];          //Buffer für zu verschlüsselnde Punkte (2 Elemente (Distanz, Winkel) pro Paket)
uint8_t encryptBufferIndex = 0;                               //Index, wie viele Punkte schon im Paket sind
uint8_t encryptedPointsOutput[BLOCK_SIZE];                    //Sendepaket

const uint8_t AES_KEY[16] = {                                 //Verschlüsselungs-Schlüssel
    0x2B, 0x2B, 0x2B, 0x2B, 0x2B, 0x2B, 0x2B, 0x2B,
    0x2B, 0x2B, 0x2B, 0x2B, 0x2B, 0x2B, 0x2B, 0x2B
};

mbedtls_aes_context aesEnc;
mbedtls_aes_context aesDec;


const char* ssid = "TRML4D-AP";               //SSID vom Raspberry Pi WLAN
const char* password = "testtest";            //Passwort vom Raspberri Pi WLAN
const char* hostname = "TRML4D";              
const uint16_t TCP_PORT = 5000;
const uint16_t UDP_PORT = 5001;

IPAddress local_IP(192, 168, 4, 100);
IPAddress gateway(192, 168, 4, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress primaryDNS(192, 168, 4, 1);

IPAddress piIP = IPAddress(192, 168, 4, 1);

WiFiServer server(TCP_PORT);                 //startet den TCP Server
WiFiClient client;
WiFiUDP udp;


//Struktur mit allen Daten eines LiDAR-Punkts
struct lidarPoint {
  uint16_t angleDeg;
  uint16_t distanceMm;
  int quality;
  bool startOfScan;
};


//mögliche Health-Status-Werte des LiDAR Sensors
enum lidarHealthStatus: int {
  health_good = 0,
  health_warning = 1,
  health_error = 2
};

//setzt den PWM-Wert auf den beiden Pins für die beiden Motoren (skaliert auf 0-255)
void setMotorSpeed(uint8_t motorSpeed) {
  dutyCycle = map(motorSpeed, 0, sliderSize, 0, 255);
  analogWrite(lidarMotorPin, dutyCycle);
  analogWrite(radarMotorPin, dutyCycle);
  return;
}

//entschlüsselt den Motor-Geschwindigkeits-Wert vom Raspberri Pi
int decryptMotorSpeed(const uint8_t in[16], uint8_t &motorSpeed) {
    uint8_t out[16];
    int ret = mbedtls_aes_crypt_ecb(&aesDec, MBEDTLS_AES_DECRYPT, in, out);
    if (ret != 0) return ret;
    motorSpeed = out[0];
    return 0;
}

//Handelt den Motor-Geschwindigkeits-Wert vom Raspberri Pi
void handleValue(const uint8_t value[16]) {
    uint8_t speed = 0;
    if (decryptMotorSpeed(value, speed) != 0) {
        Serial.println("Entschluesselung fehlgeschlagen");
        return;
    }
    Serial.printf("Wert: %u (0x%02X)\n", speed, speed);
    setMotorSpeed(speed);
}

//sendet die LiDAR Daten an den UDP-Port vom Raspberri Pi
void sendSensorData(uint8_t encryptedData[BLOCK_SIZE]) {
    udp.beginPacket(piIP, UDP_PORT);
    udp.write(encryptedData, BLOCK_SIZE);
    udp.endPacket();
}

//schreibt Serial-Output, falls WLAN disconnected
void onWiFiEvent(arduino_event_id_t event, arduino_event_info_t info) {
    if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
        Serial.printf("Disconnect-Grund: %d\n", info.wifi_sta_disconnected.reason);
    }
}

//initialisiert die WLAN-Verbindiúng zum Raspberri Pi
void initWiFi() {
    WiFi.persistent(false);
    WiFi.disconnect(true);
    delay(100);

    WiFi.mode(WIFI_STA);
    if(!WiFi.config(local_IP, gateway, subnet, primaryDNS)) {
        Serial.println("Fehler bei der IP-Konfiguration");
    }
    WiFi.setHostname(hostname);

    WiFi.onEvent(onWiFiEvent);

    WiFi.begin(ssid, password);
    Serial.print("Connecting to WiFi ...");
    while(WiFi.status() != WL_CONNECTED) {
        Serial.print('.');
        Serial.println(WiFi.status());
        delay(1000);
    }
    Serial.print("\nESP32 IP Address: ");
    Serial.println(WiFi.localIP());
    Serial.print("RSSI: ");
    Serial.println(WiFi.RSSI()); //Verbindungsstärke
}


//Funktion zum Empfangen einer Antwort des LiDARs
//gibt nullptr zurück, wenn die Antwort nicht innerhalb von LIDAR_TIMEOUT_MS vollständig ankommt
int *lidar_response(int response_length) {
  static int lidar_response[LIDAR_RESPONSE_MAX];
  if (response_length > LIDAR_RESPONSE_MAX) {
    return nullptr;
  }
  int received = 0;
  unsigned long startTime = millis();
  while(received < response_length) {
    if (Serial_Lidar.available()) {
      lidar_response[received++] = Serial_Lidar.read(); //schreibt die Antwort-Bytes des LiDARs in die response
    } else if (millis() - startTime > LIDAR_TIMEOUT_MS) {
      Serial.println("Fehler: Timeout beim Warten auf den LiDAR");
      return nullptr;
    }
  }
  return lidar_response;
}

//Verschlüsselt 4 Punkte (16Byte), Rückgabe 0 bei Erfolg
int encryptPoints(const uint16_t points[POINTS_PER_BLOCK][2], uint8_t out[BLOCK_SIZE]) {
    uint8_t in[BLOCK_SIZE];
    for (size_t i = 0; i < POINTS_PER_BLOCK; i++) {
        in[i * 4 + 0] = points[i][0] & 0xFF;
        in[i * 4 + 1] = points[i][0] >> 8;
        in[i * 4 + 2] = points[i][1] & 0xFF;
        in[i * 4 + 3] = points[i][1] >> 8;
    }
    return mbedtls_aes_crypt_ecb(&aesEnc, MBEDTLS_AES_ENCRYPT, in, out);
}

//Alte Bytes aus dem Buffer verwerfen
void deleteBuffer() {
  while(Serial_Lidar.available()) {
    Serial_Lidar.read();
  }
}

//startet den Scan beim LiDAR
void startScan() {
  deleteBuffer();

  const uint8_t startScanCommand[2] = {0xA5, 0x20};

  scanDescriptorReceived = false;

  Serial_Lidar.write(startScanCommand, sizeof(startScanCommand));
  Serial_Lidar.flush();

  scanActive = true;
  Serial.println("Scan gestartet");
}

//prüft die Prüfbits eines Messpunkts: S-quer muss das Gegenteil von S sein und C muss 1 sein
bool checkBitsValid(int lidarData[5]) {
  bool s = lidarData[0] & 0x01;           //S: Bit 0 von Byte 0
  bool sInv = (lidarData[0] >> 1) & 0x01; //S-quer: Bit 1 von Byte 0
  bool c = lidarData[1] & 0x01;           //C: Bit 0 von Byte 1
  return (s != sInv) && c;
}

//Scan stoppen, Puffer leeren und neu starten (z.B. nach Fehlern oder Timeout)
void restartScan() {
  Serial.println("Scan wird neu gestartet");
  stopScan();
  startScan();
}

//frägt ein Lidarpunkt vom Sensor ab und schreibt diesen in ein lidarPoint struct
void pollLidar() {
  //Prüfen, ob der Scan läuft
  if (!scanActive) {
    Serial.println("Scan nicht aktiv");
    return;
  }

  //Prüfen ob der Descriptor schon angekommen ist
  if (!scanDescriptorReceived) {
    Serial.println("ScanDescriptor nicht angekommen");
    int *response_descriptor = lidar_response(7);

    if (response_descriptor == nullptr) {
      restartScan();
      Serial.println("ResponseDescriptor ist nullptr");
      return;
    }

    bool responseValid =
      response_descriptor[0] == 0xA5 &&
      response_descriptor[1] == 0x5A &&
      response_descriptor[2] == 0x05 &&
      response_descriptor[3] == 0x00 &&
      response_descriptor[4] == 0x00 &&
      response_descriptor[5] == 0x40 &&
      response_descriptor[6] == 0x81;

    if(!responseValid) {
      Serial.println("Fehler: Unerwarteter Response-Descriptor");
      restartScan();
      return;
    }

    //Wenn der Descriptor korrekt angekommen ist, setzen wir True
    scanDescriptorReceived = true;
    Serial.println("ScanDescriptor angekommen");
  }


  int *scanBuffer = lidar_response(5);
  if (scanBuffer == nullptr) {
    restartScan();
    return;
  }

  //Kopie anlegen, da der Puffer von lidar_response() beim Nachlesen überschrieben wird
  int packet[5];
  for (int i = 0; i < 5; i++) {
    packet[i] = scanBuffer[i];
  }

  //Prüfbits testen; bei Fehler ein Byte verwerfen und ein neues Byte hinten anhängen, bis das Paket wieder passt
  int syncAttempts = 0;
  while (!checkBitsValid(packet)) {
    if (++syncAttempts > LIDAR_SYNC_ATTEMPTS) {
      Serial.println("Fehler: Prüfbits ungültig, keine Synchronisation möglich");
      restartScan();
      return;
    }

    for (int i = 0; i < 4; i++) {
      packet[i] = packet[i + 1];
    }

    int *nextByte = lidar_response(1);
    if (nextByte == nullptr) {
      restartScan();
      return;
    }
    packet[4] = nextByte[0];
  }

  parseMeasurement(packet);
}

//verarbeitet die Bytes aus dem LiDAR zu nutzbaren Werten
void parseMeasurement(int lidarData[5]) {
  struct lidarPoint currentLidarPoint;
  currentLidarPoint.startOfScan = lidarData[0] & 0x01; //Bitweiser Vergleich, falls S (erstes Bit) = 1, ist startOfScan true
  currentLidarPoint.quality = lidarData[0] >> 2; //Bitshift nach rechts "verwirft" S-flags und lässt nur die Bits der Quality übrig (von 0-64)
  currentLidarPoint.angleDeg = (lidarData[1] >> 1) | (lidarData[2] << 7); //Bitshifts und OR zum auswerten der Fixpoint number, /64 noch notwendig
  currentLidarPoint.distanceMm = lidarData[3] | (lidarData[4] << 8); //wie angleDeg, /4 noch notwendig

  if(currentLidarPoint.quality >= qualityThreshold && currentLidarPoint.distanceMm != 0) {
    //Serial.println("Qualität hoch genug");
    uint16_t pointPackage[2] = {currentLidarPoint.angleDeg, currentLidarPoint.distanceMm};

    if(encryptBufferIndex < 4) {
      pointsToEncryptBuffer[encryptBufferIndex][0] = pointPackage[0];
      pointsToEncryptBuffer[encryptBufferIndex][1] = pointPackage[1];
      encryptBufferIndex++;
    }
    if(encryptBufferIndex == 4) {
      encryptPoints(pointsToEncryptBuffer, encryptedPointsOutput);
      encryptBufferIndex = 0;
      sendSensorData(encryptedPointsOutput);
    }
  }
  else {      //falls Qualität unter Threshold
    Serial.println("Qualität zu niedrig");
  }
}




//sendet ein Reset Kommando an den LiDAR und startet diesen neu
void sendReset() {
  const uint8_t resetCommand[2] = {0xA5, 0x40};

  Serial_Lidar.write(resetCommand, sizeof(resetCommand));
  Serial_Lidar.flush();

  //Nach RESET mindestens 2 ms warten, bevor der nächste Request gesendet wird.
  delay(20);
  deleteBuffer();
  encryptBufferIndex = 0;
}

//stoppt den aktuell laufenden Scan des LiDARs
void stopScan() {
  const uint8_t stopCommand[2] = {0xA5, 0x25};

  Serial_Lidar.write(stopCommand, sizeof(stopCommand));
  Serial_Lidar.flush();

  //Nach STOP mindestens 1 ms warten, danach noch angekommene Restdaten verwerfen
  delay(20);
  deleteBuffer();

  scanActive = false;
  scanDescriptorReceived = false;
  Serial.println("Scan gestoppt");

}

//getHealth gibt True aus, wenn Health ok ist, sonst False
bool getHealth() {

  deleteBuffer();

  const uint8_t getHealthCommand[2] = {0xA5, 0x52};
  Serial_Lidar.write(getHealthCommand, sizeof(getHealthCommand));
  Serial_Lidar.flush();

  int *response_descriptor = lidar_response(7);

  if (response_descriptor == nullptr) {
    return false;
  }

  bool responseValid =
    response_descriptor[0] == 0xA5 &&
    response_descriptor[1] == 0x5A &&
    response_descriptor[2] == 0x03 &&
    response_descriptor[3] == 0x00 &&
    response_descriptor[4] == 0x00 &&
    response_descriptor[5] == 0x00 &&
    response_descriptor[6] == 0x06;

  if(!responseValid) {
    Serial.println("Fehler: Unerwarteter Response-Descriptor");
    return false;
  }

  int *health = lidar_response(3);

  if (health == nullptr) {
    return false;
  }

  int health_status = health[0];

  //Fehlerauswertung wäre hier noch möglich

  switch(health_status) {
    case health_good:
      Serial.println("LiDAR ist betriebsbereit (Good)");
      return true;
    case health_warning:
      Serial.println("LiDAR hat eine Warnung (Warning)");
      return true;
    case health_error:
      Serial.println("LiDAR hat einen Fehler (Error)");
      return false;
    default:
      Serial.println("LiDAR meldet einen unbekannten Status");
      return false;
  }
}




void setup() {
  Serial.begin(115200); //Verbindung zum Seriellen Monitor in Arduino IDE

  Serial_Lidar.begin(RPLIDAR_BAUD, SERIAL_8N1, LIDAR_RX, LIDAR_TX); //Verbindung zum LiDAR aufbauen

  pinMode(lidarMotorPin, OUTPUT);   //PinModes der Pins für die Motoren auf Output setzen
  pinMode(radarMotorPin, OUTPUT);


  Serial.println("Serielle Verbindung zum LiDAR gestartet");

  mbedtls_aes_init(&aesEnc);
  mbedtls_aes_init(&aesDec);
  mbedtls_aes_setkey_enc(&aesEnc, AES_KEY, 128);
  mbedtls_aes_setkey_dec(&aesDec, AES_KEY, 128);

  initWiFi();
  server.begin();
  Serial.printf("TCP-Server lauscht auf Port %u\n", TCP_PORT);
  Serial.printf("UDP-Sensordaten werden gesendet an %s:%u\n", piIP.toString().c_str(), UDP_PORT);

  setMotorSpeed(180);
  stopScan();

  delay(1000);

  getHealth();

  delay(1000);

  startScan();

  delay(1000);
}


void loop() {

  // Warten auf WLAN-Verbindung
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WLAN weg, verbinde neu ...");
    WiFi.reconnect();
    delay(2000);
    return;
  }
  // Neuen Client annehmen, falls keiner verbunden ist
  if (!client || !client.connected()) {
    client = server.available();
    if (client) {
      client.setNoDelay(true);
      Serial.println("Client verbunden");
    }
  }   // Jedes ankommende Byte abrufen
  else {
    while (client.available() >= 16) {
      uint8_t buf[16];
      client.readBytes(buf, 16);
      handleValue(buf);
    }
  }
  // LiDAR Daten abfragen
  pollLidar();
}
