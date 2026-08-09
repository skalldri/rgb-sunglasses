/**
 * MCUmgr (MCU Manager) Service
 * 
 * Implements the SMP (Simple Management Protocol) for communicating with
 * Zephyr-based devices over BLE for firmware updates and device management.
 * 
 * Reference: https://docs.zephyrproject.org/latest/services/device_mgmt/smp_protocol.html
 */

import { sha256 } from 'js-sha256';
import { Characteristic, Device } from 'react-native-ble-plx';
// @ts-ignore - cbor-js doesn't have type declarations
import CBOR from 'cbor-js';

// ============================================================================
// Constants
// ============================================================================

// SMP BLE Service and Characteristic UUIDs
export const SMP_SERVICE_UUID = '8d53dc1d-1db7-4cd3-868b-8a527460aa84';
export const SMP_CHARACTERISTIC_UUID = 'da2e7828-fbce-4e01-ae9e-261174997c48';

// SMP Operation Types (OP field in header)
export enum SmpOp {
    READ_REQUEST = 0,
    READ_RESPONSE = 1,
    WRITE_REQUEST = 2,
    WRITE_RESPONSE = 3,
}

// SMP Management Groups
export enum SmpGroup {
    OS = 0,           // Default/OS Management Group
    IMAGE = 1,        // Application/software image management group
    STAT = 2,         // Statistics management
    CONFIG = 3,       // Settings (Config) Management Group
    LOG = 4,          // Application/system log management
    CRASH = 5,        // Run-time tests
    SPLIT = 6,        // Split image management
    RUN = 7,          // Test crashing application
    FS = 8,           // File management
    SHELL = 9,        // Shell management
    ZEPHYR = 63,      // Zephyr Management Group
    // This firmware's own file-management group (MGMT_GROUP_ID_PERUSER) —
    // list/delete for boot-scoped asset files. The id, the `kind` field and the
    // CBOR schema are an app↔firmware compatibility surface, append-only, defined
    // in fw/src/extensions/extension_mgmt.h and fw/docs/extension-management.md.
    FILE_MGMT = 64,
}

// Image Management Commands (Group 1)
export enum ImageCmd {
    STATE = 0,        // Get/set state of images
    UPLOAD = 1,       // Image upload
    FILE = 2,         // File (reserved)
    CORELIST = 3,     // Corelist (reserved)
    CORELOAD = 4,     // Coreload (reserved)
    ERASE = 5,        // Image erase
    SLOT_INFO = 6,    // Slot info
}

// OS Management Commands (Group 0)
export enum OsCmd {
    ECHO = 0,         // Echo
    CONSOLE = 1,      // Console/terminal
    TASKSTAT = 2,     // Task statistics
    MPSTAT = 3,       // Memory pool statistics
    DATETIME_GET = 4, // Get date time
    RESET = 5,        // Reset
    MCUMGR_PARAMS = 6, // MCUmgr parameters
    INFO = 7,         // OS/Application info
    BOOTLOADER_INFO = 8, // Bootloader info
}

// FS management commands (Group 8)
export enum FsCmd {
    DOWNLOAD_UPLOAD = 0,
    STATUS = 1,
    HASH = 2,
    LIST_SUPPORTED_HASHES = 3,
    CLOSE = 4,
}

// FS Management Group Error Codes (Group 8), from Zephyr's fs_mgmt_err_code_t.
// FILE_NOT_FOUND is the one the extension sync path branches on: it means
// "device doesn't have this file", i.e. install it, not "something went wrong".
export enum FsMgmtError {
    OK = 0,
    UNKNOWN = 1,
    FILE_INVALID_NAME = 2,
    FILE_NOT_FOUND = 3,
    FILE_IS_DIRECTORY = 4,
    FILE_OPEN_FAILED = 5,
    FILE_SEEK_FAILED = 6,
    FILE_READ_FAILED = 7,
    FILE_TRUNCATE_FAILED = 8,
    FILE_DELETE_FAILED = 9,
    FILE_WRITE_FAILED = 10,
    FILE_OFFSET_NOT_VALID = 11,
    FILE_OFFSET_LARGER_THAN_FILE = 12,
    CHECKSUM_HASH_NOT_FOUND = 13,
    MOUNT_POINT_NOT_FOUND = 14,
    READ_ONLY_FILESYSTEM = 15,
    FILE_EMPTY = 16,
}

// FILE_MGMT commands (group 64), mirroring extension_mgmt.h's CommandId.
export enum FileMgmtCmd {
    LIST = 0,
    DELETE = 1,
}

// FILE_MGMT group error codes, mirroring extension_mgmt.h's Error enum.
// Append-only: the firmware promises never to renumber these.
export enum FileMgmtError {
    OK = 0,
    /** The `kind` is not supported by this firmware (e.g. "glim" today). */
    KIND_UNSUPPORTED = 1,
    /** Name failed the on-device path fence (too long, traversal, separator). */
    INVALID_NAME = 2,
    NOT_FOUND = 3,
    UNLINK_FAILED = 4,
    /** File was deleted but post-delete cleanup (retire/purge) failed. */
    CLEANUP_FAILED = 5,
}

/**
 * File kinds the FILE_MGMT group is parameterized over. Only extensions exist
 * today; "glim" is reserved by the design for stored-animation assets.
 */
export type DeviceFileKind = 'ext';

// SMP Error Codes
export enum SmpError {
    OK = 0,
    EUNKNOWN = 1,
    ENOMEM = 2,
    EINVAL = 3,
    ETIMEOUT = 4,
    ENOENT = 5,
    EBADSTATE = 6,
    EMSGSIZE = 7,
    ENOTSUP = 8,
    ECORRUPT = 9,
    EBUSY = 10,
    EACCESSDENIED = 11,
    UNSUPPORTED_TOO_OLD = 12,
    UNSUPPORTED_TOO_NEW = 13,
}

