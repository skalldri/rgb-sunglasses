/**
 * MCUboot BLE Updater Client
 *
 * Drives the custom bootloader update protocol implemented in
 * fw/src/bluetooth/mcuboot_updater_service.cpp.
 *
 * Protocol summary:
 *   Control characteristic (write with response):
 *     0x05              — Unlock (LOCKED → IDLE)
 *     0x01 <size:u32LE> — Begin upload (5 bytes)
 *     0x02              — Validate
 *     0x03              — Commit (flashes + reboots)
 *     0x04              — Abort (returns to LOCKED)
 *
 *   Data characteristic (write without response):
 *     Raw binary package chunks, ≤244 bytes each
 *
 *   Status characteristic (read + notify):
 *     4 bytes: [state:u8, progress:u8, error:u8, flash_unlocked:u8]
 *
 *   0x06 — RequestUpdaterReboot: sets boot-mode=0xB1, device reboots in ~200 ms.
 *     After reboot, MCUboot's fprotect_hook skips flash protection and sets
 *     boot-mode=0xB2; the app reads flash_unlocked=1 to confirm.
 */

import { Characteristic, Device, Subscription } from 'react-native-ble-plx';
import {
    UUID_MCUBOOT_UPDATER_CONTROL,
    UUID_MCUBOOT_UPDATER_DATA,
    UUID_MCUBOOT_UPDATER_SERVICE,
    UUID_MCUBOOT_UPDATER_STATUS,
} from '@/constants/bluetooth';

// ============================================================================
// Types
// ============================================================================

export enum McubootUpdaterState {
    LOCKED     = 0,
    IDLE       = 1,
    ERASING    = 2,
    RECEIVING  = 3,
    VALIDATING = 4,
    VALIDATED  = 5,
    FLASHING   = 6,
    DONE       = 7,
    ERROR      = 8,
}

export interface McubootUpdaterStatus {
    state: McubootUpdaterState;
    progress: number;       // 0-100
    errorCode: number;
    flashUnlocked: boolean; // true if MCUboot skipped fprotect this boot (BOOT_MODE_UPDATER_ACTIVE was set)
}

export interface McubootPackageInfo {
    major: number;
    minor: number;
    revision: number;
    payloadSize: number;
    crc32: number;
    payload: Uint8Array;
}

// ============================================================================
// Constants
// ============================================================================

export const MCUBOOT_PKG_MAGIC       = 0x424D5247;  // "GRMB" little-endian
export const MCUBOOT_PKG_HEADER_SIZE = 16;

// Max chunk size: negotiated MTU (247) - 3 bytes ATT overhead = 244.
// Must be a multiple of 4 for QSPI write-word alignment.
export const MCUBOOT_CHUNK_SIZE = 244;

// Control command bytes (must match mcuboot_updater_service.cpp)
const CMD_UNLOCK                 = 0x05;
const CMD_BEGIN                  = 0x01;
const CMD_VALIDATE               = 0x02;
const CMD_COMMIT                 = 0x03;
const CMD_ABORT                  = 0x04;
const CMD_REQUEST_UPDATER_REBOOT = 0x06;

// ============================================================================
// Package parsing
// ============================================================================

/** IEEE 802.3 CRC32 — same result as Python's zlib.crc32() and Zephyr's crc32_ieee(). */
export function computeCrc32(data: Uint8Array): number {
    let crc = 0xFFFFFFFF;
    for (let i = 0; i < data.length; i++) {
        crc ^= data[i];
        for (let j = 0; j < 8; j++) {
            crc = (crc & 1) ? ((crc >>> 1) ^ 0xEDB88320) : (crc >>> 1);
        }
    }
    return (crc ^ 0xFFFFFFFF) >>> 0;
}

/**
 * Parse and validate a packaged MCUboot binary (produced by fw/tools/package_mcuboot.py).
 * Throws a descriptive error if validation fails.
 */
