#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h>

#define MY_ID 5
#define NODE_COUNT 5
#define MESH_CHANNEL 6

//802.11b
//#define TX_PHY_MODE WIFI_PHY_MODE_11B
//#define TX_PHY_RATE WIFI_PHY_RATE_1M_L
//#define TX_PHY_RATE WIFI_PHY_RATE_5M_L
//#define TX_PHY_RATE WIFI_PHY_RATE_11M_L

//802.11g
//#define TX_PHY_MODE WIFI_PHY_MODE_11G
//#define TX_PHY_RATE WIFI_PHY_RATE_6M
//#define TX_PHY_RATE WIFI_PHY_RATE_24M
//#define TX_PHY_RATE WIFI_PHY_RATE_54M

#define PACKET_COUNT 50
#define PAYLOAD_SIZE 200
#define SEND_INTERVAL_MS 100
#define START_DELAY_MIN_SEC 1
#define START_DELAY_MAX_SEC 30
#define TEST_PAUSE_MIN_SEC 10
#define TEST_PAUSE_MAX_SEC 10
#define RESULT_WAIT_MS 3000
#define DEFAULT_TTL 4

#define PACKET_MAGIC 0x4D45
#define CACHE_SIZE 128
#define HEARTBEAT_INTERVAL_MS 2000   
#define NODE_TIMEOUT_MS 6000         

uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

enum MessageType : uint8_t {
  TEST_START = 1,
  DATA_PACKET = 2,
  HEARTBEAT = 3
};

typedef struct __attribute__((packed)) {
  uint16_t magic;
  uint8_t type;
  uint8_t senderID;
  uint8_t targetID;
  uint8_t testSenderID;
  uint8_t testReceiverID;
  uint32_t messageID;
  uint16_t testID;
  uint16_t packetNumber;
  uint16_t totalPackets;
  uint8_t startDelaySeconds;
  uint8_t ttl;
  uint8_t hopCount;
  char payload[PAYLOAD_SIZE];
} MeshPacket;

struct SeenEntry {
  uint8_t senderID;
  uint32_t messageID;
};

SeenEntry seenCache[CACHE_SIZE] = {};
uint16_t cacheIndex = 0;
uint32_t nextMessageID = 1;
uint16_t nextTestID = 1;
uint32_t lastSeenMs[NODE_COUNT + 1] = {};   
uint32_t nextHeartbeatMs = 0;               
bool automaticMode = false;
bool seriesInitiator = false;
bool testActive = false;
bool senderActive = false;
bool receiverActive = false;
bool senderStarted = false;
bool nextTestScheduled = false;

uint16_t activeTestID = 0;
uint8_t activeSenderID = 0;
uint8_t activeReceiverID = 0;

uint32_t scheduledStartMs = 0;
uint32_t nextPacketMs = 0;
uint32_t testFinishMs = 0;
uint32_t nextTestMs = 0;

uint16_t attemptedPackets = 0;
uint16_t receivedPackets = 0;
uint16_t duplicatePackets = 0;

bool receivedSequence[PACKET_COUNT] = {};
uint32_t firstReceiveMs = 0;
uint32_t lastReceiveMs = 0;
uint32_t hopSum = 0;
uint8_t minHops = 255;
uint8_t maxHops = 0;
uint16_t phyRateCount[32] = {};
uint16_t unknownPhyPackets = 0;

