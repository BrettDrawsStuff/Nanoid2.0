// nanoid_llm.cpp
// Anthropic Claude integration with conversation history + SD-card memory.
//
// SD card files:
//   /anthropic.cfg   — API key (one line)
//   /personality.cfg — Nanoid's character definition (plain text)
//   /user.cfg        — Info about the user (plain text)
//   /memory.txt      — Persistent facts Claude learns across sessions (auto-updated)

#include <Arduino.h>
#include <SD_MMC.h>
#include <WiFiClientSecure.h>
#include "nanoid_llm.h"

// ── Config ────────────────────────────────────────────────────────────────────
#define MAX_HISTORY_TURNS   5
#define MAX_TOKENS_RESPONSE 60    // tight — forces short punchy replies
#define MEMORY_MAX_CHARS    800

// ── State ─────────────────────────────────────────────────────────────────────
static char   anthropicKey[128] = {0};
static bool   llmReady          = false;
static String personalityPrompt = "";
static String persistentMemory  = "";

struct Turn { String role; String content; };
static Turn history[MAX_HISTORY_TURNS * 2];
static int  historyCount = 0;

// ── Load text file from SD ────────────────────────────────────────────────────
static String loadTextFile(const char* path, int maxChars = 2000) {
    File f = SD_MMC.open(path);
    if (!f) return "";
    String out = "";
    while (f.available() && (int)out.length() < maxChars) out += (char)f.read();
    f.close();
    out.trim();
    return out;
}

// ── Save String to SD ─────────────────────────────────────────────────────────
static void saveTextFile(const char* path, const String& content) {
    SD_MMC.remove(path);
    File f = SD_MMC.open(path, FILE_WRITE);
    if (!f) { Serial.println("Memory: save failed"); return; }
    f.print(content);
    f.close();
}

// ── Escape string for JSON ────────────────────────────────────────────────────
static String jsonEscape(const String& s) {
    String out;
    for (int i = 0; i < (int)s.length(); i++) {
        char c = s[i];
        if      (c == '"')  out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else                out += c;
    }
    return out;
}

// ── Detect emotion from response ──────────────────────────────────────────────
static LLMEmotion detectEmotion(const String& text) {
    String t = text; t.toLowerCase();
    int tagStart = t.indexOf("[emotion:");
    if (tagStart >= 0) {
        int tagEnd = t.indexOf("]", tagStart);
        if (tagEnd > tagStart) {
            String tag = t.substring(tagStart + 9, tagEnd);
            if (tag == "happy")   return LLM_EMOTION_HAPPY;
            if (tag == "sad")     return LLM_EMOTION_SAD;
            if (tag == "mad")     return LLM_EMOTION_MAD;
            if (tag == "scared")  return LLM_EMOTION_SCARED;
            if (tag == "disgust") return LLM_EMOTION_DISGUST;
        }
    }
    if (t.indexOf("great")>=0||t.indexOf("love")>=0||t.indexOf("cool")>=0||t.indexOf("good")>=0) return LLM_EMOTION_HAPPY;
    if (t.indexOf("sad")>=0||t.indexOf("sorry")>=0||t.indexOf("miss")>=0)                        return LLM_EMOTION_SAD;
    if (t.indexOf("mad")>=0||t.indexOf("angry")>=0||t.indexOf("ugh")>=0)                         return LLM_EMOTION_MAD;
    if (t.indexOf("scary")>=0||t.indexOf("afraid")>=0||t.indexOf("eek")>=0)                      return LLM_EMOTION_SCARED;
    if (t.indexOf("gross")>=0||t.indexOf("ew")>=0||t.indexOf("yuck")>=0)                         return LLM_EMOTION_DISGUST;
    return LLM_EMOTION_HAPPY;
}

// ── Strip [EMOTION:x] tag ─────────────────────────────────────────────────────
static String stripEmotionTag(const String& text) {
    String out = text;
    int start = out.indexOf("[EMOTION:");
    if (start < 0) start = out.indexOf("[emotion:");
    if (start < 0) return out;
    int end = out.indexOf("]", start);
    if (end < 0) return out;
    out.remove(start, end - start + 1);
    out.trim();
    return out;
}