export function parseMcubootPackage(data: Uint8Array): McubootPackageInfo {
    if (data.length < MCUBOOT_PKG_HEADER_SIZE) {
        throw new Error(
            `File too small: ${data.length} bytes (minimum ${MCUBOOT_PKG_HEADER_SIZE})`
        );
    }

    const view = new DataView(data.buffer, data.byteOffset, data.byteLength);
    const magic        = view.getUint32(0, true);
    const major        = data[4];
    const minor        = data[5];
    const revision     = view.getUint16(6, true);
    const payloadSize  = view.getUint32(8, true);
    const storedCrc32  = view.getUint32(12, true);

    if (magic !== MCUBOOT_PKG_MAGIC) {
        throw new Error(
            `Invalid magic: 0x${magic.toString(16).padStart(8, '0')} ` +
            `(expected 0x${MCUBOOT_PKG_MAGIC.toString(16).padStart(8, '0')}). ` +
            `Is this a MCUboot package built with fw/tools/package_mcuboot.py?`
        );
    }

    if (data.length !== MCUBOOT_PKG_HEADER_SIZE + payloadSize) {
        throw new Error(
            `Size mismatch: header declares ${payloadSize} payload bytes, ` +
            `but file has ${data.length - MCUBOOT_PKG_HEADER_SIZE} bytes after the header`
        );
    }

    const payload = data.slice(MCUBOOT_PKG_HEADER_SIZE);
    const computedCrc = computeCrc32(payload);
    if (computedCrc !== storedCrc32) {
        throw new Error(
            `CRC32 mismatch: stored=0x${storedCrc32.toString(16).padStart(8, '0')}, ` +
            `computed=0x${computedCrc.toString(16).padStart(8, '0')}. File may be corrupt.`
        );
    }

    return { major, minor, revision, payloadSize, crc32: storedCrc32, payload };
}

// ============================================================================
// Base64 helpers
// ============================================================================

function uint8ArrayToBase64(data: Uint8Array): string {
    let binary = '';
    for (let i = 0; i < data.length; i++) {
        binary += String.fromCharCode(data[i]);
    }
    return btoa(binary);
}

function base64ToUint8Array(base64: string): Uint8Array {
    const binary = atob(base64);
    const bytes = new Uint8Array(binary.length);
    for (let i = 0; i < binary.length; i++) {
        bytes[i] = binary.charCodeAt(i);
    }
    return bytes;
}

// ============================================================================
// Client class
// ============================================================================

export class McubootUpdaterClient {
    private statusChar:  Characteristic | null = null;
    private dataChar:    Characteristic | null = null;
    private controlChar: Characteristic | null = null;
    private subscription: Subscription | null = null;

    private statusHandler: ((s: McubootUpdaterStatus) => void) | null = null;
    // Map from awaited state → {resolve, reject} for waitForState()
    private pendingWaiters: Map<
        McubootUpdaterState,
        { resolve: () => void; reject: (err: Error) => void }
    > = new Map();

    /** Discover the MCUboot updater service and subscribe to status notifications. */
    async initialize(device: Device): Promise<void> {
        const services = await device.services();
        let svc = services.find(
            s => s.uuid.toLowerCase() === UUID_MCUBOOT_UPDATER_SERVICE.toLowerCase()
        );
        if (!svc) {
            throw new Error('MCUboot updater service not found on device. Is this a Proto0 board?');
        }

        const chars = await svc.characteristics();
        for (const c of chars) {
            const u = c.uuid.toLowerCase();
            if (u === UUID_MCUBOOT_UPDATER_STATUS.toLowerCase())  this.statusChar  = c;
            if (u === UUID_MCUBOOT_UPDATER_DATA.toLowerCase())    this.dataChar    = c;
            if (u === UUID_MCUBOOT_UPDATER_CONTROL.toLowerCase()) this.controlChar = c;
        }

        if (!this.statusChar || !this.dataChar || !this.controlChar) {
            throw new Error('MCUboot updater service is missing one or more characteristics');
        }

        // Seed the real current state immediately. Without this, the client only learns the
        // device's state from the next notification, which never fires on a plain reconnect
        // (nothing changed since boot) — leaving the UI stuck on stale defaults (e.g.
        // flashUnlocked=false) even when the device is genuinely already unlocked.
        const initial = await this.statusChar.read();
        if (initial.value) this.handleStatusValue(initial.value);

        this.subscription = this.statusChar.monitor((err, char) => {
            if (err || !char?.value) return;
            this.handleStatusValue(char.value);
        });
    }

    private handleStatusValue(base64Value: string): void {
        const bytes = base64ToUint8Array(base64Value);
        if (bytes.length < 3) return;

        const status: McubootUpdaterStatus = {
            state:         bytes[0] as McubootUpdaterState,
            progress:      bytes[1],
            errorCode:     bytes[2],
            flashUnlocked: bytes.length >= 4 ? bytes[3] !== 0 : false,
        };

        this.statusHandler?.(status);
        this.dispatchToWaiters(status);
    }

