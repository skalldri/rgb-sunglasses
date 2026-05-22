import JSZip from 'jszip';

import {
  calculateOverallUploadProgress,
  findUploadedImageForIndex,
  parseFirmwareImageIndex,
  parseFirmwarePackageFromBase64,
} from '@/services/firmware-package';

function createValidMcubootImageBytes(): Uint8Array {
  const image = new Uint8Array(32);
  const view = new DataView(image.buffer);
  view.setUint32(0, 0x96f3b83d, true); // magic
  view.setUint32(12, 4096, true); // image size
  image[20] = 1;
  image[21] = 4;
  view.setUint16(22, 2, true);
  view.setUint32(24, 99, true);
  return image;
}

async function createZipBase64(files: Record<string, string | Uint8Array>): Promise<string> {
  const zip = new JSZip();
  for (const [name, content] of Object.entries(files)) {
    zip.file(name, content);
  }
  return zip.generateAsync({ type: 'base64' });
}

describe('firmware package parsing', () => {
  it('throws when manifest.json is missing', async () => {
    const base64 = await createZipBase64({
      'app.bin': new Uint8Array([1, 2, 3]),
    });

    await expect(parseFirmwarePackageFromBase64(base64)).rejects.toThrow(
      'No manifest.json found in firmware package'
    );
  });

  it('throws when manifest has no files', async () => {
    const manifest = {
      'format-version': 1,
      time: Date.now(),
      name: 'empty',
      files: [],
    };
    const base64 = await createZipBase64({
      'manifest.json': JSON.stringify(manifest),
    });

    await expect(parseFirmwarePackageFromBase64(base64)).rejects.toThrow(
      'No firmware files listed in manifest'
    );
  });

  it('throws when a referenced firmware bin file is missing', async () => {
    const manifest = {
      'format-version': 1,
      time: Date.now(),
      name: 'missing-file',
      files: [
        {
          type: 'app',
          board: 'test',
          soc: 'nrf',
          load_address: 0,
          image_index: '0',
          slot_index_primary: '0',
          slot_index_secondary: '1',
          size: 123,
          file: 'missing.bin',
          modtime: Date.now(),
        },
      ],
    };
    const base64 = await createZipBase64({
      'manifest.json': JSON.stringify(manifest),
    });

    await expect(parseFirmwarePackageFromBase64(base64)).rejects.toThrow(
      'Firmware file not found: missing.bin'
    );
  });

  it('parses and sorts images by image_index', async () => {
    const manifest = {
      'format-version': 1,
      time: Date.now(),
      name: 'multi-image',
      files: [
        {
          type: 'radio',
          board: 'test',
          soc: 'nrf',
          load_address: 0,
          image_index: '1',
          slot_index_primary: '2',
          slot_index_secondary: '3',
          size: 32,
          file: 'radio.bin',
          modtime: Date.now(),
        },
        {
          type: 'app',
          board: 'test',
          soc: 'nrf',
          load_address: 0,
          image_index: '0',
          slot_index_primary: '0',
          slot_index_secondary: '1',
          size: 32,
          file: 'app.bin',
          modtime: Date.now(),
        },
      ],
    };

    const base64 = await createZipBase64({
      'manifest.json': JSON.stringify(manifest),
      'radio.bin': createValidMcubootImageBytes(),
      'app.bin': createValidMcubootImageBytes(),
    });

    const parsed = await parseFirmwarePackageFromBase64(base64);

    expect(parsed.manifest.name).toBe('multi-image');
    expect(parsed.images).toHaveLength(2);
    expect(parsed.images[0].manifest.image_index).toBe('0');
    expect(parsed.images[1].manifest.image_index).toBe('1');
    expect(parsed.images[0].parsedHeader?.version).toBe('1.4.2+99');
  });
});

describe('firmware package helpers', () => {
  it('parses image indexes safely', () => {
    expect(parseFirmwareImageIndex('10')).toBe(10);
    expect(parseFirmwareImageIndex('bad')).toBe(0);
  });

  it('calculates overall upload progress', () => {
    expect(calculateOverallUploadProgress(0, 2, 5, 10)).toBe(25);
    expect(calculateOverallUploadProgress(1, 2, 10, 10)).toBe(100);
    expect(calculateOverallUploadProgress(0, 0, 1, 1)).toBe(0);
    expect(calculateOverallUploadProgress(0, 2, 1, 0)).toBe(0);
  });

  it('finds uploaded image in slot 1 using image index fallback', () => {
    const images = [
      { slot: 0, image: 0, version: 'a' },
      { slot: 1, image: 1, version: 'b', hash: Uint8Array.from([1]) },
      { slot: 1, version: 'c', hash: Uint8Array.from([2]) },
    ];

    expect(findUploadedImageForIndex(images, 1)?.version).toBe('b');
    expect(findUploadedImageForIndex(images, 0)?.version).toBe('c');
    expect(findUploadedImageForIndex(images, 3)).toBeUndefined();
  });
});
