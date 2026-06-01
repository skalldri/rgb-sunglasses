import JSZip from 'jszip';

import { ImageSlot, parseImageHeader } from '@/services/mcumgr';

export interface ManifestFile {
  type: string;
  board: string;
  soc: string;
  load_address: number;
  image_index: string;
  slot_index_primary: string;
  slot_index_secondary: string;
  version_MCUBOOT?: string;
  version?: string;
  size: number;
  file: string;
  modtime: number;
}

export interface FirmwareManifest {
  'format-version': number;
  time: number;
  files: ManifestFile[];
  name: string;
}

export interface FirmwareImage {
  manifest: ManifestFile;
  data: Uint8Array;
  parsedHeader: {
    magic: number;
    version: string;
    imageSize: number;
  } | null;
}

export interface FirmwarePackage {
  manifest: FirmwareManifest;
  images: FirmwareImage[];
}

export async function parseFirmwarePackageFromBase64(base64Data: string): Promise<FirmwarePackage> {
  const zip = await JSZip.loadAsync(base64Data, { base64: true });

  const manifestFile = zip.file('manifest.json');
  if (!manifestFile) {
    throw new Error('No manifest.json found in firmware package');
  }

  const manifestText = await manifestFile.async('text');
  const manifest: FirmwareManifest = JSON.parse(manifestText);

  if (!manifest.files || manifest.files.length === 0) {
    throw new Error('No firmware files listed in manifest');
  }

  const images: FirmwareImage[] = [];
  for (const fileInfo of manifest.files) {
    const binFile = zip.file(fileInfo.file);
    if (!binFile) {
      throw new Error(`Firmware file not found: ${fileInfo.file}`);
    }

    const binData = await binFile.async('uint8array');
    images.push({
      manifest: fileInfo,
      data: binData,
      parsedHeader: parseImageHeader(binData),
    });
  }

  images.sort(
    (a, b) => parseFirmwareImageIndex(a.manifest.image_index) - parseFirmwareImageIndex(b.manifest.image_index)
  );

  return {
    manifest,
    images,
  };
}

export function parseFirmwareImageIndex(imageIndex: string): number {
  const parsed = Number.parseInt(imageIndex, 10);
  return Number.isFinite(parsed) ? parsed : 0;
}

export function calculateOverallUploadProgress(
  currentImageIndex: number,
  totalImages: number,
  bytesSentForImage: number,
  totalBytesForImage: number
): number {
  if (totalImages <= 0) return 0;
  if (totalBytesForImage <= 0) return 0;

  const imageProgress = bytesSentForImage / totalBytesForImage;
  const overall = ((currentImageIndex + imageProgress) / totalImages) * 100;
  return Math.round(overall);
}

export function findUploadedImageForIndex(images: ImageSlot[], imageIndex: number): ImageSlot | undefined {
  return images.find(
    img => img.slot === 1 && (img.image === imageIndex || (img.image === undefined && imageIndex === 0))
  );
}
