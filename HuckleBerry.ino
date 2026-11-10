// HuckleBerry: phone-style firmware for the LilyGo T-Deck (Plus). Arduino needs a sketch file named
// after its folder; the code is all in src/, which Arduino compiles too. setup() and loop() are in
// src/main.cpp.
//
// Arduino finds libraries by scanning this file first, then src/ in name order. Including LilyGoLib
// here puts its lv_conf.h on the include path before any file in src/ pulls in LVGL.
#include <LilyGoLib.h>
