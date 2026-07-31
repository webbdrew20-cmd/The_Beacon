

#include <SPI.h>
#include <RadioLib.h>
#include <FS.h>
#include <LittleFS.h>
#include <Preferences.h>
#include "mesh_types.h"


#define LORA_CS    10
#define LORA_SCK   11
#define LORA_MOSI  12
#define LORA_MISO  13
#define LORA_RST   7
#define LORA_BUSY  6
#define LORA_DIO1  5

#define P4_UART_RX 8      
#define P4_UART_TX 9      
#define P4_BAUD    115200

#define LORA_FREQ_MHZ   915.0f   
#define LORA_BW_KHZ     125.0f
#define LORA_SF         9       
#define LORA_CR         7       
#define LORA_POWER_DBM  20
#define LORA_PREAMBLE   8

#define MESH_TTL_DEFAULT   3     
#define SEEN_CACHE_SIZE    32

#define INBOX_PATH         "/inbox.jsonl"
#define INBOX_MAX_BYTES    (200*1024)

#define HEARTBEAT_MS 3000


SX1262 radio = new Module(LORA_CS, LORA_DIO1, LORA_RST, LORA_BUSY);

volatile bool rxFlag = false;
void onDio1() { rxFlag = true; }


Preferences prefs;
uint32_t myNodeId = 0;
char myName[24] = "";

uint32_t seenIds[SEEN_CACHE_SIZE];
int seenHead = 0;

bool inboxHasData = false;
bool g_radioOK = false;
unsigned long lastHb = 0;
unsigned long lastFetchMs = 0;
unsigned long lastStatusPrint = 0;


static void putU32(uint8_t *p, uint32_t v) {
  p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}
static uint32_t getU32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static int packMesh(const MeshMsg &m, uint8_t *out) {
  putU32(out + 0, m.id); putU32(out + 4, m.from); putU32(out + 8, m.to);
  out[12] = m.ttl; out[13] = m.ch; out[14] = m.textLen;
  memcpy(out + 15, m.text, m.textLen);
  return 15 + m.textLen;
}
static bool unpackMesh(const uint8_t *in, size_t len, MeshMsg &m) {
  if (len < 15) return false;
  m.id = getU32(in + 0); m.from = getU32(in + 4); m.to = getU32(in + 8);
  m.ttl = in[12]; m.ch = in[13]; m.textLen = in[14];
  if ((size_t)(15 + m.textLen) > len || m.textLen > MAX_TEXT_LEN) return false;
  memcpy(m.text, in + 15, m.textLen);
  m.text[m.textLen] = 0;
  return true;
}

static bool seen(uint32_t id) {
  for (int i = 0; i < SEEN_CACHE_SIZE; i++) if (seenIds[i] == id) return true;
  return false;
}
static void markSeen(uint32_t id) {
  seenIds[seenHead] = id;
  seenHead = (seenHead + 1) % SEEN_CACHE_SIZE;
}


static void jesc(const char *in, char *out, size_t cap) {
  size_t o = 0;
  for (; *in && o + 2 < cap; in++) {
    unsigned char c = (unsigned char)*in;
    if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = c; }
    else if (c < 0x20) out[o++] = ' ';
    else out[o++] = c;
  }
  out[o] = 0;
}
static bool jsonStr(const char *json, const char *key, char *out, size_t cap) {
  char pat[32]; snprintf(pat, sizeof(pat), "\"%s\":\"", key);
  const char *p = strstr(json, pat);
  if (!p) return false;
  p += strlen(pat);
  size_t o = 0;
  while (*p && *p != '"' && o < cap - 1) {
    if (*p == '\\' && p[1]) p++;
    out[o++] = *p++;
  }
  out[o] = 0;
  return true;
}
static bool jsonNum(const char *json, const char *key, long *out) {
  char pat[32]; snprintf(pat, sizeof(pat), "\"%s\":", key);
  const char *p = strstr(json, pat);
  if (!p) return false;
  *out = atol(p + strlen(pat));
  return true;
}


