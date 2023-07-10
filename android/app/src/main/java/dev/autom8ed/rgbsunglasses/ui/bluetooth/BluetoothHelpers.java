package dev.autom8ed.rgbsunglasses.ui.bluetooth;

import java.util.UUID;

import dev.autom8ed.rgbsunglasses.ui.animations.AnimationType;

public class BluetoothHelpers {
    public static UUID getUuidForAnimationService(long animationId) {
        UUID uuid = UUID.fromString(String.format("12345678-1234-5678-%04X-56789abc0000", animationId));
        return uuid;
    }

    public static AnimationType isAnimationService(UUID uuid) {
        for (AnimationType type : AnimationType.values()) {
            if (uuid.equals(BluetoothHelpers.getUuidForAnimationService(type.ordinal()))) {
                return type;
            }
        }

        return AnimationType.None;
    }
}
