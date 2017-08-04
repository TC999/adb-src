#pragma once

#include <stdint.h>

#include <string_view>
#include <vector>

namespace adb {

enum PacketCommand : uint32_t {
  A_CNXN = 0x4e584e43,
  A_AUTH = 0x48545541,
  A_OPEN = 0x4e45504f,
  A_OKAY = 0x59414b4f,
  A_CLSE = 0x45534c43,
  A_WRTE = 0x45545257,
};

struct __attribute__((packed, aligned(4))) PacketHeader {
  uint32_t command; /* command identifier constant      */
  uint32_t arg0;    /* first argument                   */
  uint32_t arg1;    /* second argument                  */

  // Automatically set/verified.
  uint32_t data_length;   /* length of payload (0 is allowed) */
  uint32_t data_checksum; /* checksum of data payload         */
  uint32_t magic;         /* command ^ 0xffffffff             */

  void Update(uint32_t data_length, uint32_t data_checksum) {
    this->data_length = data_length;
    this->data_checksum = data_checksum;
    this->magic = ~this->command;
  }
};

struct Packet {
  Packet() = default;
  Packet(const Packet& copy) = delete;
  Packet(Packet&& move) = default;

  PacketHeader header;
  std::vector<char> data;

  uint32_t CalculateDataChecksum() const {
    uint32_t sum = 0;
    for (char byte : data) {
      sum += static_cast<unsigned char>(byte);
    }
    return sum;
  }

  bool ValidateHeader() const {
    return header.data_checksum == CalculateDataChecksum() && header.magic == ~header.command;
  }

  void UpdateHeader() { header.Update(data.size(), CalculateDataChecksum()); }
};

template <typename IOStream>
IOStream& operator<<(IOStream& stream, const Packet& packet) {
  std::string_view payload_string(packet.data.data(), packet.data.size());
  switch (packet.header.command) {
    case A_CNXN:
      stream << "[CNXN] arg0 = " << packet.header.arg0 << ", arg1 = " << packet.header.arg1
             << ", banner = '" << payload_string << "'";
      break;

    case A_AUTH:
      stream << "[AUTH]";
      break;

    case A_OPEN:
      // No quotes around payload_string, because it has an embedded null.
      stream << "[OPEN] local_id = " << packet.header.arg0 << ", path = " << payload_string;
      break;

    case A_OKAY:
      stream << "[OKAY] local_id = " << packet.header.arg0 << ", remote_id = " << packet.header.arg1;
      break;

    case A_CLSE:
      stream << "[CLSE] local_id = " << packet.header.arg0 << ", remote_id = " << packet.header.arg1;
      break;

    case A_WRTE:
      stream << "[WRTE] local_id = " << packet.header.arg0 << ", remote_id = " << packet.header.arg1
             << ", payload: " << packet.data.size() << " bytes";
      break;
  }

  if (!packet.ValidateHeader()) {
    stream << "[CHECKSUM FAILED]";
  }

  return stream;
}
}
