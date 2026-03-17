// nanoid_llm.h
#pragma once

enum LLMEmotion {
  LLM_EMOTION_HAPPY,
  LLM_EMOTION_SAD,
  LLM_EMOTION_MAD,
  LLM_EMOTION_SCARED,
  LLM_EMOTION_DISGUST,
};

struct LLMResult {
  char       text[512];
  LLMEmotion emotion;
  bool       ok;
};

void      nanoid_llm_init();
LLMResult nanoid_llm_ask(const char* userMessage);
void      nanoid_llm_clear_history();
