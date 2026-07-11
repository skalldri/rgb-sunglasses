#include <extensions/extension_metadata_blob.h>

#include <cstring>

namespace extension_metadata_blob {

void init(uint8_t *buf) {
    buf[0] = kVersion;
    buf[1] = 0;
}

bool append(uint8_t *buf, size_t bufSize, size_t *pos, uint8_t cpfFormat, const char *name) {
    size_t nameLen = strlen(name);
    if (nameLen > kMaxNameLen) {
        nameLen = kMaxNameLen;
    }
    if (*pos + 2 + nameLen > bufSize) {
        return false;
    }
    buf[(*pos)++] = cpfFormat;
    buf[(*pos)++] = static_cast<uint8_t>(nameLen);
    memcpy(buf + *pos, name, nameLen);
    *pos += nameLen;
    return true;
}

size_t finish(uint8_t *buf, size_t pos, uint8_t entryCount) {
    buf[1] = entryCount;
    return pos;
}

}  // namespace extension_metadata_blob
