import {
  base64ToUint8Array,
  createSmpHeader,
  decodeCbor,
  encodeCbor,
  formatHash,
  formatBytes,
  ImageCmd,
  McuMgrClient,
  parseImageHeader,
  parseSmpHeader,
  SmpGroup,
  SMP_CHARACTERISTIC_UUID,
  SMP_HEADER_SIZE,
  SMP_SERVICE_UUID,
  SmpOp,
  uint8ArrayToBase64,
} from '@/services/mcumgr';

function buildSmpPacket(
  payload: unknown,
  options: {
    op?: SmpOp;
    group?: SmpGroup;
    command?: number;
    sequence?: number;
  } = {}
): Uint8Array {
  const body = encodeCbor(payload);
  const header = createSmpHeader(
    options.op ?? SmpOp.READ_RESPONSE,
    options.group ?? SmpGroup.IMAGE,
    options.command ?? ImageCmd.STATE,
    body.length,
    options.sequence ?? 0
  );
  const packet = new Uint8Array(header.length + body.length);
  packet.set(header);
  packet.set(body, header.length);
  return packet;
}

beforeEach(() => {
  jest.spyOn(console, 'log').mockImplementation(() => {});
  jest.spyOn(console, 'warn').mockImplementation(() => {});
  jest.spyOn(console, 'error').mockImplementation(() => {});
});

afterEach(() => {
  jest.restoreAllMocks();
});

describe('mcumgr helpers', () => {
  it('creates and parses SMP headers', () => {
    const header = createSmpHeader(
      SmpOp.WRITE_REQUEST,
      SmpGroup.IMAGE,
      ImageCmd.UPLOAD,
      123,
      9
    );
    const parsed = parseSmpHeader(header);

    expect(parsed).toEqual({
      op: SmpOp.WRITE_REQUEST,
      version: 1,
      flags: 0,
      length: 123,
      group: SmpGroup.IMAGE,
      sequence: 9,
      command: ImageCmd.UPLOAD,
    });
  });

  it('throws on short SMP headers', () => {
    expect(() => parseSmpHeader(new Uint8Array([0x00, 0x01]))).toThrow(
      'SMP header too short'
    );
  });

  it('encodes and decodes CBOR payloads', () => {
    const data = { hello: 'world', n: 42 };
    const encoded = encodeCbor(data);
    const decoded = decodeCbor(encoded);
    expect(decoded).toEqual(data);
  });

  it('round-trips base64 and byte arrays', () => {
    const bytes = Uint8Array.from([1, 2, 3, 250, 255]);
    const encoded = uint8ArrayToBase64(bytes);
    const decoded = base64ToUint8Array(encoded);
    expect(Array.from(decoded)).toEqual(Array.from(bytes));
  });

  it('parses valid MCUboot image headers', () => {
    const image = new Uint8Array(32);
    const view = new DataView(image.buffer);
    view.setUint32(0, 0x96f3b83d, true);
    view.setUint32(12, 4096, true);
    image[20] = 1;
    image[21] = 2;
    view.setUint16(22, 345, true);
    view.setUint32(24, 6789, true);

    expect(parseImageHeader(image)).toEqual({
      magic: 0x96f3b83d,
      version: '1.2.345+6789',
      imageSize: 4096,
    });
  });

  it('returns null for invalid MCUboot headers', () => {
    const invalidMagic = new Uint8Array(32);
    new DataView(invalidMagic.buffer).setUint32(0, 0x00000000, true);
    expect(parseImageHeader(invalidMagic)).toBeNull();

    const tooShort = new Uint8Array(10);
    expect(parseImageHeader(tooShort)).toBeNull();
  });

  it('formats bytes and hashes', () => {
    expect(formatBytes(500)).toBe('500 B');
    expect(formatBytes(1024)).toBe('1.0 KB');
    expect(formatBytes(1024 * 1024)).toBe('1.00 MB');

    expect(formatHash(undefined)).toBe('N/A');
    expect(formatHash(Uint8Array.from([0, 1, 2, 3, 4, 5, 6, 7, 8]))).toBe(
      '0001020304050607...'
    );
  });
});

