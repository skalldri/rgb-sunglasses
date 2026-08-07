import { useBluetooth } from '@/context/bluetooth-context';
import { useMcuMgrClientContext } from '@/context/mcumgr-client-context';
import {
    calculateOverallUploadProgress,
    findUploadedImageForIndex,
    FirmwarePackage,
    parseFirmwareImageIndex,
} from '@/services/firmware-package';
import { ImageSlot, parseImageSha256, uint8ArrayToHex } from '@/services/mcumgr';
import { useCallback, useEffect, useRef, useState } from 'react';

/**
 * Steps of the end-to-end update. The order here is the order the user sees.
 *
 * `rebooting` and `reconnecting` are the two states during which the BLE link is
 * legitimately gone — screens must render them as progress, never as an error.
 */
export type FlowStep =
    | 'loadingSource'
    | 'ready'
    | 'uploading'
    | 'staging'
    | 'staged'
    | 'rebooting'
    | 'reconnecting'
    | 'verifying'
    | 'success'
    | 'failed';

/** Per-image outcome, used to drive the yellow → green card state. */
export interface ImageProgress {
    imageIndex: number;
    file: string;
    /** Version from the image's own MCUboot header, or the manifest. */
    version: string;
    uploaded: boolean;
    /**
     * The image's own SHA256, read out of its MCUboot TLV trailer *in the zip*.
     * This is the verification reference: it says what SHOULD be running, independent
     * of anything the device reports. Null only if the TLV can't be parsed.
     */
    expectedHash?: string;
    /** Hash the device reported for the staged slot-1 image (the fallback reference). */
    stagedHash?: string;
    staged: boolean;
    verified: boolean;
}

/**
 * How long to wait for the board to come back after a reboot before offering an
 * escape hatch. An MCUboot swap of a ~663 KB app image plus a ~171 KB net-core image
 * is slow, and this is deliberately generous: hitting it shows a "still waiting"
 * message, it does NOT fail the update.
 */
export const RECONNECT_PATIENCE_MS = 180_000;

/**
 * The image index whose hash survives installation.
 *
 * On this SoC image 0 is the application core — the one the bootloader flashes into
 * its own slot verbatim. Image 1 is the network core, delivered as a wrapper the app
 * core unwraps over IPC, so its installed hash differs from the file's. See the table
 * on `useFirmwareUpdateFlow`.
 */
const HASH_STABLE_IMAGE_INDEX = 0;

/** True once the flow has reached an end state and will not advance on its own. */
export const isTerminalStep = (step: FlowStep) => step === 'success' || step === 'failed';

function hashHex(hash: Uint8Array | undefined): string | undefined {
    return hash ? uint8ArrayToHex(hash) : undefined;
}

export interface FirmwareUpdateFlow {
    step: FlowStep;
    /** 0-100 across all images. */
    uploadProgress: number;
    /** Index into `images` currently uploading. */
    currentImageIndex: number;
    images: ImageProgress[];
    error: string;
    /** Seconds spent waiting for the device to come back, while reconnecting. */
    reconnectElapsedMs: number;
    /** True once the wait has exceeded RECONNECT_PATIENCE_MS. */
    reconnectTakingLong: boolean;
    startUpload: () => Promise<void>;
    reboot: () => Promise<void>;
    reset: () => void;
}

/**
 * The guided update: upload → stage for test → reboot → verify → confirm.
 *
 * Images are marked permanent at staging time (`setImageState(hash, true)`), not
 * test-then-confirm. That is deliberate and not a shortcut: this SoC's bootloader is
 * built `CONFIG_BOOT_UPGRADE_ONLY=y` (overwrite-only), whose own Kconfig help says it
 * "prevents the fallback recovery" — slot 1 is copied over slot 0 and marked confirmed
 * by the bootloader itself. Hardware-confirmed: an image staged as `pending` came back
 * `active confirmed` with nothing confirming it. The chip architecture cannot support a
 * swap mode, so a test-then-confirm sequence would be a permanently no-op extra step.
 * Don't reintroduce one expecting rollback — there is none to have.
 *
 * ## What verification can actually check, per core
 *
 * The reference for an image is its own `IMAGE_TLV_SHA256`, read out of the `.bin`
 * inside the zip (`parseImageSha256`). The zip's manifest.json carries no digest of any
 * kind — verified against the real fw-v2.1.0 artifact — and `sha256(whole .bin)` is a
 * different value again, because the file ends with the TLV trailer the digest cannot
 * cover (`eeacf0fa…` vs `9f5d7d3a…`).
 *
 * The two cores then behave differently, measured on hardware with fw-v2.1.0:
 *
 * | image | file TLV    | staged slot 1 | active slot 0 after install |
 * |-------|-------------|---------------|-----------------------------|
 * | 0 app | `eeacf0fa…` | `eeacf0fa…`   | `eeacf0fa…`  (stable)       |
 * | 1 net | `e43ebfa1…` | `e43ebfa1…`   | `4d4b2c28…`  (changes)      |
 *
 * The app core's image is flashed into its own slot byte-for-byte, so its hash survives
 * the install. The net-core image is a wrapper that the app core's bootloader unwraps
 * and pushes into the network core over IPC, so what ends up installed hashes
 * differently — its file TLV can never match post-install, and expecting it to made the
 * flow report a false failure on a perfectly good update.
 *
 * So hashes are checked where they mean something:
 *  - **at staging, for every image** — device slot-1 hash vs file TLV, which proves the
 *    uploaded bytes arrived intact;
 *  - **after reboot, for the app core only** — plus a version check for every image,
 *    which is the only signal that survives the net core's transform.
 */
