package dev.autom8ed.rgbsunglasses.ui.animations;

import android.bluetooth.BluetoothGattCharacteristic;
import android.bluetooth.BluetoothGattService;
import android.os.Bundle;
import android.util.Log;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Switch;

import androidx.annotation.NonNull;
import androidx.fragment.app.Fragment;

import dev.autom8ed.rgbsunglasses.databinding.FragmentRainbowanimationBinding;
import dev.autom8ed.rgbsunglasses.ui.bluetooth.DevKitBtInterface;
import dev.autom8ed.rgbsunglasses.ui.bluetooth.IsActiveCharacteristic;

public class RainbowAnimationFragment extends Fragment {

    private FragmentRainbowanimationBinding binding;

    static final public AnimationType kAnimationType = AnimationType.Rainbow;

    // Could be turned into a subclass
    IsActiveCharacteristic isActiveCharacteristic = null;
    DevKitBtInterface btInterface = null;

    Switch isActiveSw = null;

    AnimationServiceInterface animServiceCallback = new AnimationServiceInterface() {
        @Override
        public void onGattServiceFound(BluetoothGattService service) {
            super.onGattServiceFound(service);

            for (BluetoothGattCharacteristic characteristic : service.getCharacteristics()) {

                if(IsActiveCharacteristic.isIsActiveCharacteristic(characteristic.getUuid(), kAnimationType.ordinal())) {
                    Log.i("RainbowAnimation", "Found IsActive characteristic!");
                    isActiveCharacteristic = new IsActiveCharacteristic(characteristic, isActiveSw, btInterface);
                    continue;
                }

            }
        }
    };

    public View onCreateView(@NonNull LayoutInflater inflater,
                             ViewGroup container, Bundle savedInstanceState) {
        binding = FragmentRainbowanimationBinding.inflate(inflater, container, false);
        View root = binding.getRoot();

        Log.i("RainbowAnimFrag", "onCreateView");

        isActiveSw = binding.isRainbowActiveSwitch;

        btInterface = new DevKitBtInterface(getContext(), animServiceCallback, kAnimationType);

        return root;
    }

    @Override
    public void onDestroyView() {
        super.onDestroyView();
        binding = null;

        Log.i("RainbowAnimFrag", "onDestroyView");
    }
}