// Image Group Error Codes
export enum ImageError {
    OK = 0,
    UNKNOWN = 1,
    FLASH_CONFIG_QUERY_FAIL = 2,
    NO_IMAGE = 3,
    NO_TLVS = 4,
    INVALID_TLV = 5,
    TLV_MULTIPLE_HASHES_FOUND = 6,
    TLV_INVALID_SIZE = 7,
    HASH_NOT_FOUND = 8,
    NO_FREE_SLOT = 9,
    FLASH_OPEN_FAILED = 10,
    FLASH_READ_FAILED = 11,
    FLASH_WRITE_FAILED = 12,
    FLASH_ERASE_FAILED = 13,
    INVALID_SLOT = 14,
    NO_FREE_MEMORY = 15,
    FLASH_CONTEXT_ALREADY_SET = 16,
    FLASH_CONTEXT_NOT_SET = 17,
    FLASH_AREA_DEVICE_NULL = 18,
    INVALID_PAGE_OFFSET = 19,
    INVALID_OFFSET = 20,
    INVALID_LENGTH = 21,
    INVALID_IMAGE_HEADER = 22,
    INVALID_IMAGE_HEADER_MAGIC = 23,
    INVALID_HASH = 24,
    INVALID_FLASH_ADDRESS = 25,
    VERSION_GET_FAILED = 26,
    CURRENT_VERSION_IS_NEWER = 27,
    IMAGE_ALREADY_PENDING = 28,
    INVALID_IMAGE_VECTOR_TABLE = 29,
    INVALID_IMAGE_TOO_LARGE = 30,
    INVALID_IMAGE_DATA_OVERRUN = 31,
    IMAGE_CONFIRMATION_DENIED = 32,
    IMAGE_SETTING_TEST_TO_ACTIVE_DENIED = 33,
}

// SMP Header size (8 bytes)
export const SMP_HEADER_SIZE = 8;

// Default MTU for BLE (conservative)
const DEFAULT_MTU = 400;

// ============================================================================
// Types
// ============================================================================

export interface SmpHeader {
    op: SmpOp;
    version: number;
    flags: number;
    length: number;
    group: SmpGroup;
    sequence: number;
    command: number;
}

export interface ImageSlot {
    image?: number;
    slot: number;
    version: string;
    hash?: Uint8Array;
    bootable?: boolean;
    pending?: boolean;
    confirmed?: boolean;
    active?: boolean;
    permanent?: boolean;
}

export interface ImageStateResponse {
    images: ImageSlot[];
    splitStatus?: number;
}

export interface ImageUploadResponse {
    off?: number;
    match?: boolean;
    rc?: number;
    err?: { group: number; rc: number };
}

export interface SlotInfoResponse {
    images: {
        image: number;
        slots: {
            slot: number;
            size: number;
            upload_image_id?: number;
        }[];
        max_image_size?: number;
    }[];
}

export type UploadProgressCallback = (bytesSent: number, totalBytes: number) => void;

/**
 * An SMP command that the device answered with an error.
 *
 * `rc` is the return code and `group` the management group it belongs to, or
 * undefined for the legacy shape that reports a bare `rc` with no group. Both
 * are carried so callers can branch on a specific device-side outcome without
 * string-matching the message.
 */
export class SmpCommandError extends Error {
    readonly rc: number;
    readonly group?: number;

    constructor(message: string, rc: number, group?: number) {
        super(message);
        this.name = 'SmpCommandError';
        this.rc = rc;
        this.group = group;
        // Required for `instanceof` to work when targeting ES5, which the app's
        // tsconfig does through React Native's preset.
        Object.setPrototypeOf(this, SmpCommandError.prototype);
    }
}

export interface FileHashResponse {
    /** Algorithm the device used, e.g. "sha256". */
    type: string;
    off?: number;
    /** Number of bytes of the file that were hashed. */
    len: number;
    /**
     * A byte string for hashes and a number for checksums (CRC32). CBOR byte
     * strings decode to an ArrayBuffer or a typed array depending on the decoder,
     * so both are possible here.
     */
    output: number | Uint8Array | ArrayBuffer;
}

/**
 * One file the device reported from a FILE_MGMT LIST — the union of what is on
 * disk and what the boot scan loaded, so divergent states ("uploaded since
 * boot", "deleted since boot but still loaded") are named rather than implied.
 *
 * The slot fields (displayName/slot/faulted/active/retired) are only present
 * when `loaded` is true — a file uploaded since boot has no slot yet.
 */
export interface DeviceFileEntry {
    /** Bare file name, e.g. "plasma.llext". */
    name: string;
    /** The file currently exists on the FAT disk. */
    onDisk: boolean;
    /** A boot slot is associated with this file name. */
    loaded: boolean;
    /** Manifest display name, e.g. "Plasma". */
    displayName?: string;
    slot?: number;
    faulted?: boolean;
    /** This slot is the currently rendering animation. */
    active?: boolean;
    /** File was deleted this boot; the slot is parked until restart. */
    retired?: boolean;
}

// ============================================================================
// SMP Protocol Implementation
// ============================================================================

/**
 * Creates an SMP header buffer
 */
export function createSmpHeader(
    op: SmpOp,
    group: SmpGroup,
    command: number,
    dataLength: number,
    sequence: number,
    version: number = 1
): Uint8Array {
    const header = new Uint8Array(SMP_HEADER_SIZE);
    const view = new DataView(header.buffer);

    // Byte 0: Res (3 bits) | Version (2 bits) | OP (3 bits)
    header[0] = ((version & 0x03) << 3) | (op & 0x07);

    // Byte 1: Flags
    header[1] = 0;

    // Bytes 2-3: Data Length (Big Endian)
    view.setUint16(2, dataLength, false);

    // Bytes 4-5: Group ID (Big Endian)
    view.setUint16(4, group, false);

    // Byte 6: Sequence Number
    header[6] = sequence & 0xFF;

    // Byte 7: Command ID
    header[7] = command & 0xFF;

    return header;
}

/**
 * Parses an SMP header from a buffer
 */
export function parseSmpHeader(data: Uint8Array): SmpHeader {
    if (data.byteLength < SMP_HEADER_SIZE) {
        throw new Error(`SMP header too short: ${data.byteLength} bytes`);
    }

    const view = new DataView(data.buffer, data.byteOffset, data.byteLength);

    return {
        op: data[0] & 0x07,
        version: (data[0] >> 3) & 0x03,
        flags: data[1],
        length: view.getUint16(2, false),
        group: view.getUint16(4, false),
        sequence: data[6],
        command: data[7],
    };
}

/**
 * Encodes data to CBOR format
 */
