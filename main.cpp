#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define SDL_MAIN_HANDLED

#include <windows.h>

// IMPORTANT: GLEW must be included before any other OpenGL or SDL header
#if __has_include(<GL/glew.h>)
#include <GL/glew.h>
#pragma comment(lib, "glew32.lib")
#define HAS_GLEW_HEADER 1
#endif

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <filesystem>
#include <random>
#include <sstream>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>

// Miniaudio decoder only
#define MA_NO_DEVICE_IO
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

#if __has_include(<projectM-4/projectM.h>)
#include <projectM-4/projectM.h>
#elif __has_include(<projectM/projectM.h>)
#include <projectM/projectM.h>
#else
#include <projectM.h>
#endif

#include <curl/curl.h>

#if defined(_MSC_VER)
#pragma comment(lib, "opengl32.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "wldap32.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "Normaliz.lib")
#endif

namespace fs = std::filesystem;

// RAII Flag Guard to prevent F2 background thread deadlocks
struct FlagGuard {
    std::atomic<bool>& flag;
    FlagGuard(std::atomic<bool>& f) : flag(f) {}
    ~FlagGuard() { flag = false; }
};

// -------------------------------------------------------------
// Safe Exit & Crash Filter
// -------------------------------------------------------------
void WaitExit(int code = 0) {
    std::cout << "\n======================================================\n";
    std::cout << " Process ended with code (" << code << ").\n";
    std::cout << " Press ENTER to close this CMD window...\n";
    std::cout << "======================================================\n" << std::flush;
    std::cin.clear();
    std::cin.ignore(10000, '\n');
    std::cin.get();
    exit(code);
}

LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
    std::cerr << "\n\n!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n";
    std::cerr << " [FATAL CRASH INTERCEPTED BY DIAGNOSTIC HANDLER]\n";
    std::cerr << " Exception Code:    0x" << std::hex << ep->ExceptionRecord->ExceptionCode << "\n";
    std::cerr << " Faulting Address: 0x" << ep->ExceptionRecord->ExceptionAddress << std::dec << "\n";
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        std::cerr << " Details: Access Violation (Null pointer or bad memory reference).\n";
    }
    std::cerr << "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n" << std::flush;
    WaitExit(-1);
    return EXCEPTION_EXECUTE_HANDLER;
}

// -------------------------------------------------------------
// Global Application State
// -------------------------------------------------------------
projectm_handle pm = nullptr;
std::string current_preset_content;
std::string gemini_key;
std::string fallback_key;
std::string system_prompt;
std::vector<fs::path> preset_files;

std::atomic<bool> is_generating{ false };
std::mutex preset_mutex;
std::string new_preset_pending;

ma_decoder decoder;
bool decoder_initialized = false;

const char* FALLBACK_PRESET =
"[preset00]\n"
"fRating=3.000000\n"
"fGammaAdj=1.000000\n"
"fDecay=0.980000\n"
"fVideoEchoZoom=1.000000\n"
"fVideoEchoAlpha=0.000000\n"
"nVideoEchoOrientation=0\n"
"nWaveMode=0\n"
"bAdditiveWaves=0\n"
"bWaveDots=0\n"
"bWaveThick=1\n"
"fWaveAlpha=0.800000\n"
"fWaveScale=1.000000\n"
"fWaveSmoothing=0.750000\n"
"fZoom=1.000000\n"
"fRot=0.000000\n"
"fWarp=0.010000\n"
"fWaveR=1.000000\n"
"fWaveG=0.500000\n"
"fWaveB=0.200000\n"
"per_frame_1=zoom = 1.0 + 0.04 * sin(time * 1.5) + (bass_att - 1.0) * 0.05;\n"
"per_frame_2=rot = 0.01 * sin(time * 0.7);\n"
"per_frame_3=wave_r = 0.5 + 0.5 * sin(time * 1.1);\n"
"per_frame_4=wave_g = 0.5 + 0.5 * sin(time * 1.3);\n"
"per_frame_5=wave_b = 0.5 + 0.5 * sin(time * 1.7);\n";