const char *rateToText(uint8_t rate) {
  switch (rate) {
    case WIFI_PHY_RATE_1M_L: return "1 Mbit/s (802.11b)";
    case WIFI_PHY_RATE_2M_L:
    case WIFI_PHY_RATE_2M_S: return "2 Mbit/s (802.11b)";
    case WIFI_PHY_RATE_5M_L:
    case WIFI_PHY_RATE_5M_S: return "5,5 Mbit/s (802.11b)";
    case WIFI_PHY_RATE_11M_L:
    case WIFI_PHY_RATE_11M_S: return "11 Mbit/s (802.11b)";
    case WIFI_PHY_RATE_6M: return "6 Mbit/s (802.11g)";
    case WIFI_PHY_RATE_9M: return "9 Mbit/s (802.11g)";
    case WIFI_PHY_RATE_12M: return "12 Mbit/s (802.11g)";
    case WIFI_PHY_RATE_18M: return "18 Mbit/s (802.11g)";
    case WIFI_PHY_RATE_24M: return "24 Mbit/s (802.11g)";
    case WIFI_PHY_RATE_36M: return "36 Mbit/s (802.11g)";
    case WIFI_PHY_RATE_48M: return "48 Mbit/s (802.11g)";
    case WIFI_PHY_RATE_54M: return "54 Mbit/s (802.11g)";
    default: return "unbekannt";
  }
}

/* aktivieren für datenraten-test
bool setPhyRate() {
  esp_now_rate_config_t config = {};
  config.phymode = TX_PHY_MODE;
  config.rate = TX_PHY_RATE;
  esp_err_t result = esp_now_set_peer_rate_config(broadcastAddress, &config);
  Serial.printf("Eingestellte PHY-Rate: %s | %s\n", rateToText(TX_PHY_RATE), result == ESP_OK ? "ERFOLGREICH" : "FEHLER");
  return result == ESP_OK;
}
*/ 

bool timeReached(uint32_t target) {
  return static_cast<int32_t>(millis() - target) >= 0;
}

bool alreadySeen(uint8_t senderID, uint32_t messageID) {
  for (uint16_t i = 0; i < CACHE_SIZE; i++) {
    if (seenCache[i].senderID == senderID && seenCache[i].messageID == messageID) {
      return true;
    }
  }
  return false;
}

void rememberMessage(uint8_t senderID, uint32_t messageID) {
  seenCache[cacheIndex] = {senderID, messageID};
  cacheIndex = (cacheIndex + 1) % CACHE_SIZE;
}

bool sendPacket(const MeshPacket &packet) {
  return esp_now_send(broadcastAddress, reinterpret_cast<const uint8_t *>(&packet), sizeof(packet)) == ESP_OK;
}

void sendHeartbeat() {
  MeshPacket packet = {};
  packet.magic = PACKET_MAGIC;
  packet.type = HEARTBEAT;
  packet.senderID = MY_ID;
  packet.targetID = 0;
  packet.messageID = nextMessageID++;
  packet.ttl = DEFAULT_TTL;
  packet.hopCount = 1;
  rememberMessage(packet.senderID, packet.messageID);
  sendPacket(packet);
}

void prepareTest(uint16_t testID, uint8_t senderID, uint8_t receiverID, uint8_t delaySeconds) {
  activeTestID = testID;
  activeSenderID = senderID;
  activeReceiverID = receiverID;
  senderActive = MY_ID == senderID;
  receiverActive = MY_ID == receiverID;
  senderStarted = false;
  testActive = true;

  attemptedPackets = 0;
  receivedPackets = 0;
  duplicatePackets = 0;
  firstReceiveMs = 0;
  lastReceiveMs = 0;
  hopSum = 0;
  minHops = 255;
  maxHops = 0;
  unknownPhyPackets = 0;
  memset(receivedSequence, 0, sizeof(receivedSequence));
  memset(phyRateCount, 0, sizeof(phyRateCount));

  scheduledStartMs = millis() + static_cast<uint32_t>(delaySeconds) * 1000UL;
  testFinishMs = scheduledStartMs + static_cast<uint32_t>(PACKET_COUNT - 1) * SEND_INTERVAL_MS + RESULT_WAIT_MS;

  Serial.println();
  Serial.println("========== NEUER TEST ==========");
  Serial.printf("Test-ID: %u | Sender: %u | Empfaenger: %u\n", activeTestID, activeSenderID, activeReceiverID);
  Serial.printf("Start in: %u %s\n", delaySeconds, delaySeconds == 1 ? "Sekunde" : "Sekunden");

  if (senderActive) Serial.println("Rolle: Sender");
  else if (receiverActive) Serial.println("Rolle: Empfaenger");
  else Serial.println("Rolle: Weiterleitung");
}