describe('McuMgrClient protocol behavior', () => {
  afterEach(() => {
    jest.useRealTimers();
  });

  it('times out when no SMP response is received', async () => {
    jest.useFakeTimers();

    const client = new McuMgrClient({} as never);
    const internal = client as any;
    internal.characteristic = {
      writeWithoutResponse: jest.fn(async () => undefined),
    };

    const requestPromise = internal.sendRequest(
      SmpOp.READ_REQUEST,
      SmpGroup.IMAGE,
      ImageCmd.STATE,
      {},
      25
    );

    const assertion = expect(requestPromise).rejects.toThrow('SMP request timeout');
    await jest.advanceTimersByTimeAsync(30);
    await assertion;
  });

  it('splits outgoing packets into chunks and decodes responses', async () => {
    const client = new McuMgrClient({} as never);
    const internal = client as any;
    const responsePacket = buildSmpPacket({ images: [] });
    let responded = false;

    const writeWithoutResponse = jest.fn(async () => {
      if (!responded) {
        responded = true;
        internal.handleResponse(responsePacket);
      }
    });

    internal.characteristic = { writeWithoutResponse };
    internal.mtu = 12; // forces chunking in sendRequest

    const result = await internal.sendRequest(
      SmpOp.READ_REQUEST,
      SmpGroup.IMAGE,
      ImageCmd.STATE,
      { payload: 'x'.repeat(200) },
      1000
    );

    expect(result).toEqual({ images: [] });
    expect(writeWithoutResponse.mock.calls.length).toBeGreaterThan(1);
  });

  it('returns empty object for SMP responses with no payload', async () => {
    const client = new McuMgrClient({} as never);
    const internal = client as any;
    const emptyResponse = createSmpHeader(
      SmpOp.READ_RESPONSE,
      SmpGroup.IMAGE,
      ImageCmd.STATE,
      0,
      0
    );

    internal.characteristic = {
      writeWithoutResponse: jest.fn(async () => {
        internal.handleResponse(emptyResponse);
      }),
    };
    internal.mtu = 120;

    const result = await internal.sendRequest(
      SmpOp.READ_REQUEST,
      SmpGroup.IMAGE,
      ImageCmd.STATE,
      {},
      1000
    );
    expect(result).toEqual({});
  });

  it('throws when sendRequest is used before initialization', async () => {
    const client = new McuMgrClient({} as never);
    const internal = client as any;
    await expect(
      internal.sendRequest(SmpOp.READ_REQUEST, SmpGroup.IMAGE, ImageCmd.STATE, {})
    ).rejects.toThrow('Client not initialized');
  });

  it('reassembles fragmented responses when first fragment is shorter than header', async () => {
    const client = new McuMgrClient({} as never);
    const internal = client as any;
    const responsePacket = buildSmpPacket({ images: [{ slot: 1, version: '1.0.0' }] });

    const writeWithoutResponse = jest.fn(async () => {
      internal.handleResponse(responsePacket.slice(0, 4));
      internal.handleResponse(responsePacket.slice(4));
    });

    internal.characteristic = { writeWithoutResponse };
    internal.mtu = 120;

    const result = await internal.sendRequest(
      SmpOp.READ_REQUEST,
      SmpGroup.IMAGE,
      ImageCmd.STATE,
      {},
      1000
    );

    expect(result).toEqual({ images: [{ slot: 1, version: '1.0.0' }] });
  });

  it('cleans up pending responses when destroyed', async () => {
    jest.useFakeTimers();

    const client = new McuMgrClient({} as never);
    const internal = client as any;
    internal.characteristic = {
      writeWithoutResponse: jest.fn(async () => undefined),
    };

    const requestPromise = internal.sendRequest(
      SmpOp.READ_REQUEST,
      SmpGroup.IMAGE,
      ImageCmd.STATE,
      {},
      1000
    );
    // sendRequest() queues onto requestChain (see mcumgr.ts), so the actual SMP exchange (and
    // its responseResolver/responseRejecter setup) starts one microtask later than the call
    // above - flush that before destroying, or destroy() would find nothing pending yet.
    await Promise.resolve();
    client.destroy();

    await expect(requestPromise).rejects.toThrow('Client destroyed');
  });
});