const char* DEFAULT_PROMPT_TXT =
"You are a MilkDrop and projectM preset master. "
"Rewrite and optimize the mathematical equations in this .milk preset. "
"1. Inject intense audio reactivity using bass_att, treb_att, and mid_att. "
"2. Clamp zoom strictly between 0.85 and 1.15 to avoid black holes or visual collapse. "
"3. Add fluid per_frame and per_vertex coordinate transforms (rad, ang, dx, dy). "
"4. Return ONLY valid, working .milk preset text starting with [preset00]. "
"Do NOT include markdown fences (```), commentary, thoughts, or chat.";

std::string MaskKey(const std::string& key) {
    if (key.size() <= 8) return "***";
    return key.substr(0, 4) + "..." + key.substr(key.size() - 4);
}

void LoadKeys() {
    std::cout << "[INIT] Reading api.key... " << std::flush;
    std::ifstream file("api.key");
    if (file.is_open()) {
        std::getline(file, gemini_key);
        std::getline(file, fallback_key);
        if (!gemini_key.empty() && gemini_key.back() == '\r') gemini_key.pop_back();
        if (!fallback_key.empty() && fallback_key.back() == '\r') fallback_key.pop_back();
        std::cout << "OK (Gemini: " << MaskKey(gemini_key) << ", Fallback: " << MaskKey(fallback_key) << ")\n" << std::flush;
    }
    else {
        std::cout << "[WARN] api.key not found. Online mutation will be disabled.\n" << std::flush;
    }
}

void LoadPromptFile() {
    std::cout << "[INIT] Checking prompt.txt... " << std::flush;
    if (!fs::exists("prompt.txt")) {
        std::ofstream out("prompt.txt");
        out << DEFAULT_PROMPT_TXT;
        out.close();
        system_prompt = DEFAULT_PROMPT_TXT;
        std::cout << "Created default prompt.txt (" << system_prompt.size() << " bytes)\n" << std::flush;
    }
    else {
        std::ifstream in("prompt.txt");
        std::stringstream buffer;
        buffer << in.rdbuf();
        system_prompt = buffer.str();
        if (system_prompt.empty()) {
            system_prompt = DEFAULT_PROMPT_TXT;
        }
        std::cout << "Loaded user prompt.txt (" << system_prompt.size() << " bytes)\n" << std::flush;
    }
}

void ScanPresets() {
    preset_files.clear();
    std::cout << "[INIT] Scanning ./preset directory... " << std::flush;
    if (fs::exists("preset")) {
        for (const auto& entry : fs::recursive_directory_iterator("preset")) {
            if (entry.path().extension() == ".milk") {
                preset_files.push_back(entry.path());
            }
        }
    }
    std::cout << "Found " << preset_files.size() << " preset(s)\n" << std::flush;
}

std::string SummarizeMathMesh(const std::string& preset_text) {
    std::istringstream stream(preset_text);
    std::string line;
    std::ostringstream out;
    int count = 0;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("per_frame_", 0) == 0 ||
            line.rfind("per_pixel_", 0) == 0 ||
            line.rfind("per_vertex_", 0) == 0 ||
            line.rfind("fZoom=", 0) == 0 ||
            line.rfind("fRot=", 0) == 0 ||
            line.rfind("fWarp=", 0) == 0) {
            out << "     | " << line << "\n";
            count++;
            if (count >= 8) {
                out << "     | ... (" << (preset_text.size() / 1024) << " KB total preset)\n";
                break;
            }
        }
    }
    if (count == 0) out << "     | (No explicit per_frame/per_vertex equations found)\n";
    return out.str();
}

std::string EscapeJSON(const std::string& input) {
    std::string escaped;
    escaped.reserve(input.size() * 2);
    for (char c : input) {
        switch (c) {
        case '"':  escaped += "\\\""; break;
        case '\\': escaped += "\\\\"; break;
        case '\n': escaped += "\\n";  break;
        case '\r': break;
        case '\t': escaped += "\\t";  break;
        default:   escaped += c;     break;
        }
    }
    return escaped;
}

