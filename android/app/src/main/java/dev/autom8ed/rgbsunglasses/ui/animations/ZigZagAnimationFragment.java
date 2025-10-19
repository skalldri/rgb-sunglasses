package dev.autom8ed.rgbsunglasses.ui.animations;

import android.os.Bundle;

import android.util.Log;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;

import androidx.annotation.NonNull;

import dev.autom8ed.rgbsunglasses.databinding.FragmentZigzaganimationBinding;

public class ZigZagAnimationFragment extends AnimationBoilerplate {

    private @NonNull FragmentZigzaganimationBinding binding;

    static final public BtServiceType K_BT_SERVICE_TYPE = BtServiceType.ZigZag;

    public View onCreateView(@NonNull LayoutInflater inflater,
                             ViewGroup container, Bundle savedInstanceState) {
        binding = FragmentZigzaganimationBinding.inflate(inflater, container, false);

        onAnimCreateView(inflater, binding.linearLayoutVertical, K_BT_SERVICE_TYPE);

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