export function encodeCbor(data: any): Uint8Array {
    const arrayBuffer = CBOR.encode(data);
    return new Uint8Array(arrayBuffer);
}

/**
 * Decodes CBOR data
 */
export function decodeCbor(data: Uint8Array): any {
    return CBOR.decode(data.buffer.slice(data.byteOffset, data.byteOffset + data.byteLength));
}

/**
 * Converts a base64 string to Uint8Array
 */
export function base64ToUint8Array(base64: string): Uint8Array {
    const binaryString = atob(base64);
    const bytes = new Uint8Array(binaryString.length);
    for (let i = 0; i < binaryString.length; i++) {
        bytes[i] = binaryString.charCodeAt(i);
    }
    return bytes;
}

/**
 * Converts a Uint8Array to base64 string
 */
export function uint8ArrayToBase64(bytes: Uint8Array): string {
    let binary = '';
    for (let i = 0; i < bytes.length; i++) {
        binary += String.fromCharCode(bytes[i]);
    }
    return btoa(binary);
}

/**
 * Converts a Uint8Array to hex string
 */
export function uint8ArrayToHex(bytes: Uint8Array): string {
    return Array.from(bytes)
        .map(b => b.toString(16).padStart(2, '0'))
        .join('');
}

// ============================================================================
// MCUmgr Client Class
// ============================================================================

export class McuMgrClient {
    private device: Device;
    private characteristic: Characteristic | null = null;
    private sequenceNumber: number = 0;
    private mtu: number = DEFAULT_MTU;
    private responseBuffer: Uint8Array = new Uint8Array(0);
    private expectedLength: number = 0;
    private responseResolver: ((data: Uint8Array) => void) | null = null;
    private responseRejecter: ((error: Error) => void) | null = null;
    // Sequence number of the request currently awaiting a response, so a response that
    // raced a timeout can be recognised as stale instead of resolving the next request.
    private pendingSequence: number | null = null;
    private monitorSubscription: any = null;
    private isDestroyed: boolean = false;
    // Serializes every SMP exchange. The device (and this class) can only track one
    // in-flight request at a time - responseResolver/responseRejecter are single slots,
    // not a queue keyed by sequence number. Two overlapping sendRequest() calls (e.g. the
    // firmware-update modal's "Refresh" button firing getImageState()+getSlotInfo() without
    // awaiting either) would otherwise have the second call's resolver silently clobber the
    // first's, so the first request's response either gets misrouted to the wrong promise or
    // never arrives at all - it just sits until its own 5s timeout fires
    // ("SMP request timeout after Xms"). Chaining every call onto this promise (regardless of
    // whether the previous one resolved or rejected) guarantees only one exchange is ever in
    // flight, so callers can call sendRequest()-based methods without manually sequencing them.
    private requestChain: Promise<unknown> = Promise.resolve();

    constructor(device: Device) {
        this.device = device;
    }

    /**
     * Initialize the client by discovering the SMP characteristic
     */
    async initialize(): Promise<void> {
        // Ensure services are discovered
        await this.device.discoverAllServicesAndCharacteristics();

        // Find the SMP characteristic
        const services = await this.device.services();
        for (const service of services) {
            if (service.uuid.toLowerCase() === SMP_SERVICE_UUID.toLowerCase()) {
                const characteristics = await service.characteristics();
                for (const char of characteristics) {
                    if (char.uuid.toLowerCase() === SMP_CHARACTERISTIC_UUID.toLowerCase()) {
                        this.characteristic = char;
                        break;
                    }
                }
            }
        }

        if (!this.characteristic) {
            throw new Error('SMP characteristic not found');
        }

        // Set up notifications for responses
        // Deliberately unlogged on the happy path: this fires once per BLE notification,
        // so during an OTA upload it saturates the JS log buffer within milliseconds
        // (2000/2000 entries observed, evicting everything else). Errors below still log.
        this.monitorSubscription = this.characteristic.monitor((error, char) => {
            // Ignore all callbacks if client is destroyed
            if (this.isDestroyed) {
                return;
            }

            if (error) {
                // Check if this is a disconnection error - if so, ignore it
                // The error message typically contains "Disconnected" or the device will be null
                const errorStr = error?.message || String(error);
                if (errorStr.includes('Disconnect') || errorStr.includes('disconnect')) {
                    console.log('SMP monitor: Device disconnected, stopping');
                    return;
                }

                console.error('SMP notification error:', error);
                if (this.responseRejecter) {
                    this.responseRejecter(error);
                    this.responseRejecter = null;
                    this.responseResolver = null;
                }
                return;
            }

            if (char?.value) {
                this.handleResponse(base64ToUint8Array(char.value));
            }
        });

        // Try to negotiate MTU
        try {
            const negotiatedMtu = await this.device.requestMTU(512);
            if (typeof negotiatedMtu === 'number') {
                this.mtu = negotiatedMtu - 3; // Account for ATT overhead
                console.log(`Negotiated MTU: ${negotiatedMtu}, usable: ${this.mtu}`);
            }
            // eslint-disable-next-line @typescript-eslint/no-unused-vars
        } catch (_e) {
            console.warn('MTU negotiation failed, using default:', this.mtu);
        }
    }

    /**
     * Cleanup and stop monitoring characteristic
     */
    destroy(): void {
        // Set flag first to prevent any callbacks from processing
        this.isDestroyed = true;

        if (this.monitorSubscription) {
            this.monitorSubscription.remove();
            this.monitorSubscription = null;
        }

        // Reject any pending responses
        if (this.responseRejecter) {
            this.responseRejecter(new Error('Client destroyed'));
            this.responseRejecter = null;
            this.responseResolver = null;
        }

        // Clear buffers
        this.responseBuffer = new Uint8Array(0);
        this.expectedLength = 0;
        this.pendingSequence = null;
    }