std::string UnescapeJSON(const std::string& s) {
    std::string res;
    res.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char nxt = s[i + 1];
            switch (nxt) {
            case 'n':  res += '\n'; break;
            case 'r':  res += '\r'; break;
            case 't':  res += '\t'; break;
            case '"':  res += '"';  break;
            case '\\': res += '\\'; break;
            case '/':  res += '/';  break;
            default:   res += nxt;  break;
            }
            ++i;
        }
        else {
            res += s[i];
        }
    }
    return res;
}

std::string ExtractMilkPreset(const std::string& json_str) {
    size_t idx = json_str.find("[preset00]");
    if (idx == std::string::npos) {
        idx = json_str.find("\\[preset00\\]");
    }
    if (idx == std::string::npos) {
        return "";
    }

    size_t end = idx;
    bool escaped = false;
    while (end < json_str.size()) {
        char c = json_str[end];
        if (escaped) {
            escaped = false;
        }
        else if (c == '\\') {
            escaped = true;
        }
        else if (c == '"') {
            break;
        }
        ++end;
    }

    size_t start = idx;
    while (start > 0) {
        if (json_str[start] == '"' && json_str[start - 1] != '\\') {
            start += 1;
            break;
        }
        start--;
    }

    std::string raw = json_str.substr(start, end - start);
    std::string unescaped = UnescapeJSON(raw);

    size_t first_tick = unescaped.find("```");
    if (first_tick != std::string::npos) {
        size_t newline = unescaped.find('\n', first_tick);
        if (newline != std::string::npos) unescaped = unescaped.substr(newline + 1);
    }
    size_t last_tick = unescaped.rfind("```");
    if (last_tick != std::string::npos) unescaped = unescaped.substr(0, last_tick);

    return unescaped;
}

// -------------------------------------------------------------
// Live cURL Progress Ticker & Callback
// -------------------------------------------------------------
struct TransferStats {
    std::string buffer;
    size_t total_bytes = 0;
    std::chrono::steady_clock::time_point start_time;
    int last_reported_sec = -1;
};

size_t LiveCurlWriteCallback(void* contents, size_t size, size_t nmemb, TransferStats* stats) {
    size_t bytes = size * nmemb;
    stats->buffer.append(static_cast<char*>(contents), bytes);
    stats->total_bytes += bytes;
    return bytes;
}

int LiveProgressCallback(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
    TransferStats* stats = static_cast<TransferStats*>(clientp);
    auto now = std::chrono::steady_clock::now();
    int elapsed_sec = static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(now - stats->start_time).count());

    if (elapsed_sec != stats->last_reported_sec) {
        stats->last_reported_sec = elapsed_sec;
        if (stats->total_bytes == 0) {
            std::cout << "\r   [NET] Sending " << (ultotal > 0 ? ultotal : ulnow)
                << " bytes... Waiting for server inference (" << elapsed_sec << "s)" << std::flush;
        }
        else {
            std::cout << "\r   [NET] Streaming response... (" << stats->total_bytes
                << " bytes received, " << elapsed_sec << "s)" << std::flush;
        }
    }
    return 0;
}

