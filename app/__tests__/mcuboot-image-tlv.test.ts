import { parseImageSha256, uint8ArrayToHex } from '@/services/mcumgr';
import { sha256 } from 'js-sha256';

import fixture from './fixtures/mcuboot-image-header.json';

/**
 * The fixture is a real MCUboot header + TLV trailer, lifted from the published
 * fw-v2.1.0 `fw.signed.bin` (payload zeroed to keep it small). Using the real bytes
 * matters: a hand-written fixture would only prove the parser agrees with whatever I
 * assumed the format to be, which is exactly the assumption under test.
 */
const IMAGE = Uint8Array.from(fixture.bytes as number[]);

describe('parseImageSha256', () => {
    it("returns the image's own IMAGE_TLV_SHA256 from a real signed image", () => {
        // Cross-checked against fw/tools/dump_dfu_tlv.py on the same artifact.
        expect(parseImageSha256(IMAGE)).toBe(fixture.expectedSha256);
    });

    it('is NOT the sha256 of the whole file', () => {
        // The trap this parser exists to avoid. The file includes the TLV trailer,
        // which the digest inside that trailer obviously cannot cover - so hashing the
        // .bin gives a value the device will never report, and verification built on
        // it would fail every single update.
        expect(fixture.expectedSha256).not.toBe(fixture.wholeFileSha256OfRealImage);
        expect(uint8ArrayToHex(Uint8Array.from(sha256.array(IMAGE)))).not.toBe(
            fixture.expectedSha256
        );
    });

    it('returns null for data that is not an MCUboot image', () => {
        expect(parseImageSha256(Uint8Array.from([1, 2, 3]))).toBeNull();
        expect(parseImageSha256(new Uint8Array(64))).toBeNull();
    });

    it('returns null rather than reading past the end when the digest is cut short', () => {
        // Header is 512 bytes and the fixture's payload is 16, so the TLV area starts
        // at 528: 4 bytes of area header, 4 of record header, then the 32-byte digest.
        // 540 lands inside that digest.
        expect(parseImageSha256(IMAGE.subarray(0, 540))).toBeNull();
    });

    it('still finds the digest when only later TLVs are missing', () => {
        // The digest precedes the signature TLVs, so a trailer truncated after it is
        // still enough to verify against - worth pinning so the loop is not "fixed"
        // into rejecting it.
        expect(parseImageSha256(IMAGE.subarray(0, 576))).toBe(fixture.expectedSha256);
    });
});