    /**
     * Handle incoming response data (may be fragmented)
     */
    private handleResponse(data: Uint8Array): void {
        //console.log(`handleResponse called with ${data.length} bytes`);

        // Append data to buffer first. This allows the first fragment to be smaller than SMP_HEADER_SIZE.
        const newBuffer = new Uint8Array(this.responseBuffer.length + data.length);
        newBuffer.set(this.responseBuffer);
        newBuffer.set(data, this.responseBuffer.length);
        this.responseBuffer = newBuffer;

        // Parse header once enough bytes are available.
        if (this.expectedLength === 0) {
            if (this.responseBuffer.length < SMP_HEADER_SIZE) {
                return;
            }

            const header = parseSmpHeader(this.responseBuffer);
            this.expectedLength = SMP_HEADER_SIZE + header.length;
            //console.log(`First fragment, expecting total ${this.expectedLength} bytes`);
        }

        //console.log(`Buffer now has ${this.responseBuffer.length}/${this.expectedLength} bytes`);

        // Check if we have the complete response
        if (this.responseBuffer.length < this.expectedLength) {
            return;  // still reassembling
        }

        const completeResponse = this.responseBuffer.slice(0, this.expectedLength);
        // ALWAYS reset before deciding what to do with the frame. A complete response
        // that we then discard (no resolver, or wrong sequence) must not be left in the
        // buffer: the next request's first fragment would be appended to it, the stale
        // expectedLength would already be satisfied, and that request's promise would
        // resolve with THIS response's bytes while its own were dropped. Worse for a
        // partial late fragment — mid-payload bytes get parsed as an SMP header and
        // poison expectedLength outright.
        this.responseBuffer = new Uint8Array(0);
        this.expectedLength = 0;

        if (!this.responseResolver) {
            // The request this belongs to already timed out and gave up.
            console.log('Discarding an SMP response that arrived with no pending request');
            return;
        }

        // The device tracks one in-flight request, but a response that raced a timeout
        // can still show up after the NEXT request was issued. parseSmpHeader gives us
        // the sequence number the firmware echoes back, so match it rather than assuming
        // whatever arrives belongs to the request currently waiting.
        const header = parseSmpHeader(completeResponse);
        if (this.pendingSequence !== null && header.sequence !== this.pendingSequence) {
            console.log(
                `Discarding stale SMP response: sequence ${header.sequence}, awaiting ${this.pendingSequence}`);
            return;
        }

        this.responseResolver(completeResponse);
        this.responseResolver = null;
        this.responseRejecter = null;
        this.pendingSequence = null;
    }

    /**
     * Send an SMP request and wait for response.
     *
     * Queues onto requestChain so overlapping calls (from any caller) are serialized into
     * one-at-a-time SMP exchanges - see the requestChain field comment for why this is required.
     */
    private sendRequest(
        op: SmpOp,
        group: SmpGroup,
        command: number,
        payload: any,
        timeout: number = 5000
    ): Promise<any> {
        // Fail fast for the common case (already destroyed when called). The doSendRequest
        // check below covers the case where destroy() runs *after* this call but before our
        // turn in requestChain comes up - see that check's comment.
        if (this.isDestroyed) {
            return Promise.reject(new Error('Client destroyed'));
        }

        const run = () => this.doSendRequest(op, group, command, payload, timeout);
        const result = this.requestChain.then(run, run);
        // Swallow rejections here so one failed request doesn't poison the chain for whatever
        // is queued after it - `result` (returned below) still carries the real outcome to the
        // original caller.
        this.requestChain = result.catch(() => undefined);
        return result;
    }

    private async doSendRequest(
        op: SmpOp,
        group: SmpGroup,
        command: number,
        payload: any,
        timeout: number = 5000
    ): Promise<any> {
        // requestChain defers this call by at least one microtask past sendRequest() (see its
        // comment), so destroy() can run in that gap. When it does, responseRejecter hasn't
        // been installed yet (we haven't reached the `new Promise` below), so destroy()'s
        // "reject any pending responses" step has nothing to reject - without this check we'd
        // instead write to a torn-down characteristic and sit out the full timeout, since
        // destroy() already removed the monitor subscription that would have delivered a
        // response. Fail fast with the same error destroy() would have rejected with.
        if (this.isDestroyed) {
            throw new Error('Client destroyed');
        }

        if (!this.characteristic) {
            throw new Error('Client not initialized');
        }

        const sequence = this.sequenceNumber++;
        const cborPayload = encodeCbor(payload);
        const header = createSmpHeader(op, group, command, cborPayload.length, sequence);

        // Combine header and payload
        const packet = new Uint8Array(header.length + cborPayload.length);
        packet.set(header);
        packet.set(cborPayload, header.length);

        this.pendingSequence = sequence & 0xFF;  // header carries one byte

        // Create response promise with proper timeout handling
        let timeoutId: ReturnType<typeof setTimeout> | null = null;
        const responsePromise = new Promise<Uint8Array>((resolve, reject) => {
            this.responseResolver = (data: Uint8Array) => {
                if (timeoutId) {
                    clearTimeout(timeoutId);
                    timeoutId = null;
                }
                resolve(data);
            };
            this.responseRejecter = (error: Error) => {
                if (timeoutId) {
                    clearTimeout(timeoutId);
                    timeoutId = null;
                }
                reject(error);
            };

            // Set timeout
            timeoutId = setTimeout(() => {
                console.log(`Timeout fired after ${timeout}ms, responseResolver exists: ${!!this.responseResolver}`);
                if (this.responseResolver) {
                    this.responseResolver = null;
                    this.responseRejecter = null;
                    this.pendingSequence = null;
                    // Drop any partial response along with the resolver: a late or
                    // fragmented response landing after this timeout must not be
                    // prepended to the NEXT request's response (handleResponse
                    // accumulates into these fields and would otherwise resolve the
                    // next exchange with a stale expectedLength and mixed bytes).
                    // handleResponse clears these too — belt and braces, because a
                    // fragment can arrive between this timeout and the next send.
                    this.responseBuffer = new Uint8Array(0);
                    this.expectedLength = 0;
                    reject(new Error(`SMP request timeout after ${timeout}ms`));
                }
            }, timeout);
            //console.log(`Timeout set for ${timeout}ms`);
        });

        // Fragment and send if necessary
        const maxPayloadSize = Math.max(1, this.mtu - 3); // Conservative estimate with floor guard
        //console.log(`Sending packet of ${packet.length} bytes, maxPayloadSize=${maxPayloadSize}`);
        for (let offset = 0; offset < packet.length; offset += maxPayloadSize) {
            const chunk = packet.slice(offset, Math.min(offset + maxPayloadSize, packet.length));
            const base64Chunk = uint8ArrayToBase64(chunk);
            //console.log(`Writing chunk at offset ${offset}, size ${chunk.length}`);
            await this.characteristic.writeWithoutResponse(base64Chunk);
            //console.log(`Chunk written`);
        }

        // Wait for response
        //console.log(`All chunks sent, awaiting response...`);
        const response = await responsePromise;
        //console.log(`Response received, length=${response.length}`);

        // Parse response - header is parsed for validation but payload is extracted by offset
        parseSmpHeader(response);
        const responsePayload = response.slice(SMP_HEADER_SIZE);

        if (responsePayload.length > 0) {
            return decodeCbor(responsePayload);
        }

        return {};
    }

