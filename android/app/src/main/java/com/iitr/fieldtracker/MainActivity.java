package com.iitr.fieldtracker;

import android.os.Bundle;
import com.getcapacitor.BridgeActivity;

public class MainActivity extends BridgeActivity {
    @Override
    public void onCreate(Bundle savedInstanceState) {
        registerPlugin(ViewerTokenStoragePlugin.class);
        super.onCreate(savedInstanceState);
    }
}
