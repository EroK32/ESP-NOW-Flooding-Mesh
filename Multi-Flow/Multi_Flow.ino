#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h>

#define MY_ID 5
#define NODE_COUNT 5
#define INITIATOR_ID 5
#define MAX_FLOWS 4
#define MESH_CHANNEL 6

#define PACKET_COUNT 50
#define PAYLOAD_SIZE 200
#define SEND_INTERVAL_MS 100
#define PREPARE_DELAY_MS 5000UL
#define DATA_START_GUARD_MS 100UL
#define RESULT_WAIT_MS 3000
#define TEST_PAUSE_MIN_SEC 5
#define TEST_PAUSE_MAX_SEC 30
#define DEFAULT_TTL 4

#define PACKET_MAGIC 0x4D45
#define CACHE_SIZE 256
#define HEARTBEAT_INTERVAL_MS 2000   
#define NODE_TIMEOUT_MS 6000         

uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

enum MessageType : uint8_t {
  PARALLEL_PREPARE = 1,
  DATA_PACKET = 2,
  RESULT_PACKET = 3,
  HEARTBEAT = 4,
  SENDER_SUMMARY = 5,
  TEST_GO = 6
};

typedef struct __attribute__((packed)) {
  uint16_t magic;
  uint8_t type;
  uint8_t senderID;
  uint8_t targetID;
  uint8_t flowID;
  uint8_t activeFlows;
  uint8_t flowSender[MAX_FLOWS];
  uint8_t flowReceiver[MAX_FLOWS];
  uint32_t messageID;
  uint16_t testID;
  uint16_t packetNumber;
  uint8_t ttl;
  uint8_t hopCount;
  char payload[PAYLOAD_SIZE];
} MeshPacket;

struct SeenEntry {
  uint8_t senderID;
  uint32_t messageID;
};

struct FlowState {
  uint8_t senderID;
  uint8_t receiverID;
  uint16_t sent;
  uint16_t received;
  uint16_t duplicates;
  uint32_t firstRxMs;
  uint32_t lastRxMs;
  uint32_t hopSum;
  uint8_t minHops;
  uint8_t maxHops;
  bool senderStarted;
  bool resultSent;
  bool senderSummarySent;
  uint32_t txStartMs;
  uint32_t txEndMs;
  uint32_t nextPacketMs;
  bool receivedSequence[PACKET_COUNT];
  uint16_t rxPacketTime[PACKET_COUNT];
};

struct ResultData {
  uint8_t senderID;
  uint8_t receiverID;
  uint16_t received;
  uint16_t duplicates;
  uint32_t durationMs;
  uint32_t rxStartMs;
  uint32_t rxEndMs;
  uint32_t hopSum;
  uint8_t minHops;
  uint8_t maxHops;
  uint16_t rxPacketTime[PACKET_COUNT];
};

struct SenderSummaryData {
  uint8_t senderID;
  uint8_t receiverID;
  uint16_t sent;
  uint32_t txStartMs;
  uint32_t txEndMs;
};

SeenEntry seenCache[CACHE_SIZE] = {};
uint16_t cacheIndex = 0;
uint32_t nextMessageID = 1;
uint8_t activeFlowCount = 2;
FlowState flows[MAX_FLOWS] = {};

uint32_t lastSeenMs[NODE_COUNT + 1] = {};
uint32_t nextHeartbeatMs = 0;
bool testActive = false;
uint16_t activeTestID = 0;
bool automaticMode = false;
bool nextTestScheduled = false;
uint32_t nextTestMs = 0;

uint32_t prepareStartMs = 0;
uint32_t testZeroMs = 0;
uint32_t dataStartMs = 0;
uint32_t testFinishMs = 0;

bool goSent = false;
bool goReceived = false;

bool summaryReceived[MAX_FLOWS] = {};
ResultData results[MAX_FLOWS] = {};
bool senderSummaryReceived[MAX_FLOWS] = {};
SenderSummaryData senderResults[MAX_FLOWS] = {};
uint32_t resultDeadlineMs = 0;
bool resultReady = false;

bool timeReached(uint32_t target) {
  return static_cast<int32_t>(millis() - target) >= 0;
}

bool isNewerTest(uint16_t newID, uint16_t oldID) {
  return (int16_t)(newID - oldID) > 0;
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
  return esp_now_send(broadcastAddress, (const uint8_t *)&packet, sizeof(packet)) == ESP_OK;
}

