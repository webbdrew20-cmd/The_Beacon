#pragma once
#define MAX_TEXT_LEN 200

struct MeshMsg {
  uint32_t id, from, to;
  uint8_t ttl, ch, textLen;
  char text[MAX_TEXT_LEN + 1];
};
