package org.verdandi.app;

import android.os.Bundle;
import android.view.WindowManager;

import org.libsdl.app.SDLActivity;

/**
 * Verdandi: norns on android. everything happens in libmain.so (SDL_main in
 * matron/src/platform/android/android_main.cc); this activity only hosts SDL's surface.
 */
public class VerdandiActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] {"c++_shared", "SDL2", "main"};
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        // a norns screen does not go dark while a script plays
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    }
}
