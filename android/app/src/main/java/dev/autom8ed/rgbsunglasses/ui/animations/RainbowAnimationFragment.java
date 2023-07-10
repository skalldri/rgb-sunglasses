package dev.autom8ed.rgbsunglasses.ui.animations;

import android.os.Bundle;
import android.util.Log;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;

import androidx.annotation.NonNull;
import androidx.fragment.app.Fragment;

import dev.autom8ed.rgbsunglasses.databinding.FragmentRainbowanimationBinding;

public class RainbowAnimationFragment extends Fragment {

    private FragmentRainbowanimationBinding binding;

    public View onCreateView(@NonNull LayoutInflater inflater,
                             ViewGroup container, Bundle savedInstanceState) {
        binding = FragmentRainbowanimationBinding.inflate(inflater, container, false);
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