    // ========================================================================
    // Image Management Commands (Group 1)
    // ========================================================================

    /**
     * Get the state of all images on the device
     */
    async getImageState(): Promise<ImageStateResponse> {
        const response = await this.sendRequest(
            SmpOp.READ_REQUEST,
            SmpGroup.IMAGE,
            ImageCmd.STATE,
            {}
        );

        throwOnSmpError(response, 'Image state error');

        return {
            images: parseImageSlots(response.images),
            splitStatus: response.splitStatus,
        };
    }

    /**
     * Set the state of an image (mark for test or confirm)
     * 
     * @param hash - SHA256 hash of the image to set state for (optional if confirming current)
     * @param confirm - If true, confirms the image; if false, marks for test
     */
    async setImageState(hash?: Uint8Array, confirm: boolean = false): Promise<ImageStateResponse> {
        const payload: any = { confirm };
        if (hash) {
            payload.hash = Uint8Array.from(hash);
        }

        const response = await this.sendRequest(
            SmpOp.WRITE_REQUEST,
            SmpGroup.IMAGE,
            ImageCmd.STATE,
            payload
        );

        throwOnSmpError(response, 'Set image state error');

        return { images: parseImageSlots(response.images) };
    }

    /**
     * Upload a firmware image to the device
     * 
     * @param imageData - The firmware image data (bin file contents)
     * @param imageIndex - The image index (usually 0)
     * @param onProgress - Progress callback
     */
    async uploadImage(
        imageData: Uint8Array,
        imageIndex: number = 0,
        onProgress?: UploadProgressCallback
    ): Promise<void> {
        // Calculate SHA256 hash of the image
        const sha256Hash = this.calculateSha256(imageData);

        await this.chunkedUpload({
            data: imageData,
            group: SmpGroup.IMAGE,
            command: ImageCmd.UPLOAD,
            reservedBytes: 64,
            errorLabel: 'Image upload',
            // A zero-length image is meaningless, and the old loop never sent a
            // packet for one - keep that.
            sendEmpty: false,
            onProgress,
            buildPayload: (offset, chunk, isFirst) => {
                const payload: any = { off: offset, data: chunk };

                // First packet includes additional fields
                if (isFirst) {
                    payload.len = imageData.length;
                    payload.image = imageIndex;
                    payload.sha = Uint8Array.from(sha256Hash);

                    // console.log(`Sending first image chunk: ${JSON.stringify(payload.data)}`);
                }

                // Check if chunk contains the specific byte sequence
                // const chunkHex = uint8ArrayToHex(chunk);
                // console.log(`Chunk offset=${offset}, size=${chunk.length}, data=${chunkHex}`);
                // if (chunkHex.includes('0d4606462ef0d0fc')) {
                //     console.log(`Found target sequence! Full payload: ${JSON.stringify({
                //         ...payload,
                //         data: chunkHex,
                //         sha: payload.sha ? uint8ArrayToHex(payload.sha) : undefined
                //     })}`);
                // }

                return payload;
            },
        });

        console.log('Image upload complete');
    }

    /**
     * The SMP chunked-transfer loop, shared by image upload and file upload.
     *
     * Both commands speak the same protocol: send a slice at an offset, read the
     * offset the device wants next (which it may rewind to recover a partial
     * transfer), and stop when the whole payload is acknowledged. Only the
     * payload fields and the per-packet overhead differ, so those are the
     * parameters - keeping one copy of the offset-recovery, stall-detection and
     * range-validation logic, which previously existed twice and had already
     * started to drift.
     */
    private async chunkedUpload(options: {
        data: Uint8Array;
        group: SmpGroup;
        command: number;
        /**
         * Per-packet overhead to hold back from the MTU: CBOR framing, plus
         * anything repeated in every request (the FS group repeats the file
         * name; the image group does not).
         */
        reservedBytes: number;
        buildPayload: (offset: number, chunk: Uint8Array, isFirst: boolean) => any;
        /** Prefix for thrown errors, e.g. "File upload". */
        errorLabel: string;
        /** Send one packet for a zero-length payload (creates/truncates a file). */
        sendEmpty: boolean;
        onProgress?: UploadProgressCallback;
    }): Promise<void> {
        const { data, group, command, reservedBytes, buildPayload, errorLabel, sendEmpty } =
            options;
        const totalLength = data.length;
        let offset = 0;
        let stalledOffsetCount = 0;

        // Floor guard: a large reservation (a long file name) must never produce
        // a zero or negative chunk, which would loop forever sending nothing.
        const maxChunkSize = Math.max(1, this.mtu - reservedBytes);

        if (totalLength === 0 && !sendEmpty) {
            return;
        }

        do {
            const chunkSize = Math.min(maxChunkSize, totalLength - offset);
            // slice() already returns a fresh Uint8Array, so the payload builders
            // must not copy it again.
            const chunk = data.slice(offset, offset + chunkSize);

            const response = await this.sendRequest(
                SmpOp.WRITE_REQUEST,
                group,
                command,
                buildPayload(offset, chunk, offset === 0),
                10000 // Longer timeout for uploads
            );

            throwOnSmpError(response, `${errorLabel} error at offset ${offset}`);

            // Server may respond with a different offset (e.g., to continue broken upload)
            const nextOffset = response.off !== undefined ? response.off : offset + chunkSize;
            if (nextOffset < 0 || nextOffset > totalLength) {
                throw new Error(
                    `${errorLabel} error: invalid offset ${nextOffset} (total=${totalLength})`
                );
            }

            if (nextOffset <= offset && totalLength > 0) {
                stalledOffsetCount += 1;
                if (stalledOffsetCount >= 3) {
                    throw new Error(`${errorLabel} stalled at offset ${offset}`);
                }
            } else {
                stalledOffsetCount = 0;
            }

            offset = nextOffset;

            options.onProgress?.(offset, totalLength);
        } while (offset < totalLength);
    }