void createRandomTest() {
  uint8_t sender = random(1, NODE_COUNT + 1);
  uint8_t receiver;
  do {
    receiver = random(1, NODE_COUNT + 1);
  } while (receiver == sender);

  uint8_t delaySeconds = random(START_DELAY_MIN_SEC, START_DELAY_MAX_SEC + 1);
  uint16_t testID = nextTestID++;

  MeshPacket packet = {};
  packet.magic = PACKET_MAGIC;
  packet.type = TEST_START;
  packet.senderID = MY_ID;
  packet.targetID = 0;
  packet.testSenderID = sender;
  packet.testReceiverID = receiver;
  packet.messageID = nextMessageID++;
  packet.testID = testID;
  packet.totalPackets = PACKET_COUNT;
  packet.startDelaySeconds = delaySeconds;
  packet.ttl = DEFAULT_TTL;
  packet.hopCount = 1;

  rememberMessage(packet.senderID, packet.messageID);
  prepareTest(testID, sender, receiver, delaySeconds);

  for (uint8_t i = 0; i < 3; i++) {
    sendPacket(packet);
    delay(20);
  }
}

void processDataPacket(const MeshPacket &packet, const wifi_pkt_rx_ctrl_t *rxCtrl) {
  if (!receiverActive || packet.testID != activeTestID || packet.packetNumber >= PACKET_COUNT) {
    return;
  }

  if (receivedSequence[packet.packetNumber]) {
    duplicatePackets++;
    return;
  }

  receivedSequence[packet.packetNumber] = true;
  receivedPackets++;

  uint32_t now = millis();
  if (receivedPackets == 1) firstReceiveMs = now;
  lastReceiveMs = now;

  hopSum += packet.hopCount;
  if (packet.hopCount < minHops) minHops = packet.hopCount;
  if (packet.hopCount > maxHops) maxHops = packet.hopCount;

  const char *phyText = "nicht erfasst";
  if (rxCtrl != nullptr && rxCtrl->sig_mode == 0 && rxCtrl->rate < 32) {
    phyRateCount[rxCtrl->rate]++;
    phyText = rateToText(rxCtrl->rate);
  } else {
    unknownPhyPackets++;
    if (rxCtrl != nullptr && rxCtrl->sig_mode == 1) phyText = "802.11n";
  }
  Serial.printf("Paket %u/%u empfangen | Hops: %u | PHY: %s\n", packet.packetNumber + 1, PACKET_COUNT, packet.hopCount, phyText);
}

void onDataReceived(const esp_now_recv_info_t *recvInfo, const uint8_t *data, int len) {
  if (len != sizeof(MeshPacket)) return;

  MeshPacket incoming;
  memcpy(&incoming, data, sizeof(incoming));

  if (incoming.magic != PACKET_MAGIC) return;
  if (incoming.type != TEST_START && incoming.type != DATA_PACKET && incoming.type != HEARTBEAT) return;
  if (alreadySeen(incoming.senderID, incoming.messageID)) {
    if (incoming.type == DATA_PACKET && receiverActive && incoming.testID == activeTestID && incoming.targetID == MY_ID) {
      duplicatePackets++;
    }
    return;
  }

  rememberMessage(incoming.senderID, incoming.messageID);
  if (incoming.senderID <= NODE_COUNT) {
    lastSeenMs[incoming.senderID] = millis();
  }
  if (incoming.type == TEST_START) {
    if (testActive && incoming.testID != activeTestID) return;

    prepareTest(incoming.testID, incoming.testSenderID, incoming.testReceiverID, incoming.startDelaySeconds);
  }

  if (incoming.type == DATA_PACKET && incoming.targetID == MY_ID) {
    processDataPacket(incoming, recvInfo != nullptr ? recvInfo->rx_ctrl : nullptr);
    return;
  }

  if (incoming.ttl == 0) return;

  incoming.ttl--;
  incoming.hopCount++;

  if (!senderActive && !receiverActive && incoming.type != HEARTBEAT) {  
    if (incoming.type == TEST_START) {
      Serial.printf("TEST_START wird weitergeleitet | Test-ID: %u\n", incoming.testID);
    } else {
      Serial.printf("Paket %u/%u wird weitergeleitet.\n", incoming.packetNumber + 1, PACKET_COUNT);
    }
  }
  sendPacket(incoming);
}

