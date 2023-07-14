package dev.autom8ed.rgbsunglasses.ui.animations;

import android.os.Bundle;
import android.util.Log;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;

import androidx.annotation.NonNull;

import dev.autom8ed.rgbsunglasses.databinding.FragmentRainbowanimationBinding;

public class RainbowAnimationFragment extends AnimationBoilerplate {

    private FragmentRainbowanimationBinding binding;

    static final public BtServiceType K_BT_SERVICE_TYPE = BtServiceType.Rainbow;

    public View onCreateView(@NonNull LayoutInflater inflater,
                             ViewGroup container, Bundle savedInstanceState) {
        binding = FragmentRainbowanimationBinding.inflate(inflater, container, false);

        onAnimCreateView(inflater, binding.linearLayoutVertical, K_BT_SERVICE_TYPE);

        View root = binding.getRoot();

        Log.i("RainbowAnimFrag", "onCreateView");

        return root;
    }

    @Override
    public void onDestroyView() {
        super.onDestroyView();
        binding = null;

        Log.i("RainbowAnimFrag", "onDestroyView");
    }
}
