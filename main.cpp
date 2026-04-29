#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <malloc.h>
#include <curl/curl.h>

// spotify api info
const char* CLIENT_ID = "a";
const char* CLIENT_SECRET = "b";
const char* REFRESH_TOKEN = "c";

// global vars
char currentAccessToken[512] = "";
char lastStatus[256] = "Booting up...";

// playback data
char currentSong[128] = "Loading...";
char currentArtist[128] = "Loading...";
bool isPlaying = false;

// polling timer variables
u64 lastFetchTime = 0;
const u64 FETCH_INTERVAL = 5000; // 5 seconds

// network socket buffer size
#define SOC_ALIGN       0x1000
#define SOC_BUFFERSIZE  0x100000

// ui
struct Button {
    u16 x, y, width, height;
    const char* label;
};

// place buttons on bottom screen
Button btnPrev  = { 10, 100, 70, 60, "<< Prev" };
Button btnPause = { 90, 100, 60, 60, "Pause" };
Button btnPlay  = { 160, 100, 60, 60, "Play" };
Button btnNext  = { 230, 100, 70, 60, "Next >>" };

bool isTouchInside(touchPosition touch, Button btn) {
    return (touch.px >= btn.x && touch.px <= btn.x + btn.width &&
            touch.py >= btn.y && touch.py <= btn.y + btn.height);
}

// libcurl memory callback (for saving data)
struct MemoryStruct {
    char *memory;
    size_t size;
};

static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct MemoryStruct *mem = (struct MemoryStruct *)userp;

    char *ptr = (char*)realloc(mem->memory, mem->size + realsize + 1);
    if(!ptr) return 0;

    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;

    return realsize;
}

// libcurl dummy callback (to prevent printing to the screen)
static size_t WriteDummyCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    return size * nmemb; 
}

// Helper to find a boolean value in JSON, ignoring formatting
bool extractJsonBool(const char* json, const char* key) {
    char* keyPtr = strstr(json, key);
    if (!keyPtr) return false;

    char* start = keyPtr + strlen(key);
    
    while(*start == ' ' || *start == ':' || *start == '\n' || *start == '\r') start++;

    if (strncmp(start, "true", 4) == 0) return true;
    return false;
}

// smarter JSON extractor that skips spaces and colons
void extractJsonString(const char* json, const char* key, char* output, size_t maxLen) {
    char* keyPtr = strstr(json, key);
    if (!keyPtr) return;
    
    char* start = keyPtr + strlen(key);
    
    // Advance past spaces, colons, and the opening quote
    while(*start == ' ' || *start == ':') start++;
    if (*start == '\"') start++; 
    
    char* end = strchr(start, '\"');
    if (end) {
        size_t len = end - start;
        if (len >= maxLen) len = maxLen - 1;
        strncpy(output, start, len);
        output[len] = '\0';
    }
}

// extract token from JSON string
void extractAccessToken(const char* jsonResponse) {
    char tempToken[512] = "";
    extractJsonString(jsonResponse, "\"access_token\"", tempToken, sizeof(tempToken));
    
    if (strlen(tempToken) > 0) {
        strncpy(currentAccessToken, tempToken, sizeof(currentAccessToken));
        sprintf(lastStatus, "Token Refreshed!");
    } else {
        sprintf(lastStatus, "Error parsing token.");
    }
}

// fetch a new 1-hour auth token on boot
void refreshSpotifyToken() {
    CURL *curl;
    CURLcode res;
    
    struct MemoryStruct chunk;
    chunk.memory = (char*)malloc(1);  
    chunk.size = 0;

    curl = curl_easy_init();
    if(curl) {
        sprintf(lastStatus, "Requesting token...");
        
        curl_easy_setopt(curl, CURLOPT_URL, "https://accounts.spotify.com/api/token");
        
        char postData[1024];
        snprintf(postData, sizeof(postData), 
                 "grant_type=refresh_token&refresh_token=%s&client_id=%s&client_secret=%s", 
                 REFRESH_TOKEN, CLIENT_ID, CLIENT_SECRET);

        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postData);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
        
        // disable SSL checks for 3DS
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

        res = curl_easy_perform(curl);
        if(res == CURLE_OK) {
            extractAccessToken(chunk.memory);
        } else {
            sprintf(lastStatus, "Curl error: %s", curl_easy_strerror(res));
        }

        curl_easy_cleanup(curl);
        free(chunk.memory);
    }
}

// fetches the current playback status
void fetchCurrentlyPlaying() {
    if (strlen(currentAccessToken) == 0) return;

    CURL *curl;
    CURLcode res;
    struct MemoryStruct chunk;
    chunk.memory = (char*)malloc(1);  
    chunk.size = 0;

    curl = curl_easy_init();
    if(curl) {
        curl_easy_setopt(curl, CURLOPT_URL, "https://api.spotify.com/v1/me/player/currently-playing"); 

        struct curl_slist *headers = NULL;
        char authHeader[1024];
        snprintf(authHeader, sizeof(authHeader), "Authorization: Bearer %s", currentAccessToken);
        headers = curl_slist_append(headers, authHeader);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
        
        // Disable SSL checks for 3DS
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

        res = curl_easy_perform(curl);
        if(res == CURLE_OK) {
            long response_code;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
            
            if (response_code == 200 && chunk.size > 0) {
                // Robust boolean check
                isPlaying = extractJsonBool(chunk.memory, "\"is_playing\"");

                // Grab the artist
                char* artistsBlock = strstr(chunk.memory, "\"artists\"");
                if (artistsBlock) {
                    extractJsonString(artistsBlock, "\"name\"", currentArtist, sizeof(currentArtist));
                }

                // Grab the track (anchor to duration_ms to skip Album names)
                char* trackAnchor = strstr(chunk.memory, "\"duration_ms\"");
                if (trackAnchor) {
                    extractJsonString(trackAnchor, "\"name\"", currentSong, sizeof(currentSong));
                }
                
            } else if (response_code == 204) {
                // 204 No Content
                strcpy(currentSong, "Nothing playing");
                strcpy(currentArtist, "");
                isPlaying = false;
            }
        }
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        free(chunk.memory);
    }
}

