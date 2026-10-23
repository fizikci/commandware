#pragma once
#define PROGMEM
inline uint8_t pgm_read_byte(const uint8_t *address) { return *address; }
