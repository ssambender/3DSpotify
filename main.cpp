#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <malloc.h>
#include <stdarg.h>
#include <curl/curl.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

// spotify api info
const char* CLIENT_ID = "a";
const char* CLIENT_SECRET = "b";
const char* REFRESH_TOKEN = "c";

// global vars
char currentAccessToken[512] = "";
char lastStatus[256] = "Booting up...";
char debugUrl[128] = "No URL yet";

// playback data
char currentSong[128] = "Loading...";
char currentArtist[128] = "Loading...";
char currentSongId[128] = ""; 
bool isPlaying = false;
long currentProgressMs = 0;
long currentDurationMs = 0;
u64 lastSyncTime = 0;

// GPU image data
bool texLoaded = false;
C3D_Tex albumTex;
C2D_Image albumImg;
Tex3DS_SubTexture albumSubTex;
const int TARGET_IMG_SIZE = 200; 

// polling timer variables
u64 lastFetchTime = 0;
const u64 FETCH_INTERVAL = 5000;

#define SOC_ALIGN       0x1000
#define SOC_BUFFERSIZE  0x100000

// citro2d text buffer
C2D_TextBuf g_dynamicBuf;

// tab/window state
enum BottomTab { TAB_PLAYBACK, TAB_LIBRARY, TAB_SETTINGS };
BottomTab currentTab = TAB_PLAYBACK;

struct PlaylistItem {
    char name[64];
    char uri[128];
};

#define MAX_PLAYLISTS 50
PlaylistItem userPlaylists[MAX_PLAYLISTS];
int numPlaylists = 0;
bool libraryFetched = false;

// tracl scrolling position
int listScrollOffset = 0; 
const int VISIBLE_ITEMS = 6;

// ui
struct Button {
    u16 x, y, width, height;
    const char* label;
};

// playback buttons
Button btnPrev  = { 10,  100, 65, 80, "Prev" };
Button btnPause = { 85,  100, 65, 80, "Pause" };
Button btnPlay  = { 160, 100, 65, 80, "Play" };
Button btnNext  = { 235, 100, 65, 80, "Next" };

// tab buttons
Button btnTabPlayback = { 0,   210, 128, 30, "Controls" };
Button btnTabLibrary  = { 128, 210, 128, 30, "Library" };
Button btnTabSettings = { 256, 210, 64,  30, "Settings" };

bool isTouchInside(touchPosition touch, Button btn) {
    return (touch.px >= btn.x && touch.px <= btn.x + btn.width &&
            touch.py >= btn.y && touch.py <= btn.y + btn.height);
}

// libcurl memory callback
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

static size_t WriteDummyCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    return size * nmemb; 
}

// JSON utilities
bool extractJsonBool(const char* json, const char* key) {
    char* keyPtr = strstr(json, key);
    if (!keyPtr) return false;
    char* start = keyPtr + strlen(key);
    while(*start == ' ' || *start == ':' || *start == '\n' || *start == '\r') start++;
    if (strncmp(start, "true", 4) == 0) return true;
    return false;
}

long extractJsonLong(const char* json, const char* key) {
    char* keyPtr = strstr(json, key);
    if (!keyPtr) return 0;
    char* start = keyPtr + strlen(key);
    while(*start == ' ' || *start == ':') start++;
    return atol(start); 
}

