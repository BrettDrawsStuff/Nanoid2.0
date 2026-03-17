// nanoid_audio.h
#pragma once

void nanoid_audio_init();
void nanoid_audio_play(const char* filename);
void nanoid_audio_loop();
void nanoid_audio_reinit(); // call after mic releases I2S pins