describe('McuMgrClient upload behavior', () => {
  it('includes len/image/sha only in first upload packet and reports progress', async () => {
    const client = new McuMgrClient({} as never);
    const internal = client as any;
    internal.mtu = 80;

    const sendRequestSpy = jest
      .spyOn(internal, 'sendRequest')
      .mockResolvedValueOnce({ off: 16 })
      .mockResolvedValueOnce({ off: 24 });

    const onProgress = jest.fn();
    const imageData = Uint8Array.from({ length: 24 }, (_, i) => i);
    await client.uploadImage(imageData, 2, onProgress);

    expect(sendRequestSpy).toHaveBeenCalledTimes(2);

    const firstPayload = sendRequestSpy.mock.calls[0][3];
    expect(firstPayload.off).toBe(0);
    expect(firstPayload.len).toBe(24);
    expect(firstPayload.image).toBe(2);
    expect(firstPayload.sha).toBeInstanceOf(Uint8Array);
    expect((firstPayload.sha as Uint8Array).length).toBe(32);

    const secondPayload = sendRequestSpy.mock.calls[1][3];
    expect(secondPayload.off).toBe(16);
    expect(secondPayload.len).toBeUndefined();
    expect(secondPayload.image).toBeUndefined();
    expect(secondPayload.sha).toBeUndefined();

    expect(onProgress).toHaveBeenNthCalledWith(1, 16, 24);
    expect(onProgress).toHaveBeenNthCalledWith(2, 24, 24);
  });

  it('increments by chunk size when response.off is absent', async () => {
    const client = new McuMgrClient({} as never);
    const internal = client as any;
    internal.mtu = 80; // chunk size = 16

    const sendRequestSpy = jest
      .spyOn(internal, 'sendRequest')
      .mockResolvedValueOnce({})
      .mockResolvedValueOnce({});

    const onProgress = jest.fn();
    await client.uploadImage(new Uint8Array(24), 0, onProgress);

    expect(sendRequestSpy).toHaveBeenCalledTimes(2);
    expect(onProgress).toHaveBeenNthCalledWith(1, 16, 24);
    expect(onProgress).toHaveBeenNthCalledWith(2, 24, 24);
  });

  it('throws when upload response includes rc error', async () => {
    const client = new McuMgrClient({} as never);
    const internal = client as any;
    internal.mtu = 120;

    jest.spyOn(internal, 'sendRequest').mockResolvedValueOnce({ rc: 8 });
    await expect(client.uploadImage(new Uint8Array(8), 0)).rejects.toThrow(
      'Image upload error at offset 0: rc=8'
    );
  });

  it('throws when upload response includes grouped error', async () => {
    const client = new McuMgrClient({} as never);
    const internal = client as any;
    internal.mtu = 120;

    jest.spyOn(internal, 'sendRequest').mockResolvedValueOnce({ err: { group: 1, rc: 2 } });
    await expect(client.uploadImage(new Uint8Array(8), 0)).rejects.toThrow(
      'Image upload error at offset 0: group=1, rc=2'
    );
  });

  it('throws when upload stalls at same offset repeatedly', async () => {
    const client = new McuMgrClient({} as never);
    const internal = client as any;
    internal.mtu = 120;

    jest.spyOn(internal, 'sendRequest').mockResolvedValue({ off: 0 });
    await expect(client.uploadImage(new Uint8Array(4), 0)).rejects.toThrow(
      'Image upload stalled at offset 0'
    );
  });

  it('throws when server returns an invalid upload offset', async () => {
    const client = new McuMgrClient({} as never);
    const internal = client as any;
    internal.mtu = 120;

    jest.spyOn(internal, 'sendRequest').mockResolvedValueOnce({ off: 999 });
    await expect(client.uploadImage(new Uint8Array(4), 0)).rejects.toThrow(
      'Image upload error: invalid offset 999 (total=4)'
    );
  });

  it('uses sane chunk floors when mtu is very small', async () => {
    const client = new McuMgrClient({} as never);
    const internal = client as any;
    internal.mtu = 1;

    const sendRequestSpy = jest
      .spyOn(internal, 'sendRequest')
      .mockResolvedValueOnce({ off: 1 })
      .mockResolvedValueOnce({ off: 2 })
      .mockResolvedValueOnce({ off: 3 });

    await client.uploadImage(Uint8Array.from([1, 2, 3]), 0);
    expect(sendRequestSpy).toHaveBeenCalledTimes(3);
  });
});