void extractJsonString(const char* json, const char* key, char* output, size_t maxLen) {
    char* keyPtr = strstr(json, key);
    if (!keyPtr) return;
    char* start = keyPtr + strlen(key);
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

void unescapeUrl(char* url) {
    char* src = url;
    char* dst = url;
    while (*src) {
        if (*src == '\\' && *(src + 1) == '/') {
            src++; 
        }
        *dst++ = *src++;
    }
    *dst = '\0';
}

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
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(curl, CURLOPT_FORBID_REUSE, 1L);

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

void fetchPlaylists() {
    if (strlen(currentAccessToken) == 0) {
        sprintf(lastStatus, "No Token!");
        libraryFetched = true;
        return;
    }

    sprintf(lastStatus, "Fetching Library...");
    
    CURL *curl = curl_easy_init();
    struct MemoryStruct chunk;
    chunk.memory = (char*)malloc(1);  
    chunk.size = 0;

    if(curl) {
        curl_easy_setopt(curl, CURLOPT_URL, "https://api.spotify.com/v1/me/playlists"); 

        struct curl_slist *headers = NULL;
        char authHeader[1024];
        snprintf(authHeader, sizeof(authHeader), "Authorization: Bearer %s", currentAccessToken);
        headers = curl_slist_append(headers, authHeader);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L); 
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

        CURLcode res = curl_easy_perform(curl);
        if(res == CURLE_OK && chunk.size > 0) {
            long response_code;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
            
            if (response_code == 200) {
                numPlaylists = 0;
                listScrollOffset = 0; // reset scroll position on fetch new
                char* itemPtr = strstr(chunk.memory, "\"items\"");
                
                while (itemPtr != NULL && numPlaylists < MAX_PLAYLISTS) {
                    char* namePtr = strstr(itemPtr, "\"name\"");
                    if (!namePtr) break;
                    
                    char* uriPtr = strstr(namePtr, "spotify:playlist:");
                    if (!uriPtr) break;

                    extractJsonString(namePtr, "\"name\"", userPlaylists[numPlaylists].name, sizeof(userPlaylists[0].name));
                    
                    char* quoteEnd = strchr(uriPtr, '\"');
                    if (quoteEnd) {
                        int len = quoteEnd - uriPtr;
                        if (len > 127) len = 127;
                        strncpy(userPlaylists[numPlaylists].uri, uriPtr, len);
                        userPlaylists[numPlaylists].uri[len] = '\0';
                    }

                    numPlaylists++;
                    itemPtr = uriPtr + 17; 
                }
                sprintf(lastStatus, "Library loaded: %d", numPlaylists);
            } else {
                sprintf(lastStatus, "Lib API Err: %ld", response_code);
            }
        } else {
            sprintf(lastStatus, "Lib Net Err: %d", res);
        }
        
        libraryFetched = true; 
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    }
    free(chunk.memory);
}

u32 morton_index(u32 x, u32 y, u32 width) {
    u32 i = (x & 7) | ((y & 7) << 8); 
    i = (i ^ (i << 2)) & 0x1313;
    i = (i ^ (i << 1)) & 0x1515;
    i = (i | ((i >> 7) & 0x3E)) & 0x3F;
    return i + ((x >> 3) * 64) + ((y >> 3) * 64 * (width >> 3));
}

