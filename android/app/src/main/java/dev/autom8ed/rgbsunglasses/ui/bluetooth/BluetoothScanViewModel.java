package dev.autom8ed.rgbsunglasses.ui.bluetooth;

import androidx.lifecycle.LiveData;
import androidx.lifecycle.MutableLiveData;
import androidx.lifecycle.ViewModel;

public class BluetoothScanViewModel extends ViewModel {

        private final MutableLiveData<String> mText;

        public BluetoothScanViewModel() {
            mText = new MutableLiveData<>();
            mText.setValue("This is BluetoothScan fragment");
        }

        public LiveData<String> getText() {
            return mText;
        }

        public void setText(String t) {
            mText.setValue(t);
        }
}
