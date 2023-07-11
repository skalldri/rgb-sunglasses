package dev.autom8ed.rgbsunglasses.ui.animations;

import static dev.autom8ed.rgbsunglasses.ui.bluetooth.BluetoothHelpers.getUuidForCpfDescriptor;
import static dev.autom8ed.rgbsunglasses.ui.bluetooth.BluetoothHelpers.getUuidForCudDescriptor;

import android.bluetooth.BluetoothGatt;
import android.bluetooth.BluetoothGattCallback;
import android.bluetooth.BluetoothGattCharacteristic;
import android.bluetooth.BluetoothGattDescriptor;
import android.bluetooth.BluetoothGattService;
import android.bluetooth.BluetoothStatusCodes;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.LinearLayout;
import android.widget.Switch;

import androidx.annotation.NonNull;
import androidx.constraintlayout.widget.ConstraintSet;
import androidx.fragment.app.Fragment;

import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;

import dev.autom8ed.rgbsunglasses.databinding.FragmentRainbowanimationBinding;
import dev.autom8ed.rgbsunglasses.databinding.FragmentZigzaganimationBinding;
import dev.autom8ed.rgbsunglasses.databinding.ReadWriteIntBinding;
import dev.autom8ed.rgbsunglasses.ui.bluetooth.BluetoothDescriptorInfo;
import dev.autom8ed.rgbsunglasses.ui.bluetooth.BluetoothGattCharacteristicInfo;
import dev.autom8ed.rgbsunglasses.ui.bluetooth.BluetoothHelpers;
import dev.autom8ed.rgbsunglasses.ui.bluetooth.DevKitBtInterface;
import dev.autom8ed.rgbsunglasses.ui.bluetooth.DeviceGeneratedUiBase;
import dev.autom8ed.rgbsunglasses.ui.bluetooth.IsActiveCharacteristic;
import dev.autom8ed.rgbsunglasses.ui.bluetooth.ReadWriteBooleanCharacteristic;
import dev.autom8ed.rgbsunglasses.ui.bluetooth.ReadWriteIntegerCharacteristic;

public class ZigZagAnimationFragment extends AnimationBoilerplate {

    private @NonNull FragmentZigzaganimationBinding binding;

    static final public AnimationType kAnimationType = AnimationType.ZigZag;

    public View onCreateView(@NonNull LayoutInflater inflater,
                             ViewGroup container, Bundle savedInstanceState) {
        binding = FragmentZigzaganimationBinding.inflate(inflater, container, false);

        onAnimCreateView(inflater, binding.linearLayoutVertical, kAnimationType);

        View root = binding.getRoot();

        Log.i("ZigZagAnimation", "onCreateView");

        return root;
    }

    @Override
    public void onDestroyView() {
        super.onDestroyView();
        binding = null;

        Log.i("ZigZagAnimation", "onDestroyView");
    }
}
