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
        this.monitorSubscription = this.characteristic.monitor((error, char) => {
            console.log('SMP monitor called!');

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
        if (this.responseBuffer.length >= this.expectedLength && this.responseResolver) {
            //console.log(`Response complete, resolving promise`);
            const completeResponse = this.responseBuffer.slice(0, this.expectedLength);
            this.responseBuffer = new Uint8Array(0);
            this.expectedLength = 0;
            this.responseResolver(completeResponse);
            this.responseResolver = null;
            this.responseRejecter = null;
        } else if (this.responseBuffer.length >= this.expectedLength) {
            console.log(`Response complete but no resolver!`);
        }
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
        const totalLength = imageData.length;
        let offset = 0;
        let stalledOffsetCount = 0;

        // Calculate SHA256 hash of the image
        const sha256Hash = this.calculateSha256(imageData);

        while (offset < totalLength) {
            // Calculate chunk size (leave room for CBOR overhead)
            const maxChunkSize = Math.max(1, this.mtu - 64); // Conservative chunk size with floor guard
            const chunkSize = Math.min(maxChunkSize, totalLength - offset);
            const chunk = imageData.slice(offset, offset + chunkSize);

            const payload: any = {
                off: offset,
                data: Uint8Array.from(chunk),
            };

            // First packet includes additional fields
            if (offset === 0) {
                payload.len = totalLength;
                payload.image = imageIndex;
                payload.sha = Uint8Array.from(sha256Hash);

                // console.log(`Sending first image chunk: ${JSON.stringify(payload.data)}`);
            }

            // Check if chunk contains the specific byte sequence
            // const chunkHex = uint8ArrayToHex(chunk);
            // console.log(`Chunk offset=${offset}, size=${chunkSize}, data=${chunkHex}`);
            // if (chunkHex.includes('0d4606462ef0d0fc')) {
            //     console.log(`Found target sequence! Full payload: ${JSON.stringify({
            //         ...payload,
            //         data: chunkHex,
            //         sha: payload.sha ? uint8ArrayToHex(payload.sha) : undefined
            //     })}`);
            // }

            //console.log(`Sending image chunk: offset=${offset}, size=${chunkSize}`);
            const response = await this.sendRequest(
                SmpOp.WRITE_REQUEST,
                SmpGroup.IMAGE,
                ImageCmd.UPLOAD,
                payload,
                10000 // Longer timeout for uploads
            );
            //console.log(`Send complete`);

            throwOnSmpError(response, `Image upload error at offset ${offset}`);

            // Server may respond with a different offset (e.g., to continue broken upload)
            const nextOffset = response.off !== undefined ? response.off : offset + chunkSize;
            if (nextOffset < 0 || nextOffset > totalLength) {
                throw new Error(`Image upload error: invalid offset ${nextOffset} (total=${totalLength})`);
            }

            if (nextOffset <= offset) {
                stalledOffsetCount += 1;
                if (stalledOffsetCount >= 3) {
                    throw new Error(`Image upload stalled at offset ${offset}`);
                }
            } else {
                stalledOffsetCount = 0;
            }

            // console.log(`Server asked for a new offset! ${nextOffset}`);
            offset = nextOffset;

            if (onProgress) {
                onProgress(offset, totalLength);
            }
        }

        console.log('Image upload complete');
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
 * Check SMP response for errors and throw if present
 */
function throwOnSmpError(response: any, label: string): void {
    if (response.rc && response.rc !== 0) {
        throw new Error(`${label}: rc=${response.rc}`);
    }
    if (response.err) {
        throw new Error(`${label}: group=${response.err.group}, rc=${response.err.rc}`);
    }
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
