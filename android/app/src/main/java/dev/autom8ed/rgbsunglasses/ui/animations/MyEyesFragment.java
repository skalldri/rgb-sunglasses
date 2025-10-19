package dev.autom8ed.rgbsunglasses.ui.animations;

import android.os.Bundle;
import android.util.Log;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;

import androidx.annotation.NonNull;

import dev.autom8ed.rgbsunglasses.databinding.FragmentGenericbtserviceBinding;

public class MyEyesFragment extends AnimationBoilerplate {

    private FragmentGenericbtserviceBinding binding;

    static final public BtServiceType K_BT_SERVICE_TYPE = BtServiceType.MyEyes;

    public View onCreateView(@NonNull LayoutInflater inflater,
                             ViewGroup container, Bundle savedInstanceState) {
        binding = FragmentGenericbtserviceBinding.inflate(inflater, container, false);

        onAnimCreateView(inflater, binding.linearLayoutVertical, K_BT_SERVICE_TYPE);

        View root = binding.getRoot();

        Log.i("MyEyesFragment", "onCreateView");

        return root;
    }

    @Override
    public void onDestroyView() {
        super.onDestroyView();
        binding = null;

        Log.i("MyEyesFragment", "onDestroyView");
    }
}
