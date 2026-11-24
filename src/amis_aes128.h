// SPDX-License-Identifier: MIT
// libamis - AES-128 (FIPS-197) with CBC decryption, small and without dependencies.
// Runs on ESP8266 (tables in flash), ESP32 and on a PC.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace amis {

class Aes128 {
 public:
  static constexpr size_t kBlockSize = 16;
  static constexpr size_t kKeySize = 16;

  void set_key(const uint8_t key[kKeySize]);
  void encrypt_block(uint8_t block[kBlockSize]) const;
  void decrypt_block(uint8_t block[kBlockSize]) const;

  // CBC decryption in place. len must be a multiple of 16. iv is not modified.
  bool cbc_decrypt(uint8_t *data, size_t len, const uint8_t iv[kBlockSize]) const;
  // CBC encryption in place (for tests and simulation).
  bool cbc_encrypt(uint8_t *data, size_t len, const uint8_t iv[kBlockSize]) const;

 private:
  uint8_t round_keys_[176];
};

}  // namespace amis