    /**
     * Calculate SHA256 hash of data
     */
    private calculateSha256(data: Uint8Array): Uint8Array {
        // Use pure JS implementation to avoid native bridge issues
        const hash = sha256.array(data);
        return new Uint8Array(hash);
    }

    /**
     * Erase the secondary image slot
     * 
     * @param slot - Slot number to erase (default: 1 = secondary)
     */
    async eraseImage(slot: number = 1): Promise<void> {
        const response = await this.sendRequest(
            SmpOp.WRITE_REQUEST,
            SmpGroup.IMAGE,
            ImageCmd.ERASE,
            { slot },
            120000 // Very long timeout - erase can take a while
        );

        throwOnSmpError(response, 'Image erase error');
    }

    /**
     * Get slot information
     */
    async getSlotInfo(): Promise<SlotInfoResponse> {
        const response = await this.sendRequest(
            SmpOp.READ_REQUEST,
            SmpGroup.IMAGE,
            ImageCmd.SLOT_INFO,
            {}
        );

        throwOnSmpError(response, 'Slot info error');

        return response;
    }

    // ========================================================================
    // OS Management Commands (Group 0)
    // ========================================================================

    /**
     * Echo command - useful for testing connectivity
     */
    async echo(message: string): Promise<string> {
        const response = await this.sendRequest(
            SmpOp.WRITE_REQUEST,
            SmpGroup.OS,
            OsCmd.ECHO,
            { d: message }
        );

        return response.r || '';
    }

    /**
     * Reset the device
     */
    async reset(): Promise<void> {
        await this.sendRequest(
            SmpOp.WRITE_REQUEST,
            SmpGroup.OS,
            OsCmd.RESET,
            {},
            5000 // Short timeout - device will reset and won't respond
        ).catch(() => {
            // Expected - device resets before responding
            console.log('Reset command sent');
        });
    }

    /**
     * Get MCUmgr parameters (buffer size, count)
     */
    async getMcuMgrParams(): Promise<{ buf_size: number; buf_count: number }> {
        const response = await this.sendRequest(
            SmpOp.READ_REQUEST,
            SmpGroup.OS,
            OsCmd.MCUMGR_PARAMS,
            {}
        );

        return {
            buf_size: response.buf_size || 0,
            buf_count: response.buf_count || 0,
        };
    }

    /**
     * Get OS/Application info from the device.
     * @param format - Format string controlling what info to return. Use "i" for board name.
     * @returns The output string from the device (e.g. "rgb_sunglasses_proto0_nrf5340_cpuapp")
     */
    async getOsInfo(format: string = 'i'): Promise<string> {
        const response = await this.sendRequest(
            SmpOp.READ_REQUEST,
            SmpGroup.OS,
            OsCmd.INFO,
            { format }
        );

        throwOnSmpError(response, 'OS info error');

        return response.output ?? '';
    }

    // ========================================================================
    // FS Management Commands (Group 8)
    // ========================================================================

    async getFileHash(fileName: string): Promise<FileHashResponse> {
        const response = await this.sendRequest(
            SmpOp.READ_REQUEST,
            SmpGroup.FS,
            FsCmd.HASH,
            { name: fileName, type: 'sha256' }
        );
        throwOnSmpError(response, 'File hash error');

        return response;
    }

    /**
     * SHA256 of a file on the device as a lowercase hex string, or null if the
     * file does not exist.
     *
     * The null case is a normal outcome, not a failure: it's how extension sync
     * distinguishes "install this" from "replace this". Every other error still
     * throws.
     */
    async getFileSha256(fileName: string): Promise<string | null> {
        let response: FileHashResponse;
        try {
            response = await this.getFileHash(fileName);
        } catch (e: unknown) {
            if (isSmpGroupError(e, SmpGroup.FS, FsMgmtError.FILE_NOT_FOUND)) {
                return null;
            }
            throw e;
        }

        if (response.type !== 'sha256') {
            throw new Error(`Expected a sha256 hash, device returned '${response.type}'`);
        }

        // CBOR byte strings decode to an ArrayBuffer or a typed array depending on
        // the decoder; normalise both. A number here means the device answered
        // with a checksum (CRC32) rather than a hash, which the type check above
        // has already ruled out - but be explicit rather than producing garbage
        // hex from a numeric value.
        const output = response.output;
        if (typeof output === 'number') {
            throw new Error('Device returned a numeric checksum where a sha256 hash was expected');
        }

        const bytes = output instanceof Uint8Array ? output : new Uint8Array(output);
        return uint8ArrayToHex(bytes);
    }

    /**
     * Write a file to the device's filesystem, creating or truncating it.
     *
     * Chunking mirrors uploadImage(), with one difference that matters: the FS
     * upload command repeats `name` in EVERY request (the image upload doesn't),
     * so the file name's length has to come out of each chunk's byte budget or
     * a long path silently pushes packets past the MTU.
     */
    async uploadFile(
        fileName: string,
        fileData: Uint8Array,
        onProgress?: UploadProgressCallback
    ): Promise<void> {
        await this.chunkedUpload({
            data: fileData,
            group: SmpGroup.FS,
            command: FsCmd.DOWNLOAD_UPLOAD,
            // The file name is repeated in EVERY request (unlike image upload's
            // first-packet-only fields), so its length comes out of each chunk's
            // budget or a long path silently pushes packets past the MTU.
            reservedBytes: 64 + fileName.length,
            errorLabel: 'File upload',
            // A zero-length file still needs one request, to create/truncate it.
            sendEmpty: true,
            onProgress,
            buildPayload: (offset, chunk, isFirst) => {
                const payload: any = { off: offset, name: fileName, data: chunk };

                // The total length is only sent with the first packet; the device
                // uses it to know when the transfer is complete.
                if (isFirst) {
                    payload.len = fileData.length;
                }
                return payload;
            },
        });
    }