void sendMeasurementPacket() {
  MeshPacket packet = {};
  packet.magic = PACKET_MAGIC;
  packet.type = DATA_PACKET;
  packet.senderID = MY_ID;
  packet.targetID = activeReceiverID;
  packet.testSenderID = activeSenderID;
  packet.testReceiverID = activeReceiverID;
  packet.messageID = nextMessageID++;
  packet.testID = activeTestID;
  packet.packetNumber = attemptedPackets;
  packet.totalPackets = PACKET_COUNT;
  packet.ttl = DEFAULT_TTL;
  packet.hopCount = 1;
  memset(packet.payload, 'A', sizeof(packet.payload));

  rememberMessage(packet.senderID, packet.messageID);
  bool queued = sendPacket(packet);
  attemptedPackets++;
  Serial.printf("Paket %u/%u gesendet: %s\n", attemptedPackets, PACKET_COUNT, queued ? "OK" : "FEHLER");
}

void printReceiverResult() {
  uint16_t lostPackets = PACKET_COUNT - receivedPackets;
  float pdr = receivedPackets * 100.0f / PACKET_COUNT;
  uint32_t durationMs = receivedPackets > 1 ? lastReceiveMs - firstReceiveMs : 0;
  float goodputKbit = durationMs > 0 ? (receivedPackets * PAYLOAD_SIZE * 8.0f) / durationMs : 0.0f;
  float averageHops = receivedPackets > 0 ? static_cast<float>(hopSum) / receivedPackets : 0.0f;

  Serial.println();
  Serial.println("========== TESTERGEBNIS ==========");
  Serial.printf("Test-ID: %u\n", activeTestID);
  Serial.printf("Sender: %u | Empfaenger: %u\n", activeSenderID, activeReceiverID);
  Serial.printf("Gesendete Pakete: %u\n", PACKET_COUNT);
  Serial.printf("Empfangene Pakete: %u\n", receivedPackets);
  Serial.printf("Verlorene Pakete: %u\n", lostPackets);
  Serial.printf("PDR: %.2f %%\n", pdr);
  Serial.printf("Empfangsdauer: %.3f Sekunden\n", durationMs / 1000.0f);
  Serial.printf("Goodput: %.2f kbit/s\n", goodputKbit);
  Serial.printf("Erkannte Duplikate: %u\n", duplicatePackets);

  if (receivedPackets > 0) {
    Serial.printf("Hops min/max/mittel: %u / %u / %.2f\n", minHops, maxHops, averageHops);
  }

  Serial.println("PHY-Raten:");
  for (uint8_t rate = 0; rate < 32; rate++) {
    if (phyRateCount[rate] > 0) {
      Serial.printf("%s: %u Pakete\n", rateToText(rate), phyRateCount[rate]);
    }
  }
  if (unknownPhyPackets > 0) {
    Serial.printf("Nicht erfasst/802.11n: %u Pakete\n", unknownPhyPackets);
  }
  Serial.println("==================================");
}

void finishTest() {
  if (receiverActive) printReceiverResult();
  testActive = false;
  senderActive = false;
  receiverActive = false;
  senderStarted = false;
  if (seriesInitiator && automaticMode) {
    uint8_t pauseSeconds = random(TEST_PAUSE_MIN_SEC, TEST_PAUSE_MAX_SEC + 1);
    nextTestMs = millis() + static_cast<uint32_t>(pauseSeconds) * 1000UL;
    nextTestScheduled = true;
    Serial.printf("Naechster Test in %u %s.\n", pauseSeconds, pauseSeconds == 1 ? "Sekunde" : "Sekunden");
  }
}