static void inboxAppendStr(const char *fromStr, uint8_t ch, const char *text) {
  File f = LittleFS.open(INBOX_PATH, FILE_APPEND);
  if (!f) return;
  if (f.size() > INBOX_MAX_BYTES) {   
    f.close();
    File in = LittleFS.open(INBOX_PATH, "r");
    String rest = in ? in.readString() : "";
    if (in) in.close();
    rest = rest.substring(rest.length() / 2);
    int nl = rest.indexOf('\n');
    if (nl > 0) rest = rest.substring(nl + 1);
    File out = LittleFS.open(INBOX_PATH, "w");
    if (out) { out.print(rest); out.close(); }
    f = LittleFS.open(INBOX_PATH, FILE_APPEND);
    if (!f) return;
  }
  char esc[MAX_TEXT_LEN * 2 + 8];
  jesc(text, esc, sizeof(esc));
  char fesc[28];
  jesc(fromStr, fesc, sizeof(fesc));
  f.printf("{\"from\":\"%s\",\"ch\":%u,\"text\":\"%s\"}\n", fesc, ch, esc);
  f.close();
  inboxHasData = true;
}

static void inboxAppend(uint32_t from, uint8_t ch, const char *text) {
  char fs[12];
  snprintf(fs, sizeof(fs), "%08x", (unsigned)from);
  inboxAppendStr(fs, ch, text);
}

static void inboxSendToP4() {
  File f = LittleFS.open(INBOX_PATH, "r");
  if (!f || f.size() == 0) { if (f) f.close(); return; }
  Serial1.print("{\"type\":\"msgs\",\"items\":[");
  bool first = true;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    if (line.length() < 2) continue;
    if (!first) Serial1.print(',');
    Serial1.print(line);
    first = false;
  }
  Serial1.print("]}\n");
  f.close();
}

static void inboxClear() {
  File f = LittleFS.open(INBOX_PATH, "w");   // truncate
  if (f) f.close();
  inboxHasData = false;
}


static void handleReceivedPacket(const uint8_t *buf, size_t len) {
  MeshMsg m;
  if (!unpackMesh(buf, len, m)) return;
  if (seen(m.id)) return;
  markSeen(m.id);

  bool forMe = (m.to == 0 || m.to == myNodeId);
  if (forMe) inboxAppend(m.from, m.ch, m.text);

 
  bool shouldRelay = (m.to != myNodeId) && (m.ttl > 0);
  if (shouldRelay) {
    m.ttl--;
    delay(random(50, 250));   // jitter — reduces collision storms on relay
    uint8_t out[15 + MAX_TEXT_LEN];
    int n = packMesh(m, out);
    radio.transmit(out, n);
    radio.startReceive();
  }
}


static void sendMeshMessage(const char *toStr, uint8_t ch, const char *text) {
  MeshMsg m;
  m.id = (uint32_t)random(1, 0x7fffffff);
  m.from = myNodeId;
  m.to = (toStr && toStr[0]) ? strtoul(toStr, NULL, 16) : 0;
  m.ttl = MESH_TTL_DEFAULT;
  m.ch = ch;
  m.textLen = (uint8_t)strnlen(text, MAX_TEXT_LEN);
  memcpy(m.text, text, m.textLen);

  markSeen(m.id);   // don't re-process our own message if it floods back

  if (!g_radioOK) {
    inboxAppendStr("system", ch,
      "Radio offline - message NOT sent over the air. Check wiring.");
    return;
  }

  uint8_t out[15 + MAX_TEXT_LEN];
  int n = packMesh(m, out);
  int st = radio.transmit(out, n);
  radio.startReceive();
  if (st != RADIOLIB_ERR_NONE) {
    char eb[64];
    snprintf(eb, sizeof(eb), "Radio TX failed (code %d)", st);
    inboxAppendStr("system", ch, eb);
    Serial.println(eb);
  } else {
    Serial.printf("TX ok: %d bytes over the air\n", n);
  }
}


static void handleP4Line(const char *line) {
  if (strstr(line, "\"fetch\"")) {
    lastFetchMs = millis();
    if (inboxHasData) inboxSendToP4();
  } else if (strstr(line, "\"ack\"")) {
    inboxClear();
  } else if (strstr(line, "\"send\"")) {
    char to[24] = "", text[MAX_TEXT_LEN + 1] = "";
    long ch = 0;
    jsonStr(line, "to", to, sizeof(to));
    jsonStr(line, "text", text, sizeof(text));
    jsonNum(line, "ch", &ch);
    sendMeshMessage(to, (uint8_t)ch, text);

   
    if (strncmp(text, "/echo", 5) == 0) {
      const char *rest = text + 5;
      while (*rest == ' ') rest++;
      char reply[MAX_TEXT_LEN + 16];
      if (*rest)
        snprintf(reply, sizeof(reply), "Echo: %s", rest);
      else
        snprintf(reply, sizeof(reply), "Echo OK - inbound path works");
      char myid[12];
      snprintf(myid, sizeof(myid), "%08x", (unsigned)myNodeId);
      inboxAppendStr(myid, (uint8_t)ch, reply);
    }
  } else if (strstr(line, "set_name")) {
    char n[24];
    if (jsonStr(line, "name", n, sizeof(n))) {
      strlcpy(myName, n, sizeof(myName));
      prefs.putString("name", myName);
    }
  }
}