    /**
     * Close whatever file handle the device's fs_mgmt is still holding open.
     *
     * fs_mgmt keeps the last upload/download handle open for an idle window, and
     * FatFs here is compiled without file locking (FF_FS_LOCK=0), so a delete
     * racing that lingering handle could corrupt the FAT. Deleting always closes
     * first; combined with this client's fully serialized request chain that
     * removes the race from this app instance. (Another SMP client mid-upload is
     * a residual, accepted risk — see fw/docs/extension-management.md §5.)
     *
     * Best-effort by design: a device-side error (nothing open, old firmware)
     * must not turn a perfectly safe delete into a failure, so only transport
     * errors propagate.
     */
    async closeOpenedFile(): Promise<void> {
        try {
            const response = await this.sendRequest(
                SmpOp.WRITE_REQUEST,
                SmpGroup.FS,
                FsCmd.CLOSE,
                {}
            );
            throwOnSmpError(response, 'Close opened file error');
        } catch (e: unknown) {
            if (e instanceof SmpCommandError) {
                return;
            }
            throw e;
        }
    }

    // ========================================================================
    // FILE_MGMT Commands (Group 64) — this firmware's own file management
    // ========================================================================

    /**
     * List every file of `kind` the device knows about: the union of the fenced
     * directory's disk contents and the boot slot registry, so files uploaded
     * since boot and files deleted-but-still-loaded both appear (see
     * DeviceFileEntry).
     *
     * Follows the firmware's `off` continuation automatically — LIST is
     * paginated (page max 8) because the response has to fit one SMP buffer.
     *
     * Firmware without the group rejects this with a group-less SMP error
     * (MGMT_ERR_ENOTSUP as a bare `rc`); callers use that to hide management
     * affordances rather than showing buttons that always fail.
     */
    async listDeviceFiles(kind: DeviceFileKind = 'ext'): Promise<DeviceFileEntry[]> {
        const entries: DeviceFileEntry[] = [];
        let off: number | undefined;

        // The device caps `off` by construction (it walks a bounded directory),
        // but a defensive page cap keeps a buggy/hostile peer from looping us
        // forever: 64 pages × 8 entries is far beyond any real disk here.
        for (let page = 0; page < 64; page++) {
            const request: any = { kind };
            if (off !== undefined) {
                request.off = off;
            }
            const response = await this.sendRequest(
                SmpOp.READ_REQUEST,
                SmpGroup.FILE_MGMT,
                FileMgmtCmd.LIST,
                request
            );
            throwOnSmpError(response, 'File list error');

            for (const raw of response.entries ?? []) {
                entries.push({
                    name: raw.n,
                    onDisk: !!raw.disk,
                    loaded: !!raw.loaded,
                    displayName: raw.d,
                    slot: raw.s,
                    faulted: raw.f,
                    active: raw.a,
                    retired: raw.r,
                });
            }

            if (response.off === undefined) {
                return entries;
            }
            off = response.off;
        }
        throw new Error('Device kept returning LIST continuations past any plausible directory size');
    }

    /**
     * Delete a file of `kind` by bare name (never a path — the device builds and
     * fences the path itself).
     *
     * On the device this is more than an unlink: if the file backs the active
     * animation it switches away first, and the matching boot slot is retired
     * (activation rejected until restart) with its persisted settings purged.
     *
     * Always closes any lingering fs_mgmt handle first — see closeOpenedFile().
     *
     * Throws SmpCommandError with group=64 and a FileMgmtError rc for
     * device-side refusals (NOT_FOUND, INVALID_NAME, ...).
     */
    async deleteDeviceFile(name: string, kind: DeviceFileKind = 'ext'): Promise<void> {
        await this.closeOpenedFile();
        const response = await this.sendRequest(
            SmpOp.WRITE_REQUEST,
            SmpGroup.FILE_MGMT,
            FileMgmtCmd.DELETE,
            { kind, name }
        );
        throwOnSmpError(response, `Delete ${name} error`);
    }

    // ========================================================================
    // High-Level Firmware Update API
    // ========================================================================

    /**
     * Perform a complete firmware update
     * 
     * This is a high-level function that:
     * 1. Erases the secondary slot (optional)
     * 2. Uploads the new firmware image
     * 3. Marks the image for test boot
     * 4. Resets the device to boot into the new firmware
     * 
     * @param imageData - The firmware image data
     * @param options - Update options
     */
    async performFirmwareUpdate(
        imageData: Uint8Array,
        options: {
            imageIndex?: number;
            eraseFirst?: boolean;
            markForTest?: boolean;
            resetAfterUpload?: boolean;
            onProgress?: UploadProgressCallback;
            onStatus?: (status: string) => void;
        } = {}
    ): Promise<void> {
        const {
            imageIndex = 0,
            eraseFirst = true,
            markForTest = true,
            resetAfterUpload = true,
            onProgress,
            onStatus,
        } = options;

        try {
            // Step 1: Optionally erase the secondary slot
            if (eraseFirst) {
                onStatus?.('Erasing secondary slot...');
                await this.eraseImage(1);
            }

            // Step 2: Upload the firmware image
            onStatus?.('Uploading firmware...');
            await this.uploadImage(imageData, imageIndex, onProgress);

            // Step 3: Get the uploaded image info and mark for test
            if (markForTest) {
                onStatus?.('Marking image for test boot...');
                const state = await this.getImageState();

                // Find the uploaded image in secondary slot
                const uploadedImage = state.images.find(
                    img => img.slot === 1 && (img.image === imageIndex || img.image === undefined)
                );

                if (uploadedImage?.hash) {
                    await this.setImageState(uploadedImage.hash, false);
                }
            }

            // Step 4: Reset the device
            if (resetAfterUpload) {
                onStatus?.('Resetting device...');
                await this.reset();
            }

            onStatus?.('Firmware update complete!');
        } catch (error) {
            onStatus?.(`Update failed: ${error}`);
            throw error;
        }
    }

    /**
     * Confirm the currently running image (make it permanent)
     */
    async confirmCurrentImage(): Promise<void> {
        await this.setImageState(undefined, true);
    }
}

// ============================================================================
// Utility Functions
// ============================================================================

/**
 * Check SMP response for errors and throw if present.
 *
 * Throws SmpCommandError (an Error subclass) so a caller that needs to branch on
 * a specific failure - e.g. extension sync treating FS group "file not found" as
 * "install it" rather than an error - can read `group`/`rc` instead of parsing
 * the message. The message strings are unchanged, so the many call sites that
 * just surface `e.message` behave exactly as before.
 */