// -------------------------------------------------------------
// Live AI Request Engine
// -------------------------------------------------------------
std::string RequestModifiedPreset(const std::string& basePreset) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        std::cerr << "\n   [ERROR] Failed to initialize libcurl.\n" << std::flush;
        return "";
    }

    LoadPromptFile();

    std::string escaped_system_prompt = EscapeJSON(system_prompt);
    std::string escaped_preset = EscapeJSON(basePreset);
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    TransferStats stats;
    stats.start_time = std::chrono::steady_clock::now();

    // 1. PRIMARY: Gemini 3.6 Pro
    std::string gemini_model = "gemini-3.6-flash";
    std::string gemini_url = "https://generativelanguage.googleapis.com/v1beta/models/" + gemini_model + ":generateContent?key=" + gemini_key;

    std::string gemini_json = "{"
        "\"systemInstruction\":{\"parts\":[{\"text\":\"" + escaped_system_prompt + "\"}]},"
        "\"contents\":[{\"parts\":[{\"text\":\"" + escaped_preset + "\"}]}],"
        "\"generationConfig\":{\"temperature\":0.7}"
        "}";

    std::cout << "\n   [AI-1] Querying Google Gemini (" << gemini_model << ")...\n" << std::flush;

    curl_easy_setopt(curl, CURLOPT_URL, gemini_url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, gemini_json.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, LiveCurlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &stats);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, LiveProgressCallback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &stats);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L); // 120s timeout
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);

    CURLcode res = curl_easy_perform(curl);
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - stats.start_time).count();

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    std::cout << "\n   [NET] Gemini completed in " << (duration / 1000.0) << "s (HTTP " << http_code << ")\n" << std::flush;

    std::string extracted_preset = ExtractMilkPreset(stats.buffer);

    // Fallback if Gemini 3.6 Pro failed
    if (res != CURLE_OK || http_code != 200 || extracted_preset.empty()) {
        std::cerr << "\n   !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n";
        std::cerr << "   [AI-1 NOTICE] Gemini 3.6 Pro did not complete request.\n";
        std::cerr << "   HTTP Code: " << http_code << "\n";
        std::cerr << "   Raw Server Response:\n";
        std::cerr << (stats.buffer.empty() ? "   (Empty response body / Connection timeout)\n" : stats.buffer.substr(0, 500)) << "\n";
        if (stats.buffer.size() > 500) std::cerr << "   ... [truncated]\n";
        std::cerr << "   !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n\n" << std::flush;

        if (fallback_key.empty() || fallback_key == "YOUR_FALLBACK_API_KEY") {
            std::cerr << "   [AI-2] Secondary key missing in api.key. Aborting.\n" << std::flush;
            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
            return "";
        }

        // Determine best model for Groq without exceeding TPM limits
        std::string fallback_url = "https://api.openai.com/v1/chat/completions";
        std::string fallback_model = "gpt-4o-mini";

        if (fallback_key.rfind("gsk_", 0) == 0) {
            fallback_url = "https://api.groq.com/openai/v1/chat/completions";
            if ((basePreset.size() + system_prompt.size()) > 15000) {
                fallback_model = "openai/gpt-oss-20b";
                std::cout << "   [AI-2] Payload size is large (" << (basePreset.size() + system_prompt.size())
                    << " bytes). Engaging high-TPM model: " << fallback_model << "...\n" << std::flush;
            }
            else {
                fallback_model = "openai/gpt-oss-120b";
                std::cout << "   [AI-2] Engaging Groq flagship model: " << fallback_model << "...\n" << std::flush;
            }
        }
        else {
            std::cout << "   [AI-2] Engaging OpenAI Fallback: " << fallback_model << "...\n" << std::flush;
        }

        stats.buffer.clear();
        stats.total_bytes = 0;
        stats.start_time = std::chrono::steady_clock::now();
        stats.last_reported_sec = -1;

        curl_slist_free_all(headers);
        headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        std::string auth_header = "Authorization: Bearer " + fallback_key;
        headers = curl_slist_append(headers, auth_header.c_str());

        std::string fallback_json = "{"
            "\"model\": \"" + fallback_model + "\","
            "\"messages\": ["
            "{\"role\": \"system\", \"content\": \"" + escaped_system_prompt + "\"},"
            "{\"role\": \"user\", \"content\": \"" + escaped_preset + "\"}"
            "]"
            "}";

        curl_easy_setopt(curl, CURLOPT_URL, fallback_url.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, fallback_json.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        res = curl_easy_perform(curl);
        duration = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - stats.start_time).count();
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

        std::cout << "\n   [NET] Fallback completed in " << (duration / 1000.0) << "s (HTTP " << http_code << ")\n" << std::flush;

        // Auto-retry if 120B hit rate limit (HTTP 413)
        if (http_code == 413 && fallback_model == "openai/gpt-oss-120b") {
            std::cout << "   [AI-2] 120B hit TPM rate limit. Auto-retrying with high-limit model openai/gpt-oss-20b...\n" << std::flush;
            fallback_model = "openai/gpt-oss-20b";
            fallback_json = "{"
                "\"model\": \"" + fallback_model + "\","
                "\"messages\": ["
                "{\"role\": \"system\", \"content\": \"" + escaped_system_prompt + "\"},"
                "{\"role\": \"user\", \"content\": \"" + escaped_preset + "\"}"
                "]"
                "}";
            stats.buffer.clear();
            stats.total_bytes = 0;
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, fallback_json.c_str());
            curl_easy_perform(curl);
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        }

        extracted_preset = ExtractMilkPreset(stats.buffer);

        if (http_code != 200 || extracted_preset.empty()) {
            std::cerr << "\n   !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n";
            std::cerr << "   [AI-2 FAILURE] Fallback API Request ALSO Failed!\n";
            std::cerr << "   HTTP Code: " << http_code << "\n";
            std::cerr << "   Raw Server Response:\n";
            std::cerr << (stats.buffer.empty() ? "   (Empty response body)\n" : stats.buffer.substr(0, 500)) << "\n";
            std::cerr << "   !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n\n" << std::flush;
            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
            return "";
        }
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return extracted_preset;
}