describe('McuMgrClient command wrappers', () => {
  it('maps getImageState responses', async () => {
    const client = new McuMgrClient({} as never);
    const internal = client as any;
    jest.spyOn(internal, 'sendRequest').mockResolvedValueOnce({
      images: [
        {
          image: 0,
          slot: 1,
          version: '1.2.3',
          hash: [0, 1, 2],
          confirmed: true,
        },
      ],
      splitStatus: 1,
    });

    const state = await client.getImageState();
    expect(state.splitStatus).toBe(1);
    expect(state.images[0].hash).toBeInstanceOf(Uint8Array);
    expect(Array.from(state.images[0].hash || [])).toEqual([0, 1, 2]);
  });

  it('throws for getImageState rc/err responses', async () => {
    const client = new McuMgrClient({} as never);
    const internal = client as any;

    jest.spyOn(internal, 'sendRequest').mockResolvedValueOnce({ rc: 7 });
    await expect(client.getImageState()).rejects.toThrow('Image state error: rc=7');

    jest.spyOn(internal, 'sendRequest').mockResolvedValueOnce({ err: { group: 1, rc: 9 } });
    await expect(client.getImageState()).rejects.toThrow('Image state error: group=1, rc=9');
  });

  it('setImageState sends payload with hash and confirm flag', async () => {
    const client = new McuMgrClient({} as never);
    const internal = client as any;
    const sendRequestSpy = jest.spyOn(internal, 'sendRequest').mockResolvedValueOnce({ images: [] });
    const hash = Uint8Array.from([10, 20, 30]);

    await client.setImageState(hash, true);
    expect(sendRequestSpy).toHaveBeenCalledWith(
      SmpOp.WRITE_REQUEST,
      SmpGroup.IMAGE,
      ImageCmd.STATE,
      { confirm: true, hash: Uint8Array.from(hash) }
    );
  });

  it('setImageState propagates rc/err responses', async () => {
    const client = new McuMgrClient({} as never);
    const internal = client as any;

    jest.spyOn(internal, 'sendRequest').mockResolvedValueOnce({ rc: 3 });
    await expect(client.setImageState(undefined, false)).rejects.toThrow('Set image state error: rc=3');

    jest.spyOn(internal, 'sendRequest').mockResolvedValueOnce({ err: { group: 1, rc: 7 } });
    await expect(client.setImageState(undefined, false)).rejects.toThrow(
      'Set image state error: group=1, rc=7'
    );
  });

  it('eraseImage and getSlotInfo propagate rc/err', async () => {
    const client = new McuMgrClient({} as never);
    const internal = client as any;

    jest.spyOn(internal, 'sendRequest').mockResolvedValueOnce({ rc: 5 });
    await expect(client.eraseImage(1)).rejects.toThrow('Image erase error: rc=5');

    jest.spyOn(internal, 'sendRequest').mockResolvedValueOnce({ err: { group: 1, rc: 4 } });
    await expect(client.getSlotInfo()).rejects.toThrow('Slot info error: group=1, rc=4');
  });

  it('eraseImage propagates grouped error response', async () => {
    const client = new McuMgrClient({} as never);
    const internal = client as any;

    jest.spyOn(internal, 'sendRequest').mockResolvedValueOnce({ err: { group: 1, rc: 2 } });
    await expect(client.eraseImage(1)).rejects.toThrow('Image erase error: group=1, rc=2');
  });

  it('getSlotInfo returns response and handles rc error', async () => {
    const client = new McuMgrClient({} as never);
    const internal = client as any;

    const slotInfo = { images: [{ image: 0, slots: [{ slot: 0, size: 1024 }] }] };
    jest.spyOn(internal, 'sendRequest').mockResolvedValueOnce(slotInfo);
    await expect(client.getSlotInfo()).resolves.toEqual(slotInfo);

    jest.spyOn(internal, 'sendRequest').mockResolvedValueOnce({ rc: 6 });
    await expect(client.getSlotInfo()).rejects.toThrow('Slot info error: rc=6');
  });

  it('echo/reset/getMcuMgrParams wrapper behavior', async () => {
    const client = new McuMgrClient({} as never);
    const internal = client as any;

    jest.spyOn(internal, 'sendRequest').mockResolvedValueOnce({ r: 'pong' });
    await expect(client.echo('ping')).resolves.toBe('pong');

    jest.spyOn(internal, 'sendRequest').mockResolvedValueOnce({});
    await expect(client.echo('ping')).resolves.toBe('');

    jest.spyOn(internal, 'sendRequest').mockRejectedValueOnce(new Error('disconnect during reset'));
    await expect(client.reset()).resolves.toBeUndefined();

    jest.spyOn(internal, 'sendRequest').mockResolvedValueOnce({});
    await expect(client.getMcuMgrParams()).resolves.toEqual({ buf_size: 0, buf_count: 0 });
  });

  it('performFirmwareUpdate orchestrates erase/upload/state/reset', async () => {
    const client = new McuMgrClient({} as never);
    const onStatus = jest.fn();
    const onProgress = jest.fn();
    const hash = Uint8Array.from([1, 2, 3, 4]);

    jest.spyOn(client, 'eraseImage').mockResolvedValueOnce();
    jest.spyOn(client, 'uploadImage').mockResolvedValueOnce();
    jest.spyOn(client, 'getImageState').mockResolvedValueOnce({
      images: [{ image: 0, slot: 1, version: '1.0.0', hash }],
    });
    jest.spyOn(client, 'setImageState').mockResolvedValueOnce({ images: [] });
    jest.spyOn(client, 'reset').mockResolvedValueOnce();

    await client.performFirmwareUpdate(Uint8Array.from([1, 2, 3]), {
      imageIndex: 0,
      eraseFirst: true,
      markForTest: true,
      resetAfterUpload: true,
      onStatus,
      onProgress,
    });

    expect(client.eraseImage).toHaveBeenCalledWith(1);
    expect(client.uploadImage).toHaveBeenCalledWith(expect.any(Uint8Array), 0, onProgress);
    expect(client.setImageState).toHaveBeenCalledWith(hash, false);
    expect(client.reset).toHaveBeenCalled();
    expect(onStatus).toHaveBeenCalledWith('Firmware update complete!');
  });

  it('performFirmwareUpdate surfaces failures via status and throw', async () => {
    const client = new McuMgrClient({} as never);
    const onStatus = jest.fn();

    jest.spyOn(client, 'eraseImage').mockRejectedValueOnce(new Error('erase failed'));
    await expect(
      client.performFirmwareUpdate(new Uint8Array([1]), { onStatus })
    ).rejects.toThrow('erase failed');

    expect(onStatus).toHaveBeenCalledWith('Update failed: Error: erase failed');
  });

  it('confirmCurrentImage delegates to setImageState confirm=true', async () => {
    const client = new McuMgrClient({} as never);
    const spy = jest.spyOn(client, 'setImageState').mockResolvedValueOnce({ images: [] });
    await client.confirmCurrentImage();
    expect(spy).toHaveBeenCalledWith(undefined, true);
  });
});

