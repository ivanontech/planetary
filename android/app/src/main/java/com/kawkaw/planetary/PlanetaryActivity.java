package com.kawkaw.planetary;

import org.libsdl.app.SDLActivity;

public class PlanetaryActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] {
            "SDL2",
            "main"
        };
    }
}