void setup() {
  Serial.begin(115200);
 
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 3000) delay(10);
  delay(200);
  Serial.println("=== Beacon LoRa node booting ===");

  Serial1.begin(P4_BAUD, SERIAL_8N1, P4_UART_RX, P4_UART_TX);

  myNodeId = (uint32_t)(ESP.getEfuseMac() & 0xFFFFFFFF);
  prefs.begin("beacon", false);
  String savedName = prefs.getString("name", "");
  strlcpy(myName, savedName.c_str(), sizeof(myName));
  Serial.printf("Node ID: %08x  Name: %s\n", (unsigned)myNodeId, myName[0] ? myName : "(unset)");

  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed — inbox will not persist across reboots");
  }
  File initF = LittleFS.open(INBOX_PATH, "r");
  inboxHasData = initF && initF.size() > 0;
  if (initF) initF.close();

  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);
  int state = radio.begin(LORA_FREQ_MHZ, LORA_BW_KHZ, LORA_SF, LORA_CR,
                          RADIOLIB_SX126X_SYNC_WORD_PRIVATE, LORA_POWER_DBM,
                          LORA_PREAMBLE);
  g_radioOK = (state == RADIOLIB_ERR_NONE);
  if (!g_radioOK) {
    Serial.printf("Radio init failed, code %d — check wiring/pins\n", state);
  } else {
    radio.setDio1Action(onDio1);
    radio.startReceive();
    Serial.println("Radio listening");
  }

  randomSeed(esp_random());

 
  char myid[12];
  snprintf(myid, sizeof(myid), "%08x", (unsigned)myNodeId);
  char boot[MAX_TEXT_LEN];
  snprintf(boot, sizeof(boot),
           "Node %s online. Radio: %s. Storage: %s.%s%s",
           myid,
           g_radioOK ? "OK" : "FAILED - check wiring",
           LittleFS.begin(false) ? "OK" : "FAILED",
           myName[0] ? " Name: " : "",
           myName[0] ? myName : "");
  inboxAppendStr(myid, 0, boot);
  Serial.println(boot);
  Serial.println("Waiting for P4 fetch (every ~3s) to deliver boot message...");
}


void loop() {
 
  if (rxFlag) {
    rxFlag = false;
    uint8_t buf[15 + MAX_TEXT_LEN];
    int len = radio.getPacketLength();
    if (len > 0 && (size_t)len <= sizeof(buf)) {
      int state = radio.readData(buf, len);
      if (state == RADIOLIB_ERR_NONE) handleReceivedPacket(buf, len);
    }
    radio.startReceive();
  }

  
  static char line[300];
  static int llen = 0;
  while (Serial1.available()) {
    char c = (char)Serial1.read();
    if (c == '\n' || c == '\r') {
      if (llen > 0) { line[llen] = 0; handleP4Line(line); llen = 0; }
    } else if (llen < (int)sizeof(line) - 1) {
      line[llen++] = c;
    } else {
      llen = 0;
    }
  }

  
  unsigned long now = millis();
  if (now - lastHb >= HEARTBEAT_MS) {
    lastHb = now;
    if (!inboxHasData) Serial1.print("{\"type\":\"hb\"}\n");
  }

  
  if (now - lastStatusPrint >= 5000) {
    lastStatusPrint = now;
    Serial.printf("alive %lus | radio %s | inbox %s | P4 fetch: ",
                  now / 1000, g_radioOK ? "OK" : "FAIL",
                  inboxHasData ? "pending" : "empty");
    if (lastFetchMs == 0) Serial.println("never (check UART to P4)");
    else Serial.printf("%lus ago\n", (now - lastFetchMs) / 1000);
  }
}