void fetchCoverArt(const char* url) {
    sprintf(lastStatus, "Fetching Art...");
    
    CURL *curl = curl_easy_init();
    if(!curl) return;

    struct MemoryStruct chunk;
    chunk.memory = (char*)malloc(1);
    chunk.size = 0;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_FORBID_REUSE, 1L);

    CURLcode res = curl_easy_perform(curl);
    if(res == CURLE_OK && chunk.size > 0) {
        
        int origW, origH, channels;
        // decode to 4 channel to map to GPU VRAM
        u8* rawPixels = stbi_load_from_memory((const stbi_uc*)chunk.memory, chunk.size, &origW, &origH, &channels, 4);
        
        if (rawPixels) {
            
            // ensure image is power of 2 tex
            if (!texLoaded) {
                C3D_TexInit(&albumTex, 256, 256, GPU_RGBA8);
                C3D_TexSetFilter(&albumTex, GPU_LINEAR, GPU_NEAREST);
                texLoaded = true;
            }
            
            u32* vramTexData = (u32*)albumTex.data;
            memset(vramTexData, 0, 256 * 256 * 4);
            
            // downscale and save to gpu memory
            for (int y = 0; y < TARGET_IMG_SIZE; y++) {
                for (int x = 0; x < TARGET_IMG_SIZE; x++) {
                    int srcX = (x * origW) / TARGET_IMG_SIZE;
                    int srcY = (y * origH) / TARGET_IMG_SIZE;
                    
                    int srcIdx = (srcY * origW + srcX) * 4;
                    
                    u8 r = rawPixels[srcIdx + 0];
                    u8 g = rawPixels[srcIdx + 1];
                    u8 b = rawPixels[srcIdx + 2];
                    u8 a = rawPixels[srcIdx + 3];
                    
                    // citro3d GPU_RGBA8
                    u32 color = (r << 24) | (g << 16) | (b << 8) | a;
                    vramTexData[morton_index(x, y, 256)] = color;
                }
            }

            // define visual wrapper
            albumSubTex.width = TARGET_IMG_SIZE;
            albumSubTex.height = TARGET_IMG_SIZE;
            albumSubTex.left = 0.0f;
            albumSubTex.top = 1.0f; 
            albumSubTex.right = TARGET_IMG_SIZE / 256.0f;
            albumSubTex.bottom = 1.0f - (TARGET_IMG_SIZE / 256.0f);

            albumImg.tex = &albumTex;
            albumImg.subtex = &albumSubTex;

            stbi_image_free(rawPixels);
            sprintf(lastStatus, "Art updated successfully");
        } else {
            sprintf(lastStatus, "Art decode failed");
        }
    } else {
        sprintf(lastStatus, "Art DL error: %d", res);
    }

    curl_easy_cleanup(curl);
    free(chunk.memory);
}

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
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(curl, CURLOPT_FORBID_REUSE, 1L);

        res = curl_easy_perform(curl);
        if(res == CURLE_OK) {
            long response_code;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
            
            if (response_code == 200 && chunk.size > 0) {
                isPlaying = extractJsonBool(chunk.memory, "\"is_playing\"");
                
                currentProgressMs = extractJsonLong(chunk.memory, "\"progress_ms\"");
                lastSyncTime = osGetTime();

                char* artistsBlock = strstr(chunk.memory, "\"artists\"");
                if (artistsBlock) {
                    extractJsonString(artistsBlock, "\"name\"", currentArtist, sizeof(currentArtist));
                }

                char* trackAnchor = strstr(chunk.memory, "\"duration_ms\"");
                if (trackAnchor) {
                    currentDurationMs = extractJsonLong(trackAnchor, "\"duration_ms\"");
                    
                    char newSongId[128] = "";
                    extractJsonString(trackAnchor, "\"id\"", newSongId, sizeof(newSongId));
                    extractJsonString(trackAnchor, "\"name\"", currentSong, sizeof(currentSong));

                    if (strlen(newSongId) > 0 && strcmp(newSongId, currentSongId) != 0) {
                        strcpy(currentSongId, newSongId);

                        char* albumBlock = strstr(chunk.memory, "\"album\"");
                        char* imagesBlock = albumBlock ? strstr(albumBlock, "\"images\"") : NULL;
                        
                        char newCoverUrl[512] = "";
                        if (imagesBlock) {
                            char* current = imagesBlock;
                            char* targetUrl = NULL;
                            int urlCount = 0;
                            char* endArray = strchr(imagesBlock, ']');
                            
                            while ((current = strstr(current, "\"url\"")) != NULL && (!endArray || current < endArray)) {
                                urlCount++;
                                if (urlCount == 2) { 
                                    targetUrl = current;
                                    break;
                                }
                                current += 5; 
                            }
                            
                            if (!targetUrl && urlCount == 1) {
                                targetUrl = strstr(imagesBlock, "\"url\"");
                            }
                            
                            if (targetUrl) {
                                extractJsonString(targetUrl, "\"url\"", newCoverUrl, sizeof(newCoverUrl));
                                unescapeUrl(newCoverUrl); 
                            }
                        }

                        if (strlen(newCoverUrl) > 0) {
                            snprintf(debugUrl, sizeof(debugUrl), "%.46s...", newCoverUrl);
                            fetchCoverArt(newCoverUrl);
                        } else {
                            strcpy(debugUrl, "None found");
                            sprintf(lastStatus, "No Art URL found");
                        }
                    }
                }
                
            } else if (response_code == 204) {
                strcpy(currentSong, "Nothing playing");
                strcpy(currentArtist, "");
                strcpy(currentSongId, "");
                isPlaying = false;
                currentProgressMs = 0;
                currentDurationMs = 0;
            }
        } else {
             sprintf(lastStatus, "Net Err: %d", res);
        }
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        free(chunk.memory);
    }
}

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
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(curl, CURLOPT_FORBID_REUSE, 1L);

        res = curl_easy_perform(curl);
        
        if(res == CURLE_OK) {
            long response_code;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
            if(response_code == 204 || response_code == 200) {
                sprintf(lastStatus, "Success: %s", endpoint);
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

void playContext(const char* contextUri) {
    if (strlen(currentAccessToken) == 0) return;

    sprintf(lastStatus, "Starting playlist...");
    
    CURL *curl = curl_easy_init();
    if(curl) {
        curl_easy_setopt(curl, CURLOPT_URL, "https://api.spotify.com/v1/me/player/play");

        struct curl_slist *headers = NULL;
        char authHeader[1024];
        snprintf(authHeader, sizeof(authHeader), "Authorization: Bearer %s", currentAccessToken);
        headers = curl_slist_append(headers, authHeader);
        headers = curl_slist_append(headers, "Content-Type: application/json");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        char bodyData[256];
        snprintf(bodyData, sizeof(bodyData), "{\"context_uri\":\"%s\"}", contextUri);

        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, bodyData);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteDummyCallback);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

        CURLcode res = curl_easy_perform(curl);
        
        if(res == CURLE_OK) {
            long response_code;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
            if(response_code == 204 || response_code == 200) {
                sprintf(lastStatus, "Playlist Started!");
                isPlaying = true;
                lastFetchTime = osGetTime() - FETCH_INTERVAL + 500; 
            } else {
                sprintf(lastStatus, "Play Error: %ld", response_code);
            }
        } else {
            sprintf(lastStatus, "Net Error: %s", curl_easy_strerror(res));
        }

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    }
}