// -------------------------------------------------------------
// [F2] Asynchronous AI Mutation Worker (With RAII Deadlock Protection)
// -------------------------------------------------------------
void MutatePresetAsync() {
    bool expected = false;
    if (!is_generating.compare_exchange_strong(expected, true)) {
        std::cout << "\n[AI-BUSY] A mutation is already processing. Please wait...\n" << std::flush;
        return;
    }

    std::thread([]() {
        // Guaranteed RAII flag reset when thread exits
        FlagGuard guard(is_generating);

        std::string base_content = current_preset_content;
        if (base_content.empty()) {
            base_content = FALLBACK_PRESET;
        }

        std::cout << "\n======================================================\n";
        std::cout << " [AI MUTATION TRIGGERED: F2]\n";
        std::cout << " Active Preset Payload: " << base_content.size() << " bytes\n";
        std::cout << " Outgoing Math & Mesh Preview:\n";
        std::cout << SummarizeMathMesh(base_content);
        std::cout << "------------------------------------------------------\n" << std::flush;

        try {
            std::string modified = RequestModifiedPreset(base_content);

            if (!modified.empty() && modified.find("[preset00]") != std::string::npos) {
                std::cout << "\n [AI SUCCESS] Valid MilkDrop preset generated!\n";
                std::cout << " Mutated Math & Mesh (INCOMING):\n";
                std::cout << SummarizeMathMesh(modified);
                std::cout << "======================================================\n\n" << std::flush;

                std::lock_guard<std::mutex> lock(preset_mutex);
                new_preset_pending = modified;
            }
            else {
                std::cerr << "\n [AI-ABORT] Mutation could not be applied. Retaining current visual.\n" << std::flush;
            }
        }
        catch (const std::exception& e) {
            std::cerr << "\n [AI-EXCEPT] Exception caught during worker execution: " << e.what() << "\n" << std::flush;
        }
        catch (...) {
            std::cerr << "\n [AI-EXCEPT] Unknown exception caught during worker execution.\n" << std::flush;
        }
        }).detach();
}

