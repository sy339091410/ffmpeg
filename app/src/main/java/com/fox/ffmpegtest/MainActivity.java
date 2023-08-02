package com.fox.ffmpegtest;

import androidx.annotation.NonNull;
import androidx.appcompat.app.AppCompatActivity;

import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.Message;
import android.util.Log;
import android.view.Surface;
import android.view.SurfaceView;
import android.widget.TextView;

import com.fox.ffmpegtest.databinding.ActivityMainBinding;

public class MainActivity extends AppCompatActivity {

    // Used to load the 'ffmpegtest' library on application startup.
    static {
        System.loadLibrary("x264");
        System.loadLibrary("ffmpegtest");
    }

    private ActivityMainBinding binding;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        binding = ActivityMainBinding.inflate(getLayoutInflater());
        setContentView(binding.getRoot());

        // Example of a call to a native method
        TextView tv = binding.sampleText;
        SurfaceView surfaceView = binding.surfaceView;
        tv.setText(stringFromJNI(surfaceView.getHolder().getSurface()));
    }

    /**
     * A native method that is implemented by the 'ffmpegtest' native library,
     * which is packaged with this application.
     */
    public native String stringFromJNI(Surface surface);

}