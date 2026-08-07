import { useBluetooth } from '@/context/bluetooth-context';
import { useMcuMgrClientContext } from '@/context/mcumgr-client-context';
import {
    calculateOverallUploadProgress,
    findUploadedImageForIndex,
    FirmwarePackage,
    parseFirmwareImageIndex,
} from '@/services/firmware-package';
import { ImageSlot, uint8ArrayToHex } from '@/services/mcumgr';
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
    | 'confirming'
    | 'success'
    | 'failed';

/** Per-image outcome, used to drive the yellow → green card state. */
export interface ImageProgress {
    imageIndex: number;
    file: string;
    /** Version from the image's own MCUboot header, or the manifest. */
    version: string;
    uploaded: boolean;
    /** Hash the DEVICE reported for the staged slot-1 image (the verification reference). */
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
 * The install model is MCUboot's test-then-confirm, not the old "mark permanent at
 * upload time". That ordering is what makes the post-reboot check meaningful: until
 * `confirmCurrentImage()` runs, an image that fails to boot (or fails verification
 * and is never confirmed) is reverted by the bootloader on the next reset. Marking
 * permanent up front, as the previous screen did, left no way back from a bad image.
 *
 * Verification compares the hash the device reported for the staged slot-1 image
 * against the hash it reports for the active slot-0 image after the swap. The
 * reference has to come from the device on both sides because the firmware zip's
 * manifest carries no hash at all (see `ManifestFile` in services/firmware-package.ts).
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

            // Stage every uploaded image for TEST (confirm=false). Capturing the
            // device-reported hash here is the whole basis of verification later.
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
                await client.setImageState(uploaded.hash, false); // false = mark for TEST
                const hex = hashHex(uploaded.hash);
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

    // verifying → confirming → success | failed.
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
                    // Primary check: the running image is byte-identical to the one we
                    // staged. Falls back to the version string when either side has no
                    // hash, which is all an older firmware may report.
                    const verified = img.stagedHash
                        ? hashHex(active?.hash) === img.stagedHash
                        : active?.version === img.version;
                    return { ...img, verified };
                });

                if (cancelled) return;
                setImages(results);

                if (results.some(r => !r.verified)) {
                    setError(
                        'The device did not come back running the image that was uploaded. ' +
                            'It has not been confirmed, so the bootloader will restore the ' +
                            'previous firmware on the next restart.'
                    );
                    setStep('failed');
                    return;
                }

                // Only now is it safe to make the new image permanent.
                setStep('confirming');
                await client!.confirmCurrentImage();
                if (cancelled) return;
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