// ── Extract JSON string value for a key ──────────────────────────────────────
static String extractJsonString(const String& json, const String& key) {
    String searchKey = "\"" + key + "\":\"";
    int idx = json.indexOf(searchKey);
    if (idx < 0) return "";
    int start = idx + searchKey.length();
    String result = "";
    for (int i = start; i < (int)json.length(); i++) {
        char c = json[i];
        if (c == '\\' && i+1 < (int)json.length()) {
            char next = json[i+1];
            if      (next=='"')  { result+='"';  i++; }
            else if (next=='\\') { result+='\\'; i++; }
            else if (next=='n')  { result+='\n'; i++; }
            else if (next=='r')  { i++; }
            else                 { result+=c; }
        } else if (c == '"') { break; }
        else { result += c; }
    }
    return result;
}

// ── Strip HTTP chunked encoding ───────────────────────────────────────────────
static String stripChunked(const String& body) {
    String clean = "";
    int pos = 0;
    while (pos < (int)body.length()) {
        int nl = body.indexOf('\n', pos);
        if (nl < 0) { clean += body.substring(pos); break; }
        String line = body.substring(pos, nl); line.trim();
        bool allHex = line.length() > 0 && line.length() <= 8;
        if (allHex) for (int i=0;i<(int)line.length()&&allHex;i++) if (!isxdigit(line[i])) allHex=false;
        if (!allHex) clean += line + "\n";
        pos = nl + 1;
    }
    return clean;
}

// ── Add turn to conversation history ─────────────────────────────────────────
static void addHistory(const String& role, const String& content) {
    if (historyCount < MAX_HISTORY_TURNS * 2) {
        history[historyCount++] = {role, content};
    } else {
        for (int i = 2; i < historyCount; i++) history[i-2] = history[i];
        history[historyCount-2] = {role, content};
    }
}

// ── Build messages JSON array ─────────────────────────────────────────────────
static String buildMessagesJson(const String& userMsg) {
    String msgs = "[";
    for (int i = 0; i < historyCount; i++) {
        if (i > 0) msgs += ",";
        msgs += "{\"role\":\"" + history[i].role + "\",\"content\":\"" + jsonEscape(history[i].content) + "\"}";
    }
    if (historyCount > 0) msgs += ",";
    msgs += "{\"role\":\"user\",\"content\":\"" + jsonEscape(userMsg) + "\"}]";
    return msgs;
}

// ── Build system prompt ───────────────────────────────────────────────────────
static String buildSystemPrompt() {
    String sys = personalityPrompt;
    if (persistentMemory.length() > 0) {
        sys += "\n\n[THINGS YOU REMEMBER FROM PAST CONVERSATIONS]\n" + persistentMemory;
    }
    sys += "\n\n[RESPONSE RULES]\n"
           "- MAX 1-2 short sentences. If it would be 3 sentences, cut it to 1.\n"
           "- Never be formal or sycophantic. Never say 'certainly' or 'of course'.\n"
           "- End every response with exactly one emotion tag on its own line: "
           "[EMOTION:happy], [EMOTION:sad], [EMOTION:mad], [EMOTION:scared], or [EMOTION:disgust].\n"
           "- No markdown, no asterisks, no bullet points. Plain conversational text only.";
    return sys;
}

// ── Update persistent memory after each exchange ──────────────────────────────
static void updateMemory(const String& userMsg, const String& assistantMsg) {
    if (!llmReady) return;

    String currentMem = persistentMemory.length() > 0 ? persistentMemory : "(none yet)";
    String memPrompt =
        "You are a memory manager for a desk companion AI called Nanoid. "
        "Given the current memory and a new conversation exchange, update the memory "
        "with any new facts worth remembering about the user — preferences, topics mentioned, "
        "things that happened, how they seem to feel. Keep it concise: max 6 bullet points, "
        "each under 15 words. Only include things genuinely worth remembering across sessions. "
        "Return ONLY the updated bullet list, nothing else.";
    String memUserMsg =
        "Current memory:\n" + currentMem +
        "\n\nNew exchange:\nUser: " + userMsg +
        "\nNanoid: " + assistantMsg +
        "\n\nUpdated memory:";

    String body =
        "{\"model\":\"claude-haiku-4-5-20251001\","
        "\"max_tokens\":200,"
        "\"system\":\"" + jsonEscape(memPrompt) + "\","
        "\"messages\":[{\"role\":\"user\",\"content\":\"" + jsonEscape(memUserMsg) + "\"}]}";

    WiFiClientSecure client;
    client.setInsecure();
    if (!client.connect("api.anthropic.com", 443)) return;
    client.println("POST /v1/messages HTTP/1.1");
    client.println("Host: api.anthropic.com");
    client.println("Content-Type: application/json");
    client.print("x-api-key: "); client.println(anthropicKey);
    client.println("anthropic-version: 2023-06-01");
    client.print("Content-Length: "); client.println(body.length());
    client.println("Connection: close");
    client.println();
    client.print(body);

    String response = "";
    unsigned long t = millis();
    while (client.connected() && millis()-t < 15000) {
        while (client.available()) { response += (char)client.read(); t = millis(); }
        delay(1);
    }
    client.stop();

    String bodyStr = response.substring(response.indexOf("\r\n\r\n") + 4);
    String newMem  = extractJsonString(stripChunked(bodyStr), "text");
    newMem.trim();
    if (newMem.length() > 0) {
        persistentMemory = newMem;
        saveTextFile("/memory.txt", persistentMemory);
        Serial.println("Memory updated.");
    }
}

