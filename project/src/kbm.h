// Keyboard and mouse controls (kbm.cpp).
#pragma once

namespace sr {

// Mouse look speed (1.0 = default).
void SetMouseSensitivity(double sensitivity);
double GetMouseSensitivity();

// Mouse wheel input from the host window (positive = wheel up).
void AddMouseWheel(int delta);

}  // namespace sr