void sendRepeatedControlPacket(MeshPacket packet, uint8_t repetitions, uint16_t gapMs) {
  for (uint8_t i = 0; i < repetitions; i++) {
    packet.messageID = nextMessageID++;
    rememberMessage(packet.senderID, packet.messageID);
    sendPacket(packet);
    if (i + 1 < repetitions) delay(gapMs);
  }
}

void sendHeartbeat() {
  MeshPacket packet = {};
  packet.magic = PACKET_MAGIC;
  packet.type = HEARTBEAT;
  packet.senderID = MY_ID;
  packet.messageID = nextMessageID++;
  packet.ttl = DEFAULT_TTL;
  packet.hopCount = 1;
  rememberMessage(packet.senderID, packet.messageID);
  sendPacket(packet);
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

void resetParallelTest(uint16_t testID, const uint8_t *senders, const uint8_t *receivers) {
  activeTestID = testID;
  testActive = true;
  resultReady = false;
  goReceived = false;
  goSent = false;
  testZeroMs = 0;
  dataStartMs = 0;
  testFinishMs = 0;
  resultDeadlineMs = 0;
  memset(senderSummaryReceived, 0, sizeof(senderSummaryReceived));
  memset(senderResults, 0, sizeof(senderResults));
  memset(flows, 0, sizeof(flows));
  memset(summaryReceived, 0, sizeof(summaryReceived));
  memset(results, 0, sizeof(results));

  for (uint8_t flow = 0; flow < activeFlowCount; flow++) {
    flows[flow].senderID = senders[flow];
    flows[flow].receiverID = receivers[flow];
    flows[flow].minHops = 255;
  }

  Serial.println();
  Serial.println("========== MULTI-FLOW-TEST ==========");
  Serial.printf("Aktive Flows: %u\n", activeFlowCount);
  Serial.printf("Test-ID: %u\n", activeTestID);

  for (uint8_t flow = 0; flow < activeFlowCount; flow++) {
    Serial.printf("Flow %c: %u -> %u\n", 'A' + flow, flows[flow].senderID, flows[flow].receiverID);
  }
  Serial.println("Test vorbereitet. Warte auf GO.");
}

void activateGo() {
  if (!testActive || goReceived) return;
  testZeroMs = millis();
  dataStartMs = testZeroMs + DATA_START_GUARD_MS;
  testFinishMs = dataStartMs + (PACKET_COUNT - 1) * SEND_INTERVAL_MS + RESULT_WAIT_MS;
  resultDeadlineMs = testFinishMs + 3000;
  goReceived = true;

  Serial.println();
  Serial.printf("GO empfangen | Test %u | Testzeit = 0 ms\n", activeTestID);
  Serial.printf("DATA-Start in %u ms\n", DATA_START_GUARD_MS);
}

void sendGoPacket() {
  if (MY_ID != INITIATOR_ID || !testActive || goSent) return;
  goSent = true;
  MeshPacket packet = {};
  packet.magic = PACKET_MAGIC;
  packet.type = TEST_GO;
  packet.senderID = MY_ID;
  packet.testID = activeTestID;
  packet.ttl = DEFAULT_TTL;
  packet.hopCount = 1;
  activateGo();
  sendRepeatedControlPacket(packet, 3, 20);
}

void shuffleNodes(uint8_t *nodes) {
  for (int8_t i = 3; i > 0; i--) {
    uint8_t j = random(0, i + 1);
    uint8_t temp = nodes[i];
    nodes[i] = nodes[j];
    nodes[j] = temp;
  }
}

bool validFlowAssignment(const uint8_t *senders, const uint8_t *receivers) {
  for (uint8_t i = 0; i < activeFlowCount; i++)
    if (senders[i] == receivers[i]) return false;
  return true;
}

void startParallelTest() {
  if (MY_ID != INITIATOR_ID || testActive) return;
  uint8_t senders[MAX_FLOWS] = {1, 2, 3, 4};
  uint8_t receivers[MAX_FLOWS] = {1, 2, 3, 4};
  shuffleNodes(senders);

  do {
    shuffleNodes(receivers);
  } while (!validFlowAssignment(senders, receivers));

  uint16_t testID = activeTestID + 1;
  MeshPacket packet = {};
  packet.magic = PACKET_MAGIC;
  packet.type = PARALLEL_PREPARE;
  packet.senderID = MY_ID;
  packet.testID = testID;
  packet.activeFlows = activeFlowCount;
  packet.ttl = DEFAULT_TTL;
  packet.hopCount = 1;
  memcpy(packet.flowSender, senders, MAX_FLOWS);
  memcpy(packet.flowReceiver, receivers, MAX_FLOWS);
  resetParallelTest(testID, senders, receivers);
  prepareStartMs = millis();
  sendRepeatedControlPacket(packet, 3, 20);
}

void scheduleNextTest() {
  if (!automaticMode || MY_ID != INITIATOR_ID) return;
  uint8_t pauseSeconds = random(TEST_PAUSE_MIN_SEC, TEST_PAUSE_MAX_SEC + 1);
  nextTestMs = millis() + (uint32_t)pauseSeconds * 1000;
  nextTestScheduled = true;
  Serial.printf("Naechster Test in %u Sekunden.\n", pauseSeconds);
}

void sendMeasurementPacket(uint8_t flowID) {
  FlowState &flow = flows[flowID];
  MeshPacket packet = {};
  packet.magic = PACKET_MAGIC;
  packet.type = DATA_PACKET;
  packet.senderID = MY_ID;
  packet.targetID = flow.receiverID;
  packet.flowID = flowID;
  packet.messageID = nextMessageID++;
  packet.testID = activeTestID;
  packet.packetNumber = flow.sent;
  packet.ttl = DEFAULT_TTL;
  packet.hopCount = 1;
  memset(packet.payload, 'A', sizeof(packet.payload));
  rememberMessage(packet.senderID, packet.messageID);
  sendPacket(packet);
  flow.sent++;
}

void processReceiverPacket(const MeshPacket &packet) {
  if (!testActive || packet.testID != activeTestID || packet.flowID >= activeFlowCount || packet.packetNumber >= PACKET_COUNT) return;
  FlowState &flow = flows[packet.flowID];
  if (flow.receiverID != MY_ID) return;
  if (flow.receivedSequence[packet.packetNumber]) {
    flow.duplicates++;
    return;
  }
  flow.receivedSequence[packet.packetNumber] = true;
  flow.received++;
  uint32_t now = millis() - testZeroMs;
  flow.rxPacketTime[packet.packetNumber] = (uint16_t)now;
  if (flow.received == 1) flow.firstRxMs = now;
  flow.lastRxMs = now;
  flow.hopSum += packet.hopCount;
  if (packet.hopCount < flow.minHops) flow.minHops = packet.hopCount;
  if (packet.hopCount > flow.maxHops) flow.maxHops = packet.hopCount;
}

void sendSenderSummaryPacket(uint8_t flowID) {
  FlowState &flow = flows[flowID];
  SenderSummaryData summary = {flow.senderID, flow.receiverID, flow.sent, flow.txStartMs, flow.txEndMs};
  MeshPacket packet = {};
  packet.magic = PACKET_MAGIC;
  packet.type = SENDER_SUMMARY;
  packet.senderID = MY_ID;
  packet.targetID = INITIATOR_ID;
  packet.flowID = flowID;
  packet.testID = activeTestID;
  packet.ttl = DEFAULT_TTL;
  packet.hopCount = 1;
  memcpy(packet.payload, &summary, sizeof(summary));
  sendRepeatedControlPacket(packet, 3, 20);
}

void sendResultPacket(uint8_t flowID) {
  FlowState &flow = flows[flowID];
  ResultData result = {};
  result.senderID = flow.senderID;
  result.receiverID = flow.receiverID;
  result.received = flow.received;
  result.duplicates = flow.duplicates;
  result.durationMs = flow.received > 1 ? flow.lastRxMs - flow.firstRxMs : 0;
  result.rxStartMs = flow.received ? flow.firstRxMs : 0;
  result.rxEndMs = flow.received ? flow.lastRxMs : 0;
  result.hopSum = flow.hopSum;
  result.minHops = flow.received ? flow.minHops : 0;
  result.maxHops = flow.maxHops;

  memcpy(result.rxPacketTime, flow.rxPacketTime, sizeof(flow.rxPacketTime));
  MeshPacket packet = {};
  packet.magic = PACKET_MAGIC;
  packet.type = RESULT_PACKET;
  packet.senderID = MY_ID;
  packet.targetID = INITIATOR_ID;
  packet.flowID = flowID;
  packet.testID = activeTestID;
  packet.ttl = DEFAULT_TTL;
  packet.hopCount = 1;
  memcpy(packet.payload, &result, sizeof(result));
  sendRepeatedControlPacket(packet, 3, 20);
}

bool allResultsReceived() {
  for (uint8_t i = 0; i < activeFlowCount; i++)
    if (!summaryReceived[i] || !senderSummaryReceived[i]) return false;
    return true;
}

void printParallelResult() {
  Serial.println();
  Serial.println("========== ERGEBNIS ==========");
  for (uint8_t flow = 0; flow < activeFlowCount; flow++) {
    ResultData &result = results[flow];
    SenderSummaryData &sender = senderResults[flow];
    float pdr = result.received * 100.0f / PACKET_COUNT;
    float goodput = result.durationMs > 0 ? (result.received * PAYLOAD_SIZE * 8.0f) / result.durationMs : 0.0f;
    float averageHops = result.received > 0 ? static_cast<float>(result.hopSum) / result.received : 0.0f;
    Serial.printf("\nFlow %c: %u -> %u\n", 'A' + flow, result.senderID, result.receiverID);
    Serial.printf("TX: Start %lu ms | Ende %lu ms\n", static_cast<unsigned long>(sender.txStartMs), 
    static_cast<unsigned long>(sender.txEndMs));
    Serial.printf("RX: Start %lu ms | Ende %lu ms\n", static_cast<unsigned long>(result.rxStartMs), 
    static_cast<unsigned long>(result.rxEndMs));
    Serial.printf("Empfangen: %u/%u\n", result.received, PACKET_COUNT);
    Serial.printf("PDR: %.2f %%\n", pdr);
    Serial.printf("Dauer: %.3f Sekunden\n", result.durationMs / 1000.0f);
    Serial.printf("Goodput: %.2f kbit/s\n", goodput);
    Serial.printf("Duplikate: %u\n", result.duplicates);
    Serial.printf("Hops min/max/mittel: %u / %u / %.2f\n", result.minHops, result.maxHops, averageHops);
  }
    uint32_t latestTxStart = senderResults[0].txStartMs;
    uint32_t earliestTxEnd = senderResults[0].txEndMs;
    uint32_t latestRxStart = results[0].rxStartMs;
    uint32_t earliestRxEnd = results[0].rxEndMs;

  for (uint8_t flow = 1; flow < activeFlowCount; flow++) {
    latestTxStart = max(latestTxStart, senderResults[flow].txStartMs);
    earliestTxEnd = min(earliestTxEnd, senderResults[flow].txEndMs);
    latestRxStart = max(latestRxStart, results[flow].rxStartMs);
    earliestRxEnd = min(earliestRxEnd, results[flow].rxEndMs);
  }

  uint32_t txOverlapMs = earliestTxEnd > latestTxStart ? earliestTxEnd - latestTxStart : 0;
  uint32_t parallelWindowMs = earliestRxEnd > latestRxStart ? earliestRxEnd - latestRxStart : 0;
  Serial.println();
  Serial.printf("Spaetester TX-Start: %lu ms\n", (unsigned long)latestTxStart);
  Serial.printf("Spaetester RX-Start: %lu ms\n", (unsigned long)latestRxStart);
  Serial.printf("Fruehestes TX-Ende: %lu ms\n", (unsigned long)earliestTxEnd);
  Serial.printf("Fruehestes RX-Ende: %lu ms\n", (unsigned long)earliestRxEnd);
  Serial.printf("TX-Ueberlappung: %.3f Sekunden\n", txOverlapMs / 1000.0f);
  Serial.printf("Parallelfenster: %.3f Sekunden\n", parallelWindowMs / 1000.0f);
  Serial.printf("Alle Flows parallel: %s\n", parallelWindowMs > 0 ? "JA" : "NEIN");
  uint16_t packetsInParallelWindow = 0;

  if (parallelWindowMs > 0) {
    for (uint8_t flow = 0; flow < activeFlowCount; flow++) {
      for (uint16_t packet = 0; packet < PACKET_COUNT; packet++) {
        uint16_t rxTime = results[flow].rxPacketTime[packet];
        if (rxTime >= latestRxStart && rxTime <= earliestRxEnd) {
          packetsInParallelWindow++;
        }
      }
    }
  }

  float cellRate = parallelWindowMs > 0 ? (packetsInParallelWindow * PAYLOAD_SIZE * 8.0f) / parallelWindowMs : 0.0f;
  Serial.printf("Pakete im Parallelfenster: %u\n", packetsInParallelWindow);
  Serial.printf("Cell Rate: %.2f kbit/s\n", cellRate);
  Serial.println("===============================");
  testActive = false;
  scheduleNextTest();
}

void onDataReceived(const esp_now_recv_info_t *recvInfo, const uint8_t *data, int len) {
  if (len != sizeof(MeshPacket)) return;
  MeshPacket incoming;
  memcpy(&incoming, data, sizeof(incoming));
  if (incoming.magic != PACKET_MAGIC) return;
  if (incoming.type != PARALLEL_PREPARE && incoming.type != DATA_PACKET && incoming.type != RESULT_PACKET &&
  incoming.type != HEARTBEAT && incoming.type != SENDER_SUMMARY && incoming.type != TEST_GO) {
    return;
  }

  if (alreadySeen(incoming.senderID, incoming.messageID)) {
    if (incoming.type == DATA_PACKET && incoming.testID == activeTestID && 
    incoming.flowID < activeFlowCount && incoming.targetID == MY_ID) {
      flows[incoming.flowID].duplicates++;
    }
    return;
  }
  rememberMessage(incoming.senderID, incoming.messageID);

  if (incoming.senderID <= NODE_COUNT) {
    lastSeenMs[incoming.senderID] = millis();
  }

  if (incoming.type == PARALLEL_PREPARE) {
    Serial.printf("PREPARE empfangen | Test %u | lokal aktiv: %s | aktuelle Test-ID: %u\n", incoming.testID, testActive ? "JA" : "NEIN", activeTestID);
    bool validFlowCount = incoming.activeFlows >= 2 && incoming.activeFlows <= MAX_FLOWS;
    if (!validFlowCount) {
      return;
    }

    if (incoming.testID != activeTestID) {
      if (activeTestID == 0 || isNewerTest(incoming.testID, activeTestID)) {
        if (testActive) {
          Serial.printf("Alter Test %u wird durch neuen Test %u ersetzt.\n", activeTestID, incoming.testID);
        }
        activeFlowCount = incoming.activeFlows;
        resetParallelTest(incoming.testID, incoming.flowSender, incoming.flowReceiver);
        Serial.printf("Test %u lokal eingerichtet.\n", incoming.testID);
      }
    } else {
      Serial.printf("PREPARE-Wiederholung fuer Test %u empfangen.\n", incoming.testID);
    }
  }
  if (incoming.type == TEST_GO) {
    if (incoming.testID != activeTestID) {
      return;
    }
    activateGo();

    if (incoming.ttl > 0) {
      incoming.ttl--;
      incoming.hopCount++;
      sendPacket(incoming);
    }
    return;
  }

  if (incoming.type == DATA_PACKET && incoming.targetID == MY_ID) {
    if (!goReceived) {
      return;
    }
    processReceiverPacket(incoming);
    return;
  }

  if (incoming.type == RESULT_PACKET && MY_ID == INITIATOR_ID && incoming.targetID == INITIATOR_ID &&
  incoming.testID == activeTestID &&incoming.flowID < activeFlowCount) {
    if (summaryReceived[incoming.flowID]) {
      return;
    }
    memcpy(&results[incoming.flowID], incoming.payload, sizeof(ResultData));
    summaryReceived[incoming.flowID] = true;
    if (allResultsReceived()) {
      resultReady = true;
    }
    return;
  }

  if (incoming.type == SENDER_SUMMARY && MY_ID == INITIATOR_ID && incoming.targetID == INITIATOR_ID &&
    incoming.testID == activeTestID && incoming.flowID < activeFlowCount) {
    if (senderSummaryReceived[incoming.flowID]) {
      return;
    }

    memcpy(&senderResults[incoming.flowID], incoming.payload, sizeof(SenderSummaryData));
    senderSummaryReceived[incoming.flowID] = true;
    if (allResultsReceived()) {
    resultReady = true;
    }
    return;
  }

  if (incoming.ttl == 0) return;

  incoming.ttl--;
  incoming.hopCount++;

  if (incoming.type == DATA_PACKET || incoming.type == PARALLEL_PREPARE || incoming.type == RESULT_PACKET || 
  incoming.type == HEARTBEAT || incoming.type == SENDER_SUMMARY) {
    sendPacket(incoming);
  }
}

void onDataSent(const wifi_tx_info_t *, esp_now_send_status_t status) {
  if (status != ESP_NOW_SEND_SUCCESS && testActive) {
    Serial.println("ESP-NOW-Uebertragung fehlgeschlagen.");
  }
}

void handleSerialInput() {
  if (!Serial.available()) return;

  String command = Serial.readStringUntil('\n');
  command.trim();
  command.toLowerCase();

  if (command.startsWith("start ")) {
    if (MY_ID != INITIATOR_ID) {
      Serial.println("Start nur bei Knoten 5.");
      return;
    }

    if (testActive) {
      Serial.println("Test laeuft bereits.");
      return;
    }

    uint8_t requestedFlows = command.substring(6).toInt();

    if (requestedFlows < 2 || requestedFlows > MAX_FLOWS) {
      Serial.println("Erlaubt: start 2, start 3 oder start 4");
      return;
    }

    activeFlowCount = requestedFlows;

    automaticMode = true;
    nextTestScheduled = false;

    Serial.printf("Automatische %u-Flow-Messreihe gestartet.\n", activeFlowCount);

    startParallelTest();

  } else if (command == "stop") {
    automaticMode = false;
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

  Serial.printf("ESP-NOW-Flooding-Mesh gestartet | ID: %u | Kanal: %u\n", MY_ID, MESH_CHANNEL);
  Serial.print("Zuordnung: ID ");
  Serial.print(MY_ID);
  Serial.print(" = ");
  Serial.println(WiFi.macAddress());
  Serial.println("'start 2', 'start 3', 'start 4' | 'stop' | 'who'");
}

void loop() {
  handleSerialInput();

  if (!testActive && timeReached(nextHeartbeatMs)) {
    sendHeartbeat();
    lastSeenMs[MY_ID] = millis();
    nextHeartbeatMs = millis() + HEARTBEAT_INTERVAL_MS;
  }
  
  if (MY_ID == INITIATOR_ID && testActive && !goSent && timeReached(prepareStartMs + PREPARE_DELAY_MS)) {
    sendGoPacket();
  }

  if (testActive) {
    for (uint8_t flow = 0; flow < activeFlowCount; flow++) {
      FlowState &state = flows[flow];

      if (goReceived && state.senderID == MY_ID && !state.senderStarted && timeReached(dataStartMs)) {
        state.senderStarted = true;

        state.txStartMs =
        millis() - testZeroMs;
        state.nextPacketMs =
        millis();
      }

      if (state.senderID == MY_ID && state.senderStarted && state.sent < PACKET_COUNT && timeReached(state.nextPacketMs)) {
        sendMeasurementPacket(flow);
        state.nextPacketMs += SEND_INTERVAL_MS;
      }

      if (state.senderID == MY_ID && state.sent >= PACKET_COUNT && !state.senderSummarySent) {
        state.txEndMs = millis() - testZeroMs;
        state.senderSummarySent = true;
        sendSenderSummaryPacket(flow);
      }

      if (goReceived && state.receiverID == MY_ID && !state.resultSent && timeReached(testFinishMs + flow * 250UL)) {
        state.resultSent = true;
        sendResultPacket(flow);
      }
    }
    bool localFinished = true;

    for (uint8_t flow = 0; flow < activeFlowCount; flow++) {
      if (flows[flow].senderID == MY_ID && flows[flow].sent < PACKET_COUNT) {
        localFinished = false;
      }

      if (flows[flow].receiverID == MY_ID && !flows[flow].resultSent) {
        localFinished = false;
      }
    }

    if (MY_ID != INITIATOR_ID && goReceived && localFinished && timeReached(testFinishMs + (activeFlowCount - 1) * 250UL + 1000UL)) {
      testActive = false;
    }
  }

  if (MY_ID == INITIATOR_ID && testActive && goReceived && timeReached(resultDeadlineMs) && !allResultsReceived()) {
    Serial.println();
    Serial.println("========== TEST UNGUELTIG ==========");
    Serial.printf("Test-ID: %u\n", activeTestID);

    for (uint8_t flow = 0; flow < activeFlowCount; flow++) {
      Serial.printf("Flow %c | TX-Summary: %s | RX-Summary: %s\n", 'A' + flow, senderSummaryReceived[flow] ? "EMPFANGEN" : "FEHLT",
      summaryReceived[flow] ? "EMPFANGEN" : "FEHLT");
    }
    Serial.println("Mindestens eine TX- oder RX-Summary fehlt.");
    Serial.println("Dieser Test wird nicht ausgewertet.");
    Serial.println("====================================");
    testActive = false;
    scheduleNextTest();
  }
  if (resultReady) {
    resultReady = false;
    printParallelResult();
  }
  if (MY_ID == INITIATOR_ID && automaticMode && !testActive && nextTestScheduled && timeReached(nextTestMs)) {
    nextTestScheduled = false;
    startParallelTest();
  }
}