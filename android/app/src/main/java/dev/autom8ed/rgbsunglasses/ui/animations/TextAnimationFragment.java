package dev.autom8ed.rgbsunglasses.ui.animations;

import android.os.Bundle;
import android.util.Log;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;

import androidx.annotation.NonNull;

import dev.autom8ed.rgbsunglasses.databinding.FragmentTextanimationBinding;

public class TextAnimationFragment extends AnimationBoilerplate {

    static final public BtServiceType K_BT_SERVICE_TYPE = BtServiceType.Text;

    private FragmentTextanimationBinding binding;


    public View onCreateView(@NonNull LayoutInflater inflater,
                             ViewGroup container, Bundle savedInstanceState) {

        binding = FragmentTextanimationBinding.inflate(inflater, container, false);

        onAnimCreateView(inflater, binding.linearLayoutVertical, K_BT_SERVICE_TYPE);

        View root = binding.getRoot();

        Log.i("TextAnimFrag", "onCreateView");

        return root;
    }

    @Override
    public void onDestroyView() {
        super.onDestroyView();
        binding = null;

        Log.i("TextAnimFrag", "onDestroyView");
    }
}