void onDataSent(const wifi_tx_info_t *, esp_now_send_status_t status) {
  if (status != ESP_NOW_SEND_SUCCESS && senderActive) {
    Serial.println("ESP-NOW-Uebertragung fehlgeschlagen.");
  }
}

void printActiveNodes() {
  uint32_t now = millis();
  Serial.println("Aktive Knoten:");
  for (uint8_t id = 1; id <= NODE_COUNT; id++) {
    if (lastSeenMs[id] != 0 && now - lastSeenMs[id] <= NODE_TIMEOUT_MS) {
      Serial.printf("ID %u aktiv (zuletzt vor %lu ms)\n", id, now - lastSeenMs[id]);
    }
  }
}

void handleSerialInput() {
  if (!Serial.available()) return;

  String command = Serial.readStringUntil('\n');
  command.trim();
  command.toLowerCase();

  if (command == "start") {
    if (automaticMode) {
      Serial.println("Automatische Messreihe laeuft bereits.");
      return;
    }

    automaticMode = true;
    seriesInitiator = true;
    nextTestScheduled = false;
    Serial.println("Automatische Messreihe gestartet.");

    if (!testActive) createRandomTest();
  } else if (command == "stop") {
      automaticMode = false;
      seriesInitiator = false;
      nextTestScheduled = false;
      Serial.println("Automatik beendet; laufender Test wird abgeschlossen.");
  } else if (command == "who") {          
      printActiveNodes();                   
  }
}

void setup() {
  Serial.begin(115200);
  Serial.setTimeout(50);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  randomSeed(esp_random());

  if (esp_wifi_set_channel(MESH_CHANNEL, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
    Serial.println("WLAN-Kanal konnte nicht gesetzt werden.");
    return;
  }

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW-Initialisierung fehlgeschlagen.");
    return;
  }

  esp_now_register_send_cb(onDataSent);
  esp_now_register_recv_cb(onDataReceived);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, broadcastAddress, sizeof(broadcastAddress));
  peer.channel = MESH_CHANNEL;
  peer.encrypt = false;

  esp_err_t result = esp_now_add_peer(&peer);
  if (result != ESP_OK && result != ESP_ERR_ESPNOW_EXIST) {
    Serial.println("Broadcast-Peer konnte nicht hinzugefuegt werden.");
    return;
  }

  /* aktivieren für datenrate-test
  if (!setPhyRate()) {
    Serial.println("PHY-Rate konnte nicht gesetzt werden.");
    return;
  }
  */

  Serial.printf("ESP-NOW-Flooding-Mesh gestartet | ID: %u | Kanal: %u\n", MY_ID, MESH_CHANNEL);
  Serial.print("Zuordnung: ID ");
  Serial.print(MY_ID);
  Serial.print(" = ");
  Serial.println(WiFi.macAddress());
  Serial.println("'start' = starten | 'stop' = beenden | 'who' = aktive Knoten");
}

void loop() {
  handleSerialInput();
  
  if (!testActive && timeReached(nextHeartbeatMs)) {
    sendHeartbeat();
    lastSeenMs[MY_ID] = millis();
    nextHeartbeatMs = millis() + HEARTBEAT_INTERVAL_MS;
  }

  if (testActive && senderActive && !senderStarted && timeReached(scheduledStartMs)) {
    senderStarted = true;
    nextPacketMs = millis();
    Serial.println("Sendevorgang beginnt.");
  }

  if (testActive && senderActive && senderStarted && attemptedPackets < PACKET_COUNT && timeReached(nextPacketMs)) {
    sendMeasurementPacket();
    nextPacketMs += SEND_INTERVAL_MS;
  }

  if (testActive && timeReached(testFinishMs)) {
    finishTest();
  }

  if (seriesInitiator && automaticMode && !testActive && nextTestScheduled && timeReached(nextTestMs)) {
    nextTestScheduled = false;
    createRandomTest();
  }
}