// send playback commands
void sendSpotifyCommand(const char* endpoint, const char* method) {
    if (strlen(currentAccessToken) == 0) {
        sprintf(lastStatus, "No Token! Restart app.");
        return;
    }

    CURL *curl;
    CURLcode res;
    curl = curl_easy_init();
    
    if(curl) {
        char url[256];
        snprintf(url, sizeof(url), "https://api.spotify.com/v1/me/player%s", endpoint);
        curl_easy_setopt(curl, CURLOPT_URL, url);

        struct curl_slist *headers = NULL;
        char authHeader[1024];
        snprintf(authHeader, sizeof(authHeader), "Authorization: Bearer %s", currentAccessToken);
        headers = curl_slist_append(headers, authHeader);
        headers = curl_slist_append(headers, "Content-Length: 0");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteDummyCallback);
        
        // disable SSL checks for 3DS
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

        res = curl_easy_perform(curl);
        
        if(res == CURLE_OK) {
            long response_code;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
            if(response_code == 204 || response_code == 200) {
                sprintf(lastStatus, "Success: %s", endpoint);
                
                // Force a UI update 500ms from now
                lastFetchTime = osGetTime() - FETCH_INTERVAL + 500; 
            } else {
                sprintf(lastStatus, "API Error: %ld", response_code);
            }
        } else {
            sprintf(lastStatus, "Net Error: %s", curl_easy_strerror(res));
        }

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    }
}

// main
int main(int argc, char **argv) {
    gfxInitDefault();
    
    u32 *socBuffer = (u32*)memalign(SOC_ALIGN, SOC_BUFFERSIZE);
    if(R_FAILED(socInit(socBuffer, SOC_BUFFERSIZE))) {
        printf("Failed to initialize SOC service!\n");
    }
    
    curl_global_init(CURL_GLOBAL_DEFAULT);

    PrintConsole topScreen, bottomScreen;
    consoleInit(GFX_TOP, &topScreen);
    consoleInit(GFX_BOTTOM, &bottomScreen);

    consoleSelect(&topScreen);
    printf("\x1b[2J");
    consoleSelect(&bottomScreen);
    printf("\x1b[2J");

    refreshSpotifyToken();
    fetchCurrentlyPlaying();
    lastFetchTime = osGetTime();

    bool touchLock = false;

    while (aptMainLoop()) {
        hidScanInput();
        u32 kDown = hidKeysDown();
        u32 kHeld = hidKeysHeld();

        if (kDown & KEY_START) break;

        u64 currentTime = osGetTime();
        if (currentTime - lastFetchTime >= FETCH_INTERVAL) {
            fetchCurrentlyPlaying();
            lastFetchTime = currentTime;
        }

        if (kHeld & KEY_TOUCH) {
            if (!touchLock) {
                touchPosition touch;
                hidTouchRead(&touch);

                if (isTouchInside(touch, btnPrev)) {
                    sprintf(lastStatus, "Sending: Previous...");
                    sendSpotifyCommand("/previous", "POST");
                    touchLock = true;
                } 
                else if (isTouchInside(touch, btnPause)) {
                    sprintf(lastStatus, "Sending: Pause...");
                    sendSpotifyCommand("/pause", "PUT");
                    touchLock = true;
                } 
                else if (isTouchInside(touch, btnPlay)) {
                    sprintf(lastStatus, "Sending: Play...");
                    sendSpotifyCommand("/play", "PUT");
                    touchLock = true;
                }
                else if (isTouchInside(touch, btnNext)) {
                    sprintf(lastStatus, "Sending: Next...");
                    sendSpotifyCommand("/next", "POST");
                    touchLock = true;
                }
            }
        } else {
            touchLock = false;
        }

        consoleSelect(&topScreen);
        printf("\x1b[2;15H=== 3DSpotify ===");
        
        char displayToken[15] = "";
        if(strlen(currentAccessToken) > 0) {
            strncpy(displayToken, currentAccessToken, 10);
            strcat(displayToken, "...");
        } else {
            strcpy(displayToken, "None");
        }
        
        printf("\x1b[5;2HToken:  %-40s", displayToken);
        printf("\x1b[8;2HStatus: %-40s", isPlaying ? "[> Playing]" : "[|| Paused]");
        printf("\x1b[10;2HSong:   %-40s", currentSong);
        printf("\x1b[12;2HArtist: %-40s", currentArtist);
        
        printf("\x1b[28;2HPress START to exit.");

        consoleSelect(&bottomScreen);
        printf("\x1b[2;10H--- Controls ---");
        
        printf("\x1b[5;2HStatus: %-30s", lastStatus);

        printf("\x1b[13;2H[ Prev ]");
        printf("\x1b[13;12H[ Pause ]");
        printf("\x1b[13;22H[ Play ]");
        printf("\x1b[13;31H[ Next ]");

        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }

    curl_global_cleanup();
    socExit();
    free(socBuffer);
    gfxExit();
    
    return 0;
}