export function useFirmwareUpdateFlow(pkg: FirmwarePackage | null): FirmwareUpdateFlow {
    const { client } = useMcuMgrClientContext();
    const { selectedDevice } = useBluetooth();

    const [step, setStep] = useState<FlowStep>('loadingSource');
    const [uploadProgress, setUploadProgress] = useState(0);
    const [currentImageIndex, setCurrentImageIndex] = useState(0);
    const [images, setImages] = useState<ImageProgress[]>([]);
    const [error, setError] = useState('');
    const [reconnectElapsedMs, setReconnectElapsedMs] = useState(0);

    // The mac we rebooted, captured before the link drops. Used to recognise the
    // device coming back as *the same* device rather than any reconnect.
    const rebootedMacRef = useRef<string | null>(null);
    // Mirrors `step` for use inside effects that must not re-subscribe on every
    // transition (see the reconnect watcher below).
    const stepRef = useRef<FlowStep>(step);
    stepRef.current = step;

    // Keyed on the package's *contents*, not its object identity. A caller that
    // re-creates an equivalent package object on every render (easy to do by
    // accident, and exactly what an inline `useFirmwareUpdateFlow(parse(...))` would
    // do) would otherwise re-run this effect forever: it calls setImages with a fresh
    // array, which re-renders, which rebuilds the package, which re-runs the effect.
    const packageKey = pkg
        ? `${pkg.manifest.name}:${pkg.images.map(i => `${i.manifest.file}@${i.data.length}`).join(',')}`
        : '';

    useEffect(() => {
        if (!pkg) return;
        setImages(
            pkg.images.map(img => ({
                imageIndex: parseFirmwareImageIndex(img.manifest.image_index),
                file: img.manifest.file,
                version:
                    img.parsedHeader?.version ??
                    img.manifest.version_MCUBOOT ??
                    img.manifest.version ??
                    'unknown',
                expectedHash: parseImageSha256(img.data) ?? undefined,
                uploaded: false,
                staged: false,
                verified: false,
            }))
        );
        setStep(current => (current === 'loadingSource' ? 'ready' : current));
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, [packageKey]);

    const startUpload = useCallback(async () => {
        if (!client || !pkg) return;

        setError('');
        setUploadProgress(0);
        setStep('uploading');

        try {
            const total = pkg.images.length;

            for (let i = 0; i < total; i++) {
                const image = pkg.images[i];
                const imageIndex = parseFirmwareImageIndex(image.manifest.image_index);
                setCurrentImageIndex(i);

                await client.uploadImage(image.data, imageIndex, (sent, totalBytes) => {
                    setUploadProgress(calculateOverallUploadProgress(i, total, sent, totalBytes));
                });

                setImages(prev =>
                    prev.map(p => (p.imageIndex === imageIndex ? { ...p, uploaded: true } : p))
                );
            }

            // Stage every uploaded image for TEST (confirm=false). The device-reported
            // hash is captured too, as a fallback reference for images whose TLV could
            // not be parsed.
            setStep('staging');
            for (let i = 0; i < total; i++) {
                const imageIndex = parseFirmwareImageIndex(pkg.images[i].manifest.image_index);
                const state = await client.getImageState();
                const uploaded = findUploadedImageForIndex(state.images, imageIndex);
                if (!uploaded?.hash) {
                    throw new Error(
                        `Uploaded image ${imageIndex} not found on the device after upload`
                    );
                }
                // Prove the bytes that landed are the bytes we sent, before committing
                // to them. Valid for BOTH cores: the staged slot-1 hash matches the
                // file's TLV for each (it is only the net core's *installed* hash that
                // differs). A mismatch here means a corrupt upload, not a bad build.
                const hex = hashHex(uploaded.hash);
                const image = pkg.images[i];
                const expected = parseImageSha256(image.data);
                if (expected && hex !== expected) {
                    throw new Error(
                        `Image ${imageIndex} did not arrive intact: the device holds ` +
                            `${hex?.slice(0, 16)}… but the file is ${expected.slice(0, 16)}…`
                    );
                }

                // confirm=true: mark permanent. See the note on this hook - the
                // bootloader is overwrite-only, so there is no test-then-confirm to have.
                await client.setImageState(uploaded.hash, true);
                setImages(prev =>
                    prev.map(p =>
                        p.imageIndex === imageIndex ? { ...p, staged: true, stagedHash: hex } : p
                    )
                );
            }

            // Confirm the device really is holding them pending before telling the
            // user it is safe to reboot.
            const after = await client.getImageState();
            const pending = after.images.filter((s: ImageSlot) => s.slot === 1 && s.pending);
            if (pending.length === 0) {
                throw new Error('Device did not mark the uploaded image for test');
            }

            setStep('staged');
        } catch (e: unknown) {
            setError(e instanceof Error ? e.message : String(e));
            setStep('failed');
        }
    }, [client, pkg]);

    const reboot = useCallback(async () => {
        if (!client) return;
        rebootedMacRef.current = selectedDevice?.mac ?? null;
        setReconnectElapsedMs(0);
        setStep('rebooting');
        // reset() resolves even when the device dies before answering - by design,
        // since that is the normal case. So it tells us nothing; the disconnect that
        // follows is the real signal, picked up by the watcher below.
        await client.reset();
    }, [client, selectedDevice?.mac]);

    // rebooting → reconnecting: the link dropping is the confirmation that the reset
    // landed. Deliberately NOT setting intentionalDisconnectRef: that would suppress
    // the auto-reconnect loop, and the loop is exactly what we want here.
    useEffect(() => {
        if (step !== 'rebooting') return;
        if (selectedDevice) return;
        setStep('reconnecting');
    }, [step, selectedDevice]);

    // Elapsed-time ticker while waiting for the board to come back.
    useEffect(() => {
        if (step !== 'reconnecting') return;
        const startedAt = Date.now();
        const id = setInterval(() => setReconnectElapsedMs(Date.now() - startedAt), 1000);
        return () => clearInterval(id);
    }, [step]);

    // reconnecting → verifying. `selectedDevice` going null → non-null is the atomic
    // completion edge: use-ble-connection sets it in one go after the whole discovery
    // walk, so there is no half-discovered state to guard against. The client arrives
    // a moment later (its own hook keys on the new Device object), which is why
    // verification waits for `client` too.
    useEffect(() => {
        if (step !== 'reconnecting') return;
        if (!selectedDevice || !client) return;
        if (rebootedMacRef.current && selectedDevice.mac !== rebootedMacRef.current) return;
        setStep('verifying');
    }, [step, selectedDevice, client]);

    // verifying → success | failed.
    useEffect(() => {
        if (step !== 'verifying' || !client) return;
        let cancelled = false;

        async function verify() {
            try {
                const state = await client!.getImageState();
                if (cancelled) return;

                const results = images.map(img => {
                    const active = state.images.find(
                        (s: ImageSlot) =>
                            s.slot === 0 &&
                            s.active &&
                            (s.image === img.imageIndex ||
                                (s.image === undefined && img.imageIndex === 0))
                    );
                    // Version has to match for every image. The device reports a
                    // 'major.minor.patch' string where the file carries
                    // 'major.minor.patch+build', so compare on the former.
                    const versionMatches =
                        active?.version != null &&
                        img.version.startsWith(active.version);

                    // The hash only survives installation for the app core (see the
                    // table on this hook), so only demand it there. Requiring it for
                    // the net core reported a false failure on a good update.
                    const hashMatters =
                        img.imageIndex === HASH_STABLE_IMAGE_INDEX && !!img.expectedHash;
                    const hashMatches = hashHex(active?.hash) === img.expectedHash;

                    const verified = versionMatches && (!hashMatters || hashMatches);
                    return { ...img, verified };
                });

                if (cancelled) return;
                setImages(results);

                if (results.some(r => !r.verified)) {
                    // Deliberately does not promise a rollback: this bootloader is
                    // overwrite-only and cannot revert (see the note on this hook).
                    setError(
                        'The device restarted, but it is not running the firmware that ' +
                            'was uploaded. Check the FW Update Debug page for what is ' +
                            'actually installed.'
                    );
                    setStep('failed');
                    return;
                }

                setStep('success');
            } catch (e: unknown) {
                if (cancelled) return;
                setError(e instanceof Error ? e.message : String(e));
                setStep('failed');
            }
        }

        verify();
        return () => {
            cancelled = true;
        };
        // `images` is read to build the comparison but must not re-trigger verification -
        // this effect writes it, and depending on it would re-issue SMP traffic in a loop.
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, [step, client]);

    const reset = useCallback(() => {
        setError('');
        setUploadProgress(0);
        setCurrentImageIndex(0);
        setReconnectElapsedMs(0);
        rebootedMacRef.current = null;
        setImages(prev =>
            prev.map(p => ({ ...p, uploaded: false, staged: false, verified: false, stagedHash: undefined }))
        );
        setStep(pkg ? 'ready' : 'loadingSource');
    }, [pkg]);

    return {
        step,
        uploadProgress,
        currentImageIndex,
        images,
        error,
        reconnectElapsedMs,
        reconnectTakingLong: reconnectElapsedMs > RECONNECT_PATIENCE_MS,
        startUpload,
        reboot,
        reset,
    };
}