    /** Register a handler called on every status notification. */
    onStatusChanged(handler: (s: McubootUpdaterStatus) => void): void {
        this.statusHandler = handler;
    }

    private dispatchToWaiters(status: McubootUpdaterStatus): void {
        // Always reject all waiters if we land in ERROR state
        if (status.state === McubootUpdaterState.ERROR) {
            for (const [, waiter] of this.pendingWaiters) {
                waiter.reject(new Error(`Device entered ERROR state (errorCode=${status.errorCode})`));
            }
            this.pendingWaiters.clear();
            return;
        }

        const waiter = this.pendingWaiters.get(status.state);
        if (waiter) {
            this.pendingWaiters.delete(status.state);
            waiter.resolve();
        }
    }

    private waitForState(state: McubootUpdaterState, timeoutMs: number): Promise<void> {
        return new Promise((resolve, reject) => {
            const timer = setTimeout(() => {
                this.pendingWaiters.delete(state);
                reject(new Error(`Timeout waiting for updater state ${McubootUpdaterState[state]}`));
            }, timeoutMs);

            this.pendingWaiters.set(state, {
                resolve: () => { clearTimeout(timer); resolve(); },
                reject:  (err) => { clearTimeout(timer); reject(err); },
            });
        });
    }

    private async writeControl(payload: Uint8Array): Promise<void> {
        if (!this.controlChar) throw new Error('Client not initialized');
        await this.controlChar.writeWithResponse(uint8ArrayToBase64(payload));
    }

    /** Unlock the updater (LOCKED → IDLE). */
    async unlock(): Promise<void> {
        const waiter = this.waitForState(McubootUpdaterState.IDLE, 5000);
        await this.writeControl(new Uint8Array([CMD_UNLOCK]));
        await waiter;
    }

    /** Signal the start of an upload. Waits for RECEIVING (staging erase takes ~2-3 s). */
    async beginUpload(payloadSize: number): Promise<void> {
        const cmd = new Uint8Array(5);
        cmd[0] = CMD_BEGIN;
        new DataView(cmd.buffer).setUint32(1, payloadSize, true);
        const waiter = this.waitForState(McubootUpdaterState.RECEIVING, 30000);
        await this.writeControl(cmd);
        await waiter;
    }

    /** Send one chunk of the package binary (header + payload, sequentially). */
    async sendChunk(chunk: Uint8Array): Promise<void> {
        if (!this.dataChar) throw new Error('Client not initialized');
        await this.dataChar.writeWithoutResponse(uint8ArrayToBase64(chunk));
    }

    /** Trigger firmware-side CRC32 validation. Waits for VALIDATED. */
    async validate(): Promise<void> {
        const waiter = this.waitForState(McubootUpdaterState.VALIDATED, 15000);
        await this.writeControl(new Uint8Array([CMD_VALIDATE]));
        await waiter;
    }

    /**
     * Flash the validated binary and reboot. Waits for DONE.
     * After DONE, the device reboots and the BLE connection drops.
     */
    async commit(): Promise<void> {
        const waiter = this.waitForState(McubootUpdaterState.DONE, 15000);
        await this.writeControl(new Uint8Array([CMD_COMMIT]));
        await waiter;
    }

    /** Abort and return the updater to LOCKED state. */
    async abort(): Promise<void> {
        await this.writeControl(new Uint8Array([CMD_ABORT]));
    }

    /**
     * Set boot-mode to UPDATER_REQ and reboot.
     * The device will reboot in ~200 ms after the ATT write response is sent.
     * After reboot, MCUboot's fprotect_hook skips flash protection and sets
     * boot-mode=UPDATER_ACTIVE, which the app reads back as flashUnlocked=true.
     * The BLE connection will drop when the device reboots — do NOT await a
     * state change; the caller should handle the disconnect event instead.
     */
    async requestUpdaterReboot(): Promise<void> {
        await this.writeControl(new Uint8Array([CMD_REQUEST_UPDATER_REBOOT]));
    }

    /** Unsubscribe from status notifications. Call on component unmount. */
    destroy(): void {
        this.subscription?.remove();
        this.subscription = null;
        for (const [, waiter] of this.pendingWaiters) {
            waiter.reject(new Error('McubootUpdaterClient destroyed'));
        }
        this.pendingWaiters.clear();
    }
}