function throwOnSmpError(response: any, label: string): void {
    if (response.rc && response.rc !== 0) {
        // Legacy (SMP version 0) error shape: a bare `rc` with no group, which
        // for a non-zero value is an mcumgr_err_t, not a group-specific code.
        throw new SmpCommandError(`${label}: rc=${response.rc}`, response.rc, undefined);
    }
    if (response.err) {
        throw new SmpCommandError(
            `${label}: group=${response.err.group}, rc=${response.err.rc}`,
            response.err.rc,
            response.err.group
        );
    }
}

/**
 * Returns true if `error` came from the given management group - the SMP
 * version 1 `{err: {group, rc}}` shape - optionally also matching a specific
 * return code. Omit `rc` to match any failure attributable to that group.
 *
 * A bare `rc` is deliberately NOT matched: it carries no group, so a
 * group-specific code like FS "file not found" (3) would be indistinguishable
 * from the generic mcumgr EINVAL (3). That also means a group-less failure
 * (a transport error, or ENOTSUP from firmware that doesn't implement the
 * group at all) never looks like a per-request problem.
 */
export function isSmpGroupError(error: unknown, group: SmpGroup, rc?: number): boolean {
    if (!(error instanceof SmpCommandError) || error.group !== group) {
        return false;
    }
    return rc === undefined || error.rc === rc;
}

/**
 * Parse image slots from SMP response
 */
function parseImageSlots(images: any[]): ImageSlot[] {
    return (images || []).map((img: any) => ({
        image: img.image,
        slot: img.slot,
        version: img.version,
        hash: img.hash ? new Uint8Array(img.hash) : undefined,
        bootable: img.bootable,
        pending: img.pending,
        confirmed: img.confirmed,
        active: img.active,
        permanent: img.permanent,
    }));
}

/**
 * Parse a firmware image header to extract version info
 * MCUboot image format: https://docs.mcuboot.com/design.html#image-format
 */
export function parseImageHeader(data: Uint8Array): {
    magic: number;
    version: string;
    imageSize: number;
} | null {
    if (data.length < 32) {
        return null;
    }

    const view = new DataView(data.buffer, data.byteOffset, data.byteLength);

    // MCUboot magic number: 0x96f3b83d
    const magic = view.getUint32(0, true);
    if (magic !== 0x96f3b83d) {
        return null;
    }

    const imageSize = view.getUint32(12, true);
    const versionMajor = data[20];
    const versionMinor = data[21];
    const versionRevision = view.getUint16(22, true);
    const versionBuild = view.getUint32(24, true);

    return {
        magic,
        version: `${versionMajor}.${versionMinor}.${versionRevision}+${versionBuild}`,
        imageSize,
    };
}

/** MCUboot TLV type for the image's own SHA256 digest (`IMAGE_TLV_SHA256`). */
const IMAGE_TLV_SHA256 = 0x10;
/** Magic of the unprotected TLV area (`IMAGE_TLV_INFO_MAGIC`). */
const IMAGE_TLV_INFO_MAGIC = 0x6907;
/** Magic of the protected TLV area, which precedes the unprotected one when present. */
const IMAGE_TLV_PROT_INFO_MAGIC = 0x6908;

/**
 * Extract the image's own SHA256 digest from its MCUboot TLV trailer.
 *
 * This is the value the device reports as a slot's `hash` in `getImageState()`, so it
 * is what a post-update verification must compare against. It is emphatically NOT
 * `sha256(whole .bin)` — the file includes the TLV trailer, which the digest itself
 * cannot cover. Verified against the real fw-v2.1.0 artifact: the TLV reads
 * `eeacf0fa…`, matching what the device reported for the staged slot, while the
 * whole-file digest is `9f5d7d3a…`.
 *
 * Layout (docs.mcuboot.com/design.html#image-format): header (`hdr_size`), payload
 * (`img_size`), then optionally a protected TLV area, then the unprotected TLV area.
 * Each area starts with `{magic:u16, total_len:u16}` and holds `{type:u16, len:u16,
 * value}` records, all little-endian.
 *
 * @returns lowercase hex digest, or null if the image has no parseable SHA256 TLV.
 */
export function parseImageSha256(data: Uint8Array): string | null {
    if (data.length < 32) return null;

    const view = new DataView(data.buffer, data.byteOffset, data.byteLength);
    if (view.getUint32(0, true) !== 0x96f3b83d) return null;

    const headerSize = view.getUint16(8, true);
    const imageSize = view.getUint32(12, true);

    let offset = headerSize + imageSize;
    // Walk at most two areas: protected (optional) then unprotected.
    for (let area = 0; area < 2; area++) {
        if (offset + 4 > data.length) return null;

        const magic = view.getUint16(offset, true);
        const totalLen = view.getUint16(offset + 2, true);
        if (magic !== IMAGE_TLV_INFO_MAGIC && magic !== IMAGE_TLV_PROT_INFO_MAGIC) {
            return null;
        }

        const areaEnd = Math.min(offset + totalLen, data.length);
        let cursor = offset + 4;
        while (cursor + 4 <= areaEnd) {
            const type = view.getUint16(cursor, true);
            const len = view.getUint16(cursor + 2, true);
            const valueStart = cursor + 4;
            if (valueStart + len > data.length) return null;
            if (type === IMAGE_TLV_SHA256 && len === 32) {
                return uint8ArrayToHex(data.subarray(valueStart, valueStart + len));
            }
            cursor = valueStart + len;
        }

        // Not in this area — the protected area is followed by the unprotected one.
        if (magic !== IMAGE_TLV_PROT_INFO_MAGIC) return null;
        offset = areaEnd;
    }

    return null;
}

/**
 * Format bytes as human-readable size
 */
export function formatBytes(bytes: number): string {
    if (bytes < 1024) return `${bytes} B`;
    if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
    return `${(bytes / (1024 * 1024)).toFixed(2)} MB`;
}

/**
 * Format a hash as hex string for display
 */
export function formatHash(hash: Uint8Array | undefined): string {
    if (!hash) return 'N/A';
    return uint8ArrayToHex(hash.slice(0, 8)) + '...';
}