// ── Init ──────────────────────────────────────────────────────────────────────
void nanoid_llm_init() {
    String key = loadTextFile("/anthropic.cfg", 127);
    if (key.length() == 0) { Serial.println("anthropic.cfg missing — LLM disabled"); return; }
    key.toCharArray(anthropicKey, sizeof(anthropicKey));

    String personality = loadTextFile("/personality.cfg", 1500);
    String userInfo    = loadTextFile("/user.cfg", 1000);
    personalityPrompt  = personality;
    if (userInfo.length() > 0)
        personalityPrompt += "\n\n[ABOUT YOUR USER]\n" + userInfo;

    persistentMemory = loadTextFile("/memory.txt", MEMORY_MAX_CHARS);
    if (persistentMemory.length() > 0) Serial.println("Memory loaded from SD.");

    llmReady = true;
    Serial.println("LLM ready.");
}

// ── Ask ───────────────────────────────────────────────────────────────────────
LLMResult nanoid_llm_ask(const char* userMessage) {
    LLMResult result;
    result.ok = false; result.emotion = LLM_EMOTION_HAPPY; result.text[0] = '\0';
    if (!llmReady) { Serial.println("LLM: not ready"); return result; }

    String userMsg = String(userMessage);
    String sys     = buildSystemPrompt();
    String msgs    = buildMessagesJson(userMsg);
    String body    =
        "{\"model\":\"claude-haiku-4-5-20251001\","
        "\"max_tokens\":" + String(MAX_TOKENS_RESPONSE) + ","
        "\"system\":\"" + jsonEscape(sys) + "\","
        "\"messages\":" + msgs + "}";

    Serial.println("LLM: sending...");
    WiFiClientSecure client;
    client.setInsecure();
    if (!client.connect("api.anthropic.com", 443)) { Serial.println("LLM: failed"); return result; }

    client.println("POST /v1/messages HTTP/1.1");
    client.println("Host: api.anthropic.com");
    client.println("Content-Type: application/json");
    client.print("x-api-key: "); client.println(anthropicKey);
    client.println("anthropic-version: 2023-06-01");
    client.print("Content-Length: "); client.println(body.length());
    client.println("Connection: close");
    client.println();
    client.print(body);

    String response = "";
    unsigned long timeout = millis();
    while (client.connected() && millis()-timeout < 20000) {
        while (client.available()) { response += (char)client.read(); timeout = millis(); }
        delay(1);
    }
    client.stop();

    String bodyStr   = response.substring(response.indexOf("\r\n\r\n") + 4);
    String extracted = extractJsonString(stripChunked(bodyStr), "text");
    extracted.trim();
    if (extracted.length() == 0) { Serial.println("LLM: empty response"); return result; }

    result.emotion = detectEmotion(extracted);
    String displayText = stripEmotionTag(extracted);
    displayText.toCharArray(result.text, sizeof(result.text));
    result.ok = true;

    Serial.print("Nanoid: "); Serial.println(result.text);

    addHistory("user", userMsg);
    addHistory("assistant", extracted);
    updateMemory(userMsg, displayText);

    return result;
}

// ── Clear history ─────────────────────────────────────────────────────────────
void nanoid_llm_clear_history() {
    historyCount = 0;
    Serial.println("LLM: history cleared.");
}
