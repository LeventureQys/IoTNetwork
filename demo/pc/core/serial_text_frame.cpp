#include "serial_text_frame.h"

namespace serial_text_frame {

bool Encode(const std::string &text, std::vector<uint8_t> *out)
{
    if (out == nullptr || text.empty() || text.size() > kMaxPayloadBytes)
        return false;

    const uint16_t payload_len = static_cast<uint16_t>(text.size());
    out->clear();
    out->reserve(6 + text.size());
    out->push_back(kSof0);
    out->push_back(kSof1);
    out->push_back(kVersion);
    out->push_back(static_cast<uint8_t>((payload_len >> 8) & 0xFF));
    out->push_back(static_cast<uint8_t>(payload_len & 0xFF));
    out->insert(out->end(), text.begin(), text.end());

    uint8_t checksum = 0;
    for (uint8_t byte : *out)
        checksum ^= byte;
    out->push_back(checksum);
    return true;
}

bool Decode(const uint8_t *bytes, size_t length, std::string *text)
{
    if (bytes == nullptr || text == nullptr || length < 6)
        return false;
    if (bytes[0] != kSof0 || bytes[1] != kSof1 || bytes[2] != kVersion)
        return false;

    const size_t payload_len =
        (static_cast<size_t>(bytes[3]) << 8) | static_cast<size_t>(bytes[4]);
    if (payload_len == 0 || payload_len > kMaxPayloadBytes ||
        length != 6 + payload_len)
        return false;

    uint8_t checksum = 0;
    for (size_t i = 0; i + 1 < length; ++i)
        checksum ^= bytes[i];
    if (checksum != bytes[length - 1])
        return false;

    text->assign(reinterpret_cast<const char *>(bytes + 5), payload_len);
    return true;
}

std::string Hex(const uint8_t *bytes, size_t length)
{
    static const char kHex[] = "0123456789ABCDEF";
    std::string out;
    if (bytes == nullptr || length == 0)
        return out;
    out.reserve(length * 3);
    for (size_t i = 0; i < length; ++i) {
        if (i != 0)
            out.push_back(' ');
        out.push_back(kHex[(bytes[i] >> 4) & 0x0F]);
        out.push_back(kHex[bytes[i] & 0x0F]);
    }
    return out;
}

} // namespace serial_text_frame