// -------------------------------------------------------------
// [F3] Safe Random Preset Switcher (With Try-Catch Protection)
// -------------------------------------------------------------
void LoadRandomPreset() {
    if (preset_files.empty()) {
        std::cout << "\n[WARN] No presets available in ./preset directory to switch.\n\n" << std::flush;
        return;
    }

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<size_t> dis(0, preset_files.size() - 1);

    // Try up to 5 random picks if a corrupted preset is encountered
    for (int attempts = 0; attempts < 5; ++attempts) {
        fs::path random_path = preset_files[dis(gen)];

        std::ifstream file(random_path);
        if (!file.is_open()) continue;

        std::stringstream buffer;
        buffer << file.rdbuf();
        std::string new_content = buffer.str();

        if (new_content.empty() || new_content.find("[preset00]") == std::string::npos) {
            continue;
        }

        try {
            // Write to temp file and load safely into projectM
            std::ofstream out("temp_ai.milk");
            out << new_content;
            out.close();

            projectm_load_preset_file(pm, "temp_ai.milk", false);
            current_preset_content = new_content;

            std::cout << "\n======================================================\n";
            std::cout << " [INSTANT PRESET LOADED: F3]\n";
            std::cout << " Preset File: " << random_path.filename().string() << " (" << current_preset_content.size() << " bytes)\n";
            std::cout << " Math & Mesh Preview:\n";
            std::cout << SummarizeMathMesh(current_preset_content);
            std::cout << "======================================================\n\n" << std::flush;
            return;
        }
        catch (...) {
            std::cerr << "\n [WARN] Preset " << random_path.filename().string() << " failed shader compile. Retrying...\n" << std::flush;
        }
    }
}

// -------------------------------------------------------------
// [F1] Auto-increment Save
// -------------------------------------------------------------
void SavePreset() {
    try {
        fs::create_directories("save");
        int counter = 1;
        fs::path save_path;
        char filename[64];

        do {
            snprintf(filename, sizeof(filename), "save/preset_%04d.milk", counter++);
            save_path = filename;
        } while (fs::exists(save_path));

        std::ofstream out(save_path);
        if (current_preset_content.empty()) {
            out << FALLBACK_PRESET;
        }
        else {
            out << current_preset_content;
        }
        out.close();
        std::cout << "\n[IO SUCCESS] Saved current preset snapshot to: " << save_path.string() << "\n\n" << std::flush;
    }
    catch (const std::exception& e) {
        std::cerr << "\n[IO ERROR] Failed to save preset: " << e.what() << "\n\n" << std::flush;
    }
}