// draw dynamic text w/ citro2d
void DrawText(float x, float y, float size, u32 color, const char* format, ...) {
    char buffer[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    
    C2D_Text textObj;
    C2D_TextParse(&textObj, g_dynamicBuf, buffer);
    C2D_TextOptimize(&textObj);
    C2D_DrawText(&textObj, C2D_WithColor, x, y, 0.5f, size, size, color);
}

// draw borders w/ citro2d
void DrawBorderC2D(float x, float y, float w, float h, float t, u32 color) {
    C2D_DrawRectSolid(x, y, 0.5f, w, t, color); // top
    C2D_DrawRectSolid(x, y + h - t, 0.5f, w, t, color); // bottom
    C2D_DrawRectSolid(x, y, 0.5f, t, h, color); // left
    C2D_DrawRectSolid(x + w - t, y, 0.5f, t, h, color); // right
}

int main(int argc, char **argv) {
    // init standard services
    gfxInitDefault();
    u32 *socBuffer = (u32*)memalign(SOC_ALIGN, SOC_BUFFERSIZE);
    if(R_FAILED(socInit(socBuffer, SOC_BUFFERSIZE))) {
        printf("Failed to initialize SOC service!\n");
    }
    curl_global_init(CURL_GLOBAL_DEFAULT);

    // init GPU and 2D graphics engine
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();

    // create rendering targets for both screens
    C3D_RenderTarget* topTarget = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    C3D_RenderTarget* bottomTarget = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);

    // create text buffer
    g_dynamicBuf = C2D_TextBufNew(4096);

    // HEX to citro2d colors
    u32 clrBg       = C2D_Color32(18, 18, 18, 255);    
    u32 clrGreen    = C2D_Color32(29, 185, 84, 255);   
    u32 clrBtnBg    = C2D_Color32(33, 33, 33, 255);    
    u32 clrDisabled = C2D_Color32(85, 85, 85, 255);    
    u32 clrHighlight = C2D_Color32(50, 50, 50, 255);   

    refreshSpotifyToken();
    fetchCurrentlyPlaying();
    lastFetchTime = osGetTime();

    bool touchLock = false;

    while (aptMainLoop()) {
        hidScanInput();
        u32 kDown = hidKeysDown();
        u32 kHeld = hidKeysHeld();

        if (kDown & KEY_START) break;

        // tab switching
        if (kDown & KEY_R) { // RIGHT
            if (currentTab == TAB_PLAYBACK) currentTab = TAB_LIBRARY;
            else if (currentTab == TAB_LIBRARY) currentTab = TAB_SETTINGS;
            else if (currentTab == TAB_SETTINGS) currentTab = TAB_PLAYBACK;
        }
        if (kDown & KEY_L) { // LEFT
            if (currentTab == TAB_PLAYBACK) currentTab = TAB_SETTINGS;
            else if (currentTab == TAB_LIBRARY) currentTab = TAB_PLAYBACK;
            else if (currentTab == TAB_SETTINGS) currentTab = TAB_LIBRARY;
        }

        // fetch playlist on library tab
        if ((kDown & KEY_L) || (kDown & KEY_R)) {
            if (currentTab == TAB_LIBRARY && (!libraryFetched || numPlaylists == 0)) {
                fetchPlaylists(); 
            }
        }

        // scroll list
        if (currentTab == TAB_LIBRARY && numPlaylists > VISIBLE_ITEMS) {
            int maxOffset = numPlaylists - VISIBLE_ITEMS;
            
            // D-PAD DOWN
            if ((kDown & KEY_DDOWN) || (kDown & KEY_CPAD_DOWN)) {
                if (listScrollOffset < maxOffset) {
                    listScrollOffset++;
                } else {
                    listScrollOffset = 0; // loop back to top
                }
            }
            // D-PAD UP
            if ((kDown & KEY_DUP) || (kDown & KEY_CPAD_UP)) {
                if (listScrollOffset > 0) {
                    listScrollOffset--;
                } else {
                    listScrollOffset = maxOffset; // loop to bottom
                }
            }
            // D-PAD RIGHT
            if ((kDown & KEY_DRIGHT) || (kDown & KEY_CPAD_RIGHT)) {
                if (listScrollOffset == maxOffset) {
                    listScrollOffset = 0; // loop back to top
                } else {
                    listScrollOffset += VISIBLE_ITEMS;
                    if (listScrollOffset > maxOffset) listScrollOffset = maxOffset;
                }
            }
            // D-PAD LEFT
            if ((kDown & KEY_DLEFT) || (kDown & KEY_CPAD_LEFT)) {
                if (listScrollOffset == 0) {
                    listScrollOffset = maxOffset; // loop to bottom
                } else {
                    listScrollOffset -= VISIBLE_ITEMS;
                    if (listScrollOffset < 0) listScrollOffset = 0;
                }
            }
        }

        u64 currentTime = osGetTime();
        if (currentTime - lastFetchTime >= FETCH_INTERVAL) {
            fetchCurrentlyPlaying();
            lastFetchTime = currentTime;
        }

        if (kHeld & KEY_TOUCH) {
            if (!touchLock) {
                touchPosition touch;
                hidTouchRead(&touch);

                // tab switching
                if (isTouchInside(touch, btnTabPlayback)) {
                    currentTab = TAB_PLAYBACK;
                    touchLock = true;
                }
                else if (isTouchInside(touch, btnTabLibrary)) {
                    currentTab = TAB_LIBRARY;
                    if (!libraryFetched || numPlaylists == 0) {
                        fetchPlaylists(); 
                    }
                    touchLock = true;
                }
                else if (isTouchInside(touch, btnTabSettings)) {
                    currentTab = TAB_SETTINGS;
                    touchLock = true;
                }

                // playback controls tab
                if (currentTab == TAB_PLAYBACK && touchLock == false) {
                    if (isTouchInside(touch, btnPrev)) {
                        sprintf(lastStatus, "Sending: Previous...");
                        currentProgressMs = 0; 
                        lastSyncTime = osGetTime();
                        sendSpotifyCommand("/previous", "POST");
                        touchLock = true;
                    } 
                    else if (isTouchInside(touch, btnPause) && isPlaying) {
                        sprintf(lastStatus, "Sending: Pause...");
                        isPlaying = false;
                        currentProgressMs += (osGetTime() - lastSyncTime); 
                        lastSyncTime = osGetTime();
                        sendSpotifyCommand("/pause", "PUT");
                        touchLock = true;
                    } 
                    else if (isTouchInside(touch, btnPlay) && !isPlaying) {
                        sprintf(lastStatus, "Sending: Play...");
                        isPlaying = true;
                        lastSyncTime = osGetTime(); 
                        sendSpotifyCommand("/play", "PUT");
                        touchLock = true;
                    }
                    else if (isTouchInside(touch, btnNext)) {
                        sprintf(lastStatus, "Sending: Next...");
                        currentProgressMs = 0; 
                        lastSyncTime = osGetTime();
                        sendSpotifyCommand("/next", "POST");
                        touchLock = true;
                    }
                }
                
                // library/playlist tab
                if (currentTab == TAB_LIBRARY && touchLock == false) {
                    int displayCount = (numPlaylists - listScrollOffset < VISIBLE_ITEMS) ? (numPlaylists - listScrollOffset) : VISIBLE_ITEMS;
                    for (int i = 0; i < displayCount; i++) {
                        Button row = {10, (u16)(45 + (i * 26)), 300, 22, ""};
                        if (isTouchInside(touch, row)) {
                            int actualDataIndex = listScrollOffset + i;
                            playContext(userPlaylists[actualDataIndex].uri);
                            touchLock = true;
                            currentTab = TAB_PLAYBACK; 
                            break;
                        }
                    }
                }
            }
        } else {
            touchLock = false;
        }

        // rendering
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        C2D_TextBufClear(g_dynamicBuf); 

        // ============= TOP SCREEN =================
        C2D_TargetClear(topTarget, clrBg);
        C2D_SceneBegin(topTarget);
        
        char displayToken[15] = "";
        if(strlen(currentAccessToken) > 0) {
            snprintf(displayToken, sizeof(displayToken), "%.10s...", currentAccessToken);
        } else {
            strcpy(displayToken, "None");
        }

        char progressDisplay[32] = "0:00 / 0:00";
        if (currentDurationMs > 0) {
            long displayMs = currentProgressMs;
            if (isPlaying) displayMs += (osGetTime() - lastSyncTime);
            if (displayMs > currentDurationMs) displayMs = currentDurationMs; 

            int p_sec = (displayMs / 1000) % 60;
            int p_min = (displayMs / 1000) / 60;
            int d_sec = (currentDurationMs / 1000) % 60;
            int d_min = (currentDurationMs / 1000) / 60;

            sprintf(progressDisplay, "%d:%02d / %d:%02d", p_min, p_sec, d_min, d_sec);
        }
        
        DrawText(10, 10,  1f, clrGreen, "3DSpotify");
        DrawText(10, 35,  0.5f, clrGreen, "Token: %.18s", displayToken);
        DrawText(10, 60,  0.5f, clrGreen, "Status: %s", isPlaying ? "> Playing" : "|| Paused");
        DrawText(10, 85,  0.5f, clrGreen, "Time: %s", progressDisplay);
        
        DrawText(10, 110, 0.5f, clrGreen, "Song: %.22s", currentSong);
        DrawText(10, 135, 0.5f, clrGreen, "Artist: %.22s", currentArtist);
        DrawText(10, 210, 0.5f, clrDisabled, "Press START to exit.");

        if (texLoaded) {
            C2D_DrawImageAt(albumImg, 190.0f, 20.0f, 0.5f, NULL, 1.0f, 1.0f);
        }

        // ============= BOTTOM SCREEN =================
        C2D_TargetClear(bottomTarget, clrBg);
        C2D_SceneBegin(bottomTarget);
        
        if (currentTab == TAB_PLAYBACK) {
            DrawText(10, 10, 0.6f, clrGreen, "Controls");
            DrawText(10, 40, 0.5f, clrGreen, "Status: %.35s", lastStatus);
            DrawText(10, 60, 0.5f, clrGreen, "Art URL: %.35s", debugUrl);

            C2D_DrawRectSolid(btnPrev.x, btnPrev.y, 0.5f, btnPrev.width, btnPrev.height, clrBtnBg);
            DrawBorderC2D(btnPrev.x, btnPrev.y, btnPrev.width, btnPrev.height, 2.0f, clrGreen);
            DrawText(btnPrev.x + 12, btnPrev.y + 32, 0.55f, clrGreen, btnPrev.label);

            C2D_DrawRectSolid(btnPause.x, btnPause.y, 0.5f, btnPause.width, btnPause.height, clrBtnBg);
            DrawBorderC2D(btnPause.x, btnPause.y, btnPause.width, btnPause.height, 2.0f, isPlaying ? clrGreen : clrDisabled);
            DrawText(btnPause.x + 10, btnPause.y + 32, 0.55f, isPlaying ? clrGreen : clrDisabled, btnPause.label);

            C2D_DrawRectSolid(btnPlay.x, btnPlay.y, 0.5f, btnPlay.width, btnPlay.height, clrBtnBg);
            DrawBorderC2D(btnPlay.x, btnPlay.y, btnPlay.width, btnPlay.height, 2.0f, !isPlaying ? clrGreen : clrDisabled);
            DrawText(btnPlay.x + 15, btnPlay.y + 32, 0.55f, !isPlaying ? clrGreen : clrDisabled, btnPlay.label);

            C2D_DrawRectSolid(btnNext.x, btnNext.y, 0.5f, btnNext.width, btnNext.height, clrBtnBg);
            DrawBorderC2D(btnNext.x, btnNext.y, btnNext.width, btnNext.height, 2.0f, clrGreen);
            DrawText(btnNext.x + 15, btnNext.y + 32, 0.55f, clrGreen, btnNext.label);
            
        } else if (currentTab == TAB_LIBRARY) {
            DrawText(10, 10, 0.6f, clrGreen, "Your Playlists");
            
            if (!libraryFetched) {
                DrawText(10, 50, 0.5f, clrDisabled, "Loading...");
            } else if (numPlaylists == 0) {
                DrawText(10, 50, 0.5f, clrDisabled, "No playlists found. Tap tab to retry.");
            } else {
                // calculate which playlists on screen
                int displayCount = (numPlaylists - listScrollOffset < VISIBLE_ITEMS) ? (numPlaylists - listScrollOffset) : VISIBLE_ITEMS;

                if (numPlaylists > VISIBLE_ITEMS) {
                    DrawText(150, 10, 0.4f, clrDisabled, "(Up/Down: Scroll)"); 
                    // range
                    DrawText(220, 25, 0.45f, clrGreen, "%d-%d / %d", listScrollOffset + 1, listScrollOffset + displayCount, numPlaylists);
                }

                for (int i = 0; i < displayCount; i++) {
                    int actualDataIndex = listScrollOffset + i;
                    int yPos = 45 + (i * 26); 
                    C2D_DrawRectSolid(10, yPos, 0.5f, 300, 22, clrBtnBg);
                    DrawText(15, yPos + 3, 0.45f, clrGreen, "> %.40s", userPlaylists[actualDataIndex].name);
                }
            }
        } else if (currentTab == TAB_SETTINGS) {
            // settings tab
            DrawText(10, 10, 0.6f, clrGreen, "Settings");
            DrawText(10, 50, 0.5f, clrDisabled, "Settings coming soon...");
            
            // placeholder button
            Button btnDummy = { 10, 80, 200, 30, "Clear Cache (WIP)" };
            C2D_DrawRectSolid(btnDummy.x, btnDummy.y, 0.5f, btnDummy.width, btnDummy.height, clrBtnBg);
            DrawBorderC2D(btnDummy.x, btnDummy.y, btnDummy.width, btnDummy.height, 2.0f, clrDisabled);
            DrawText(btnDummy.x + 10, btnDummy.y + 7, 0.5f, clrDisabled, btnDummy.label);
        }

        // render tabs
        C2D_DrawRectSolid(btnTabPlayback.x, btnTabPlayback.y, 0.5f, btnTabPlayback.width, btnTabPlayback.height, currentTab == TAB_PLAYBACK ? clrHighlight : clrBtnBg);
        DrawBorderC2D(btnTabPlayback.x, btnTabPlayback.y, btnTabPlayback.width, btnTabPlayback.height, 1.0f, clrDisabled);
        DrawText(btnTabPlayback.x + 35, btnTabPlayback.y + 8, 0.5f, currentTab == TAB_PLAYBACK ? clrGreen : clrDisabled, btnTabPlayback.label);
        
        C2D_DrawRectSolid(btnTabLibrary.x, btnTabLibrary.y, 0.5f, btnTabLibrary.width, btnTabLibrary.height, currentTab == TAB_LIBRARY ? clrHighlight : clrBtnBg);
        DrawBorderC2D(btnTabLibrary.x, btnTabLibrary.y, btnTabLibrary.width, btnTabLibrary.height, 1.0f, clrDisabled);
        DrawText(btnTabLibrary.x + 40, btnTabLibrary.y + 8, 0.5f, currentTab == TAB_LIBRARY ? clrGreen : clrDisabled, btnTabLibrary.label);

        C2D_DrawRectSolid(btnTabSettings.x, btnTabSettings.y, 0.5f, btnTabSettings.width, btnTabSettings.height, currentTab == TAB_SETTINGS ? clrHighlight : clrBtnBg);
        DrawBorderC2D(btnTabSettings.x, btnTabSettings.y, btnTabSettings.width, btnTabSettings.height, 1.0f, clrDisabled);
        DrawText(btnTabSettings.x + 10, btnTabSettings.y + 10, 0.45f, currentTab == TAB_SETTINGS ? clrGreen : clrDisabled, btnTabSettings.label);
        
        C3D_FrameEnd(0);
    }

    if (texLoaded) {
        C3D_TexDelete(&albumTex); 
    }
    
    C2D_TextBufDelete(g_dynamicBuf);
    C2D_Fini();
    C3D_Fini();
    
    curl_global_cleanup();
    socExit();
    free(socBuffer);
    gfxExit();
    
    return 0;
}
