import {
  McubootUpdaterClient,
  McubootUpdaterState,
} from '@/services/mcuboot-updater-client';
import {
  UUID_MCUBOOT_UPDATER_CONTROL,
  UUID_MCUBOOT_UPDATER_DATA,
  UUID_MCUBOOT_UPDATER_SERVICE,
  UUID_MCUBOOT_UPDATER_STATUS,
} from '@/constants/bluetooth';

function makeCharacteristic(uuid: string, opts: { readValue?: string | null } = {}) {
  return {
    uuid,
    read: jest.fn(async () => ({ value: opts.readValue ?? null })),
    monitor: jest.fn((cb: any) => ({ remove: jest.fn(), _cb: cb })),
    writeWithResponse: jest.fn(async () => undefined),
    writeWithoutResponse: jest.fn(async () => undefined),
  };
}

function statusBase64(state: McubootUpdaterState, progress: number, errorCode: number, flashUnlocked: boolean) {
  const bytes = [state, progress, errorCode, flashUnlocked ? 1 : 0];
  return btoa(String.fromCharCode(...bytes));
}

function makeDevice(chars: ReturnType<typeof makeCharacteristic>[]) {
  const service = {
    uuid: UUID_MCUBOOT_UPDATER_SERVICE,
    characteristics: jest.fn(async () => chars),
  };
  return { services: jest.fn(async () => [service]) };
}

describe('McubootUpdaterClient.initialize', () => {
  it('seeds the status handler with an explicit initial read (issue #76 root cause fix)', async () => {
    // Regression test for the bug where reconnecting after "Prepare Device" showed the device
    // as still locked even though it had genuinely rebooted into unlocked mode: initialize()
    // used to only subscribe to notifications, never reading the characteristic's current value,
    // so a device that was already unlocked at connect time (no notification pending) never
    // updated the UI.
    const statusChar = makeCharacteristic(UUID_MCUBOOT_UPDATER_STATUS, {
      readValue: statusBase64(McubootUpdaterState.LOCKED, 0, 0, true),
    });
    const dataChar = makeCharacteristic(UUID_MCUBOOT_UPDATER_DATA);
    const controlChar = makeCharacteristic(UUID_MCUBOOT_UPDATER_CONTROL);
    const device = makeDevice([statusChar, dataChar, controlChar]);

    const client = new McubootUpdaterClient();
    const handler = jest.fn();
    // Registered before initialize(), matching the fixed call order in firmware-update-modal.tsx.
    client.onStatusChanged(handler);

    await client.initialize(device as any);

    expect(statusChar.read).toHaveBeenCalledTimes(1);
    expect(handler).toHaveBeenCalledWith({
      state: McubootUpdaterState.LOCKED,
      progress: 0,
      errorCode: 0,
      flashUnlocked: true,
    });
  });

  it('still dispatches subsequent status notifications after the initial read', async () => {
    const statusChar = makeCharacteristic(UUID_MCUBOOT_UPDATER_STATUS, {
      readValue: statusBase64(McubootUpdaterState.LOCKED, 0, 0, false),
    });
    const dataChar = makeCharacteristic(UUID_MCUBOOT_UPDATER_DATA);
    const controlChar = makeCharacteristic(UUID_MCUBOOT_UPDATER_CONTROL);
    const device = makeDevice([statusChar, dataChar, controlChar]);

    const client = new McubootUpdaterClient();
    const handler = jest.fn();
    client.onStatusChanged(handler);
    await client.initialize(device as any);

    expect(handler).toHaveBeenLastCalledWith(expect.objectContaining({ flashUnlocked: false }));

    const monitorCallback = (statusChar.monitor as jest.Mock).mock.calls[0][0];
    monitorCallback(null, { value: statusBase64(McubootUpdaterState.IDLE, 0, 0, true) });

    expect(handler).toHaveBeenLastCalledWith(
      expect.objectContaining({ state: McubootUpdaterState.IDLE, flashUnlocked: true })
    );
  });

  it('does not call the handler when there is no initial value to read (fresh/empty characteristic)', async () => {
    const statusChar = makeCharacteristic(UUID_MCUBOOT_UPDATER_STATUS, { readValue: null });
    const dataChar = makeCharacteristic(UUID_MCUBOOT_UPDATER_DATA);
    const controlChar = makeCharacteristic(UUID_MCUBOOT_UPDATER_CONTROL);
    const device = makeDevice([statusChar, dataChar, controlChar]);

    const client = new McubootUpdaterClient();
    const handler = jest.fn();
    client.onStatusChanged(handler);
    await client.initialize(device as any);

    expect(handler).not.toHaveBeenCalled();
  });

  it('throws when the MCUboot updater service is not present on the device', async () => {
    const device = { services: jest.fn(async () => []) };
    const client = new McubootUpdaterClient();
    await expect(client.initialize(device as any)).rejects.toThrow(
      'MCUboot updater service not found on device. Is this a Proto0 board?'
    );
  });
});