describe('McuMgrClient initialize behavior', () => {
  it('discovers SMP characteristic, starts monitor, and negotiates MTU', async () => {
    const monitorSubscription = { remove: jest.fn() };
    let monitorCallback: ((error: unknown, char: { value?: string } | null) => void) | null = null;

    const characteristic = {
      uuid: SMP_CHARACTERISTIC_UUID,
      monitor: jest.fn((cb: typeof monitorCallback) => {
        monitorCallback = cb;
        return monitorSubscription;
      }),
      writeWithoutResponse: jest.fn(async () => undefined),
    };
    const service = {
      uuid: SMP_SERVICE_UUID,
      characteristics: jest.fn(async () => [characteristic]),
    };
    const device = {
      discoverAllServicesAndCharacteristics: jest.fn(async () => undefined),
      services: jest.fn(async () => [service]),
      requestMTU: jest.fn(async () => 200),
    };

    const client = new McuMgrClient(device as never);
    await client.initialize();

    expect(device.discoverAllServicesAndCharacteristics).toHaveBeenCalled();
    expect(service.characteristics).toHaveBeenCalled();
    expect(characteristic.monitor).toHaveBeenCalled();
    expect((client as any).mtu).toBe(197);

    const responsePacket = buildSmpPacket({ images: [] });
    expect(monitorCallback).not.toBeNull();
    monitorCallback?.(null, { value: uint8ArrayToBase64(responsePacket) });
  });

  it('monitor callback ignores disconnect errors and destroyed state', async () => {
    const monitorSubscription = { remove: jest.fn() };
    let monitorCallback: ((error: unknown, char: { value?: string } | null) => void) | null = null;

    const characteristic = {
      uuid: SMP_CHARACTERISTIC_UUID,
      monitor: jest.fn((cb: typeof monitorCallback) => {
        monitorCallback = cb;
        return monitorSubscription;
      }),
      writeWithoutResponse: jest.fn(async () => undefined),
    };
    const service = {
      uuid: SMP_SERVICE_UUID,
      characteristics: jest.fn(async () => [characteristic]),
    };
    const device = {
      discoverAllServicesAndCharacteristics: jest.fn(async () => undefined),
      services: jest.fn(async () => [service]),
      requestMTU: jest.fn(async () => 200),
    };

    const client = new McuMgrClient(device as never);
    await client.initialize();
    expect(monitorCallback).not.toBeNull();

    monitorCallback?.({ message: 'device disconnect' }, null);
    client.destroy();
    monitorCallback?.(null, { value: uint8ArrayToBase64(buildSmpPacket({})) });

    expect(monitorSubscription.remove).toHaveBeenCalled();
  });

  it('monitor callback rejects pending request for non-disconnect errors', async () => {
    jest.useFakeTimers();
    const monitorSubscription = { remove: jest.fn() };
    let monitorCallback: ((error: unknown, char: { value?: string } | null) => void) | null = null;

    const characteristic = {
      uuid: SMP_CHARACTERISTIC_UUID,
      monitor: jest.fn((cb: typeof monitorCallback) => {
        monitorCallback = cb;
        return monitorSubscription;
      }),
      writeWithoutResponse: jest.fn(async () => undefined),
    };
    const service = {
      uuid: SMP_SERVICE_UUID,
      characteristics: jest.fn(async () => [characteristic]),
    };
    const device = {
      discoverAllServicesAndCharacteristics: jest.fn(async () => undefined),
      services: jest.fn(async () => [service]),
      requestMTU: jest.fn(async () => 200),
    };

    const client = new McuMgrClient(device as never);
    await client.initialize();
    const internal = client as any;

    const requestPromise = internal.sendRequest(
      SmpOp.READ_REQUEST,
      SmpGroup.IMAGE,
      ImageCmd.STATE,
      {},
      1000
    );
    const assertion = expect(requestPromise).rejects.toEqual({ message: 'SMP failed' });
    // See the comment in the "cleans up pending responses when destroyed" test above - the
    // responseRejecter isn't set until requestChain's queued microtask runs.
    await Promise.resolve();
    monitorCallback?.({ message: 'SMP failed' }, null);
    await assertion;
  });

  it('throws when SMP characteristic is missing', async () => {
    const service = {
      uuid: SMP_SERVICE_UUID,
      characteristics: jest.fn(async () => []),
    };
    const device = {
      discoverAllServicesAndCharacteristics: jest.fn(async () => undefined),
      services: jest.fn(async () => [service]),
      requestMTU: jest.fn(async () => 200),
    };

    const client = new McuMgrClient(device as never);
    await expect(client.initialize()).rejects.toThrow('SMP characteristic not found');
  });

  it('keeps default MTU when negotiation fails', async () => {
    const monitorSubscription = { remove: jest.fn() };
    const characteristic = {
      uuid: SMP_CHARACTERISTIC_UUID,
      monitor: jest.fn(() => monitorSubscription),
      writeWithoutResponse: jest.fn(async () => undefined),
    };
    const service = {
      uuid: SMP_SERVICE_UUID,
      characteristics: jest.fn(async () => [characteristic]),
    };
    const device = {
      discoverAllServicesAndCharacteristics: jest.fn(async () => undefined),
      services: jest.fn(async () => [service]),
      requestMTU: jest.fn(async () => {
        throw new Error('mtu fail');
      }),
    };

    const client = new McuMgrClient(device as never);
    await client.initialize();
    expect((client as any).mtu).toBe(400);
  });
});

describe('wire format sanity', () => {
  it('builds packets with expected header size', () => {
    const packet = buildSmpPacket({ hello: true }, { sequence: 7 });
    const header = packet.slice(0, SMP_HEADER_SIZE);
    const parsed = parseSmpHeader(header);
    expect(parsed.sequence).toBe(7);
    expect(parsed.length).toBe(packet.length - SMP_HEADER_SIZE);
  });

  it('base64 encodes packet bytes for BLE transport', () => {
    const packet = buildSmpPacket({ ok: true });
    const b64 = uint8ArrayToBase64(packet);
    expect(typeof b64).toBe('string');
    expect(b64.length).toBeGreaterThan(0);
  });
});
