/*
 * extensions.metadata_blob — native_sim tests for the pure byte-packer
 * backing the runtime bulk-metadata characteristic for extension GATT
 * services (extension_metadata_blob.cpp, issue #90). Round-trips against a
 * hand-decoded expected buffer, mirroring exactly what the app's
 * parseMetadataBlob() (app/services/ble-value-codec.ts) expects:
 * [version][entry_count] then entry_count * [cpf_format][name_len][name_bytes].
 */

#include <extensions/extension_metadata_blob.h>
#include <zephyr/ztest.h>

#include <cstring>

using extension_metadata_blob::append;
using extension_metadata_blob::finish;
using extension_metadata_blob::init;
using extension_metadata_blob::kMaxBlobSize;
using extension_metadata_blob::kMaxEntries;
using extension_metadata_blob::kMaxNameLen;
using extension_metadata_blob::kVersion;

namespace {

struct DecodedEntry {
    uint8_t cpfFormat;
    char name[extension_metadata_blob::kMaxNameLen + 1];
};

/* Hand-decodes a blob the same way the app's parseMetadataBlob() does, so
 * these tests double as firmware-side proof of the app's exact expectations. */
size_t decode(const uint8_t *buf, size_t len, DecodedEntry *out, size_t outCapacity) {
    zassert_true(len >= 2, "blob too short for header");
    zassert_equal(buf[0], kVersion, "version byte mismatch");
    uint8_t entryCount = buf[1];
    size_t pos = 2;
    for (uint8_t i = 0; i < entryCount; i++) {
        zassert_true(i < outCapacity, "more entries than test buffer can hold");
        zassert_true(pos + 2 <= len, "truncated entry header");
        uint8_t cpfFormat = buf[pos++];
        uint8_t nameLen = buf[pos++];
        zassert_true(pos + nameLen <= len, "truncated entry name");
        out[i].cpfFormat = cpfFormat;
        memcpy(out[i].name, buf + pos, nameLen);
        out[i].name[nameLen] = '\0';
        pos += nameLen;
    }
    return entryCount;
}

}  // namespace

ZTEST(extension_metadata_blob_suite, test_init_writes_version_and_zero_count) {
    uint8_t buf[kMaxBlobSize] = {};
    init(buf);
    zassert_equal(buf[0], kVersion);
    zassert_equal(buf[1], 0);
}

ZTEST(extension_metadata_blob_suite, test_single_entry_round_trip) {
    uint8_t buf[kMaxBlobSize] = {};
    init(buf);
    size_t pos = 2;
    uint8_t entryCount = 0;

    zassert_true(append(buf, sizeof(buf), &pos, 0x04 /* BOOLEAN */, "Is Active"));
    entryCount++;

    size_t total = finish(buf, pos, entryCount);

    DecodedEntry entries[4];
    size_t count = decode(buf, total, entries, 4);
    zassert_equal(count, 1u);
    zassert_equal(entries[0].cpfFormat, 0x04);
    zassert_equal(strcmp(entries[0].name, "Is Active"), 0);
}

ZTEST(extension_metadata_blob_suite, test_multi_entry_round_trip_preserves_order) {
    uint8_t buf[kMaxBlobSize] = {};
    init(buf);
    size_t pos = 2;
    uint8_t entryCount = 0;

    zassert_true(append(buf, sizeof(buf), &pos, 0x19 /* UTF8S */, "Animation Name"));
    entryCount++;
    zassert_true(append(buf, sizeof(buf), &pos, 0x04 /* BOOLEAN */, "Is Active"));
    entryCount++;
    zassert_true(append(buf, sizeof(buf), &pos, 0x08 /* UINT32 */, "Speed"));
    entryCount++;
    zassert_true(append(buf, sizeof(buf), &pos, 0x1A /* RGB888 */, "Color"));
    entryCount++;

    size_t total = finish(buf, pos, entryCount);

    DecodedEntry entries[4];
    size_t count = decode(buf, total, entries, 4);
    zassert_equal(count, 4u);
    zassert_equal(strcmp(entries[0].name, "Animation Name"), 0);
    zassert_equal(strcmp(entries[1].name, "Is Active"), 0);
    zassert_equal(strcmp(entries[2].name, "Speed"), 0);
    zassert_equal(entries[2].cpfFormat, 0x08);
    zassert_equal(strcmp(entries[3].name, "Color"), 0);
    zassert_equal(entries[3].cpfFormat, 0x1A);
}

ZTEST(extension_metadata_blob_suite, test_append_truncates_overlong_name_defensively) {
    uint8_t buf[kMaxBlobSize] = {};
    init(buf);
    size_t pos = 2;

    char longName[kMaxNameLen + 20];
    memset(longName, 'X', sizeof(longName) - 1);
    longName[sizeof(longName) - 1] = '\0';

    zassert_true(append(buf, sizeof(buf), &pos, 0x08, longName));
    size_t total = finish(buf, pos, 1);

    DecodedEntry entries[1];
    size_t count = decode(buf, total, entries, 1);
    zassert_equal(count, 1u);
    zassert_equal(strlen(entries[0].name), kMaxNameLen,
                  "name must be truncated to kMaxNameLen bytes, got %zu", strlen(entries[0].name));
}

ZTEST(extension_metadata_blob_suite, test_append_returns_false_on_overflow_without_corrupting_state) {
    uint8_t buf[5] = {};  // room for header (2) + exactly one 3-byte entry, no more
    init(buf);
    size_t pos = 2;

    zassert_true(append(buf, sizeof(buf), &pos, 0x08, "A"));  // 2 + 1 = 3 bytes, fits exactly
    zassert_equal(pos, 5u);

    size_t posBefore = pos;
    bool ok = append(buf, sizeof(buf), &pos, 0x08, "B");  // would need 3 more bytes - no room
    zassert_false(ok, "expected append to refuse when it would overflow bufSize");
    zassert_equal(pos, posBefore, "pos must be left unchanged on a refused append");
}

ZTEST(extension_metadata_blob_suite, test_worst_case_fill_fits_and_round_trips) {
    uint8_t buf[kMaxBlobSize] = {};
    init(buf);
    size_t pos = 2;

    char name[kMaxNameLen + 1];
    memset(name, 'N', kMaxNameLen);
    name[kMaxNameLen] = '\0';

    for (size_t i = 0; i < kMaxEntries; i++) {
        zassert_true(append(buf, sizeof(buf), &pos, 0x08, name),
                     "entry %zu of %zu should fit within kMaxBlobSize", i, kMaxEntries);
    }
    size_t total = finish(buf, pos, static_cast<uint8_t>(kMaxEntries));
    zassert_equal(total, kMaxBlobSize, "a full worst-case fill should use exactly kMaxBlobSize bytes");

    DecodedEntry entries[kMaxEntries];
    size_t count = decode(buf, total, entries, kMaxEntries);
    zassert_equal(count, kMaxEntries);
    zassert_equal(strlen(entries[kMaxEntries - 1].name), kMaxNameLen);
}

ZTEST_SUITE(extension_metadata_blob_suite, NULL, NULL, NULL, NULL, NULL);
