package dev.autom8ed.rgbsunglasses.ui.animations;

import android.util.Log;

import androidx.lifecycle.LiveData;
import androidx.lifecycle.MutableLiveData;
import androidx.lifecycle.ViewModel;

public class TextAnimationViewModel extends ViewModel {

    private final MutableLiveData<String> slot0Text;

    public TextAnimationViewModel() {
        slot0Text = new MutableLiveData<>();
        slot0Text.setValue("Hello world!");

        Log.i("TextAnimationViewModel", "constructor");
    }

    public void finalize() {
        Log.i("TextAnimationViewModel", "finalize");
    }

    public LiveData<String> getSlot0() {
        return slot0Text;
    }
}