// -------------------------------------------------------------
// Host Entry Point
// -------------------------------------------------------------
int main(int argc, char* argv[]) {
    SetUnhandledExceptionFilter(CrashFilter);

    std::cout << "======================================================\n";
    std::cout << "   ProjectM v4 + SDL3 Live AI Visualizer Host         \n";
    std::cout << "======================================================\n" << std::flush;
    std::cout << "[DEBUG] Working Directory: " << fs::current_path().string() << "\n" << std::flush;

    LoadKeys();
    LoadPromptFile();
    ScanPresets();
    curl_global_init(CURL_GLOBAL_DEFAULT);

    // STEP 1: SDL3 Subsystems Init
    std::cout << "[STEP 1/10] Initializing SDL3 (Video + Audio)... " << std::flush;
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        std::cerr << "\n[FAIL] SDL_Init Error: " << SDL_GetError() << "\n";
        WaitExit(-1);
    }
    std::cout << "SUCCESS.\n" << std::flush;

    // STEP 2: Configure OpenGL Compatibility Context
    std::cout << "[STEP 2/10] Setting SDL OpenGL Context Attributes (3.3 Compatibility)... " << std::flush;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    std::cout << "SUCCESS.\n" << std::flush;

    // STEP 3: Window Creation
    std::cout << "[STEP 3/10] Creating 1080x768 OpenGL Window... " << std::flush;
    SDL_Window* window = SDL_CreateWindow("ProjectM Visualizer (AI Enabled)", 1080, 768, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (!window) {
        std::cerr << "\n[FAIL] SDL_CreateWindow Error: " << SDL_GetError() << "\n";
        WaitExit(-1);
    }
    std::cout << "SUCCESS.\n" << std::flush;

    // STEP 4: Context Creation & Binding
    std::cout << "[STEP 4/10] Creating and binding OpenGL Context... " << std::flush;
    SDL_GLContext gl_ctx = SDL_GL_CreateContext(window);
    if (!gl_ctx) {
        std::cerr << "\n[FAIL] SDL_GL_CreateContext Error: " << SDL_GetError() << "\n";
        WaitExit(-1);
    }
    SDL_GL_MakeCurrent(window, gl_ctx);
    SDL_GL_SetSwapInterval(1);
    std::cout << "SUCCESS.\n" << std::flush;

    // STEP 4.5: Initialize OpenGL Extension Wrangler (GLEW)
    std::cout << "[STEP 4.5] Initializing OpenGL Extension Pointers (GLEW)... " << std::flush;
    bool glew_ready = false;

#if defined(HAS_GLEW_HEADER)
    glewExperimental = GL_TRUE;
    GLenum err = glewInit();
    if (err == GLEW_OK) glew_ready = true;
#else
    HMODULE hGlew = GetModuleHandleA("glew32.dll");
    if (!hGlew) hGlew = LoadLibraryA("glew32.dll");
    if (hGlew) {
        unsigned char* pExp = (unsigned char*)GetProcAddress(hGlew, "glewExperimental");
        if (pExp) *pExp = 1;
        typedef unsigned int(__stdcall* PFN_glewInit)(void);
        PFN_glewInit pGlewInit = (PFN_glewInit)GetProcAddress(hGlew, "glewInit");
        if (pGlewInit && pGlewInit() == 0) glew_ready = true;
    }
#endif

    while (glGetError() != GL_NO_ERROR);

    if (glew_ready) {
        std::cout << "SUCCESS.\n" << std::flush;
    }
    else {
        std::cerr << "\n[FAIL] Could not initialize GLEW. Ensure glew32.dll is beside the executable.\n";
        WaitExit(-1);
    }

    // STEP 5: GPU Hardware Profile Query
    std::cout << "[STEP 5/10] Querying Hardware Profile:\n" << std::flush;
    std::cout << "  - GL_VENDOR:   " << reinterpret_cast<const char*>(glGetString(GL_VENDOR)) << "\n";
    std::cout << "  - GL_RENDERER: " << reinterpret_cast<const char*>(glGetString(GL_RENDERER)) << "\n";
    std::cout << "  - GL_VERSION:  " << reinterpret_cast<const char*>(glGetString(GL_VERSION)) << "\n" << std::flush;

    // STEP 6: projectM Instance Creation
    std::cout << "[STEP 6/10] Initializing projectM v4 core instance... " << std::flush;
    pm = projectm_create();
    if (!pm) {
        std::cerr << "\n[FAIL] projectm_create() returned NULL!\n";
        WaitExit(-1);
    }
    projectm_set_window_size(pm, 1080, 768);
    std::cout << "SUCCESS.\n" << std::flush;

    // STEP 7: Audio Pipeline Setup
    std::cout << "[STEP 7/10] Initializing miniaudio decoder & SDL3 stream... " << std::flush;
    SDL_AudioStream* stream = nullptr;
    SDL_AudioSpec spec;
    spec.format = SDL_AUDIO_F32;
    spec.channels = 2;
    spec.freq = 44100;

    if (ma_decoder_init_file("song.mp3", nullptr, &decoder) == MA_SUCCESS) {
        decoder_initialized = true;
        spec.channels = decoder.outputChannels;
        spec.freq = decoder.outputSampleRate;
        std::cout << "\n  -> Opened 'song.mp3': " << spec.freq << "Hz, " << spec.channels << " channels.\n" << std::flush;
    }
    else {
        std::cout << "\n  -> [NOTICE] 'song.mp3' not found beside executable. Running in silent visualizer mode.\n" << std::flush;
    }

    stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (stream) {
        SDL_ResumeAudioStreamDevice(stream);
        std::cout << "  -> Audio playback stream active.\n" << std::flush;
    }
    else {
        std::cerr << "  -> [WARN] Audio output stream failed: " << SDL_GetError() << "\n" << std::flush;
    }

    // STEP 8: Seed Initial Preset & Warm Audio Buffers
    std::cout << "[STEP 8/10] Loading initial MilkDrop preset... " << std::flush;
    current_preset_content = FALLBACK_PRESET;
    std::ofstream initial_out("temp_ai.milk");
    initial_out << current_preset_content;
    initial_out.close();

    projectm_load_preset_file(pm, "temp_ai.milk", false);

    std::vector<float> warmup(1024, 0.0f);
    projectm_pcm_add_float(pm, warmup.data(), 512, PROJECTM_STEREO);
    std::cout << "SUCCESS.\n" << std::flush;

    // STEP 9: Test First Frame Render
    std::cout << "[STEP 9/10] Testing first OpenGL frame render... " << std::flush;
    projectm_opengl_render_frame(pm);
    SDL_GL_SwapWindow(window);
    std::cout << "SUCCESS.\n" << std::flush;

    // STEP 10: Run Loop
    std::cout << "[STEP 10/10] All systems online!\n";
    std::cout << "======================================================\n";
    std::cout << " Hotkeys:\n";
    std::cout << "   [F1]  Save preset snapshot to ./save/\n";
    std::cout << "   [F2]  Mutate current preset via AI (Gemini 3.6 Pro)\n";
    std::cout << "   [F3]  Load another random preset from ./preset\n";
    std::cout << "   [ESC] Quit program\n";
    std::cout << "======================================================\n\n" << std::flush;

    bool running = true;
    SDL_Event event;
    const int buffer_frames = 1024;
    std::vector<float> audio_buffer(buffer_frames * spec.channels, 0.0f);

    while (running) {
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            }
            else if (event.type == SDL_EVENT_KEY_DOWN) {
                SDL_Keycode k = event.key.key;
                SDL_Scancode s = event.key.scancode;

                if (k == SDLK_ESCAPE || s == SDL_SCANCODE_ESCAPE) {
                    running = false;
                }
                else if (k == SDLK_F1 || s == SDL_SCANCODE_F1) {
                    SavePreset();
                }
                else if (k == SDLK_F2 || s == SDL_SCANCODE_F2) {
                    MutatePresetAsync();
                }
                else if (k == SDLK_F3 || s == SDL_SCANCODE_F3) {
                    LoadRandomPreset();
                }
            }
            else if (event.type == SDL_EVENT_WINDOW_RESIZED) {
                projectm_set_window_size(pm, event.window.data1, event.window.data2);
            }
        }

        // Apply newly generated preset once background thread finishes
        {
            std::lock_guard<std::mutex> lock(preset_mutex);
            if (!new_preset_pending.empty()) {
                current_preset_content = new_preset_pending;
                new_preset_pending.clear();

                try {
                    std::ofstream out("temp_ai.milk");
                    out << current_preset_content;
                    out.close();

                    projectm_load_preset_file(pm, "temp_ai.milk", false);
                }
                catch (...) {
                    std::cerr << "[WARN] Failed to apply pending AI preset into projectM.\n" << std::flush;
                }
            }
        }

        // Audio stream feeding
        if (decoder_initialized && stream) {
            int min_audio_bytes = (spec.freq * sizeof(float) * spec.channels) / 8;
            if (SDL_GetAudioStreamQueued(stream) < min_audio_bytes) {
                ma_uint64 framesRead = 0;
                ma_decoder_read_pcm_frames(&decoder, audio_buffer.data(), buffer_frames, &framesRead);

                if (framesRead == 0) {
                    ma_decoder_seek_to_pcm_frame(&decoder, 0); // Loop
                }
                else {
                    SDL_PutAudioStreamData(stream, audio_buffer.data(), static_cast<int>(framesRead * spec.channels * sizeof(float)));
                    projectm_pcm_add_float(pm, audio_buffer.data(), static_cast<unsigned int>(framesRead),
                        spec.channels == 2 ? PROJECTM_STEREO : PROJECTM_MONO);
                }
            }
        }

        // Render visualizer
        projectm_opengl_render_frame(pm);
        SDL_GL_SwapWindow(window);
    }

    // Cleanup
    std::cout << "\n[SHUTDOWN] Exiting cleanly...\n" << std::flush;
    if (stream) SDL_DestroyAudioStream(stream);
    if (decoder_initialized) ma_decoder_uninit(&decoder);
    curl_global_cleanup();
    projectm_destroy(pm);
    SDL_GL_DestroyContext(gl_ctx);
    SDL_DestroyWindow(window);
    SDL_Quit();

    WaitExit(0);
    return 0;
}