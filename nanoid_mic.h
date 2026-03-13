// nanoid_mic.h
#pragma once

void nanoid_mic_init();
void nanoid_mic_prep();
bool nanoid_mic_start();
void nanoid_mic_stop();
void nanoid_mic_loop();
bool nanoid_mic_busy();
const char* nanoid_mic_last_transcript();
void nanoid_mic_scan_i2c();
