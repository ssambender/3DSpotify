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
// todo - replace above with a QR code based login to save user tokens

// multithreading vars
Thread networkThread;
LightLock dataLock;
bool exitThread = false;

enum NetCommand {
    CMD_NONE,
    CMD_FETCH_PLAYING,
    CMD_PLAY,
    CMD_PAUSE,
    CMD_NEXT,
    CMD_PREV,
    CMD_FETCH_PLAYLISTS,
    CMD_PLAY_CONTEXT
};

NetCommand currentCommand = CMD_NONE;
bool isRequestPending = false;
char commandUri[128] = "";

u32* tempArtBuffer = NULL;
bool newArtReady = false;

// legacy global vars
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

// album cover art GPU image data
bool texLoaded = false;
C3D_Tex albumTex;
C2D_Image albumImg;
Tex3DS_SubTexture albumSubTex;
const int TARGET_IMG_SIZE = 200; 

// Tab images
C3D_Tex tabTexPlayback, tabTexLibrary, tabTexSettings;
C2D_Image imgTabPlayback, imgTabLibrary, imgTabSettings;
bool tabImagesLoaded = false;
Tex3DS_SubTexture tabSubTex = { 108, 60, 10.0f / 128.0f, 1.0f - 4.0f / 64.0f, 118.0f / 128.0f, 0.0f };

// control button
C3D_Tex texControlPlay, texControlPause, texControlNext, texControlPrev;
C3D_Tex texControlPlay2, texControlPause2, texControlNext2, texControlPrev2;
C2D_Image imgControlPlay, imgControlPause, imgControlNext, imgControlPrev;
C2D_Image imgControlPlay2, imgControlPause2, imgControlNext2, imgControlPrev2;
bool controlImagesLoaded = false;
Tex3DS_SubTexture controlSubTex = { 110, 64, 9.0f / 128.0f, 1.0f, 119.0f / 128.0f, 0.0f };

// folder icon
C3D_Tex texPlaylistBar;
C2D_Image imgPlaylistBar;
Tex3DS_SubTexture subTexPlaylistBar;
bool libraryUiLoaded = false;

// button timers for texture changeback
u64 prevClickedTime = 0;
u64 nextClickedTime = 0;
u64 playClickedTime = 0;
u64 pauseClickedTime = 0;

// cover art border
C3D_Tex texBorder;
C2D_Image imgBorder;
bool borderLoaded = false;
Tex3DS_SubTexture borderSubTex = { 171, 171, 0.0f, 1.0f, 171.0f / 256.0f, (256.0f - 171.0f) / 256.0f };

// polling timer variables
u64 lastFetchTime = 0;
const u64 FETCH_INTERVAL = 5000;

#define SOC_ALIGN       0x1000
#define SOC_BUFFERSIZE  0x100000

// citro2d text buffers
C2D_TextBuf g_dynamicBuf;
C2D_TextBuf g_staticBuf;
C2D_Font fontFranxurter;

// Pre-parsed static text objects
C2D_Text txtTitlePlaylists, txtTitleSettings;
C2D_Text txtLoading, txtNoPlaylists;

// marquee text vars
float marqueeOffset = 0.0f;
u64 lastMarqueeTime = 0;
const float MARQUEE_SPEED = 0.5f; // how many px per frame to scroll/move text
const float MARQUEE_WAIT_MS = 2000; // how long to pause text before scrollibng
u64 marqueeWaitTimer = 0;
bool marqueePausedAtEnd = false;

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

// track smooth scrolling position
float listScrollPixels = 0.0f; 
bool isListDragging = false;
bool dragDidMove = false;
s16 startTouchY = 0;
s16 lastTouchY = 0;

const float ROW_HEIGHT = 34.0f;
const float LIST_START_Y = 3.0f;
const float LIST_END_Y = 190.0f;
const float VISIBLE_HEIGHT = LIST_END_Y - LIST_START_Y;

// ui
struct Button {
    s16 x, y, width, height;
    const char* label;
};

// playback buttons
Button btnPlay  = { 40,  26,  110, 62, "Play" };
Button btnPause = { 170, 26,  110, 62, "Pause" };
Button btnPrev  = { 40,  106, 110, 62, "Prev" };
Button btnNext  = { 170, 106, 110, 62, "Next" };

// tab buttons
Button btnTabPlayback = { -1,   190, 106, 45, "Controls" };
Button btnTabLibrary  = { 106, 190, 107, 45, "Library" };
Button btnTabSettings = { 213, 190, 107, 45, "Settings" };

bool isTouchInside(touchPosition touch, Button btn) {
    return (touch.px >= btn.x && touch.px <= btn.x + btn.width &&
            touch.py >= btn.y && touch.py <= btn.y + btn.height);
}

// Thread-safe status updater
void updateStatus(const char* format, ...) {
    LightLock_Lock(&dataLock);
    va_list args;
    va_start(args, format);
    vsnprintf(lastStatus, sizeof(lastStatus), format, args);
    va_end(args);
    LightLock_Unlock(&dataLock);
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
        updateStatus("Token Refreshed!");
    } else {
        updateStatus("Error parsing token.");
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
        updateStatus("Requesting token...");
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
            updateStatus("Curl error: %s", curl_easy_strerror(res));
        }

        curl_easy_cleanup(curl);
        free(chunk.memory);
    }
}

void fetchPlaylists() {
    if (strlen(currentAccessToken) == 0) {
        updateStatus("No Token!");
        LightLock_Lock(&dataLock);
        libraryFetched = true;
        LightLock_Unlock(&dataLock);
        return;
    }

    updateStatus("Fetching Library...");
    
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
                PlaylistItem tempPlaylists[MAX_PLAYLISTS];
                int tempNum = 0;

                char* itemPtr = strstr(chunk.memory, "\"items\"");
                
                while (itemPtr != NULL && tempNum < MAX_PLAYLISTS) {
                    char* namePtr = strstr(itemPtr, "\"name\"");
                    if (!namePtr) break;
                    
                    char* uriPtr = strstr(namePtr, "spotify:playlist:");
                    if (!uriPtr) break;

                    extractJsonString(namePtr, "\"name\"", tempPlaylists[tempNum].name, sizeof(tempPlaylists[0].name));
                    
                    char* quoteEnd = strchr(uriPtr, '\"');
                    if (quoteEnd) {
                        int len = quoteEnd - uriPtr;
                        if (len > 127) len = 127;
                        strncpy(tempPlaylists[tempNum].uri, uriPtr, len);
                        tempPlaylists[tempNum].uri[len] = '\0';
                    }

                    tempNum++;
                    itemPtr = uriPtr + 17; 
                }

                LightLock_Lock(&dataLock);
                numPlaylists = tempNum;
                listScrollPixels = 0.0f; // reset scroll on load
                for(int i=0; i<tempNum; i++){
                    userPlaylists[i] = tempPlaylists[i];
                }
                libraryFetched = true;
                LightLock_Unlock(&dataLock);

                updateStatus("Library loaded: %d", tempNum);
            } else {
                updateStatus("Lib API Err: %ld", response_code);
            }
        } else {
            updateStatus("Lib Net Err: %d", res);
        }
        
        LightLock_Lock(&dataLock);
        libraryFetched = true; 
        LightLock_Unlock(&dataLock);

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
    updateStatus("Fetching Art...");
    
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
            u32* tempVramData = (u32*)linearAlloc(256 * 256 * 4);
            if (tempVramData) {
                memset(tempVramData, 0, 256 * 256 * 4);
                
                // downscale and swizzle for GPU memory
                for (int y = 0; y < TARGET_IMG_SIZE; y++) {
                    for (int x = 0; x < TARGET_IMG_SIZE; x++) {
                        int srcX = (x * origW) / TARGET_IMG_SIZE;
                        int srcY = (y * origH) / TARGET_IMG_SIZE;
                        int srcIdx = (srcY * origW + srcX) * 4;
                        
                        u8 r = rawPixels[srcIdx + 0];
                        u8 g = rawPixels[srcIdx + 1];
                        u8 b = rawPixels[srcIdx + 2];
                        u8 a = rawPixels[srcIdx + 3];
                        
                        u32 color = (r << 24) | (g << 16) | (b << 8) | a;
                        tempVramData[morton_index(x, y, 256)] = color;
                    }
                }

                stbi_image_free(rawPixels);

                LightLock_Lock(&dataLock);
                if (tempArtBuffer) linearFree(tempArtBuffer);
                tempArtBuffer = tempVramData;
                newArtReady = true;
                LightLock_Unlock(&dataLock);
                
                updateStatus("Art decoded, ready for GPU");
            } else {
                stbi_image_free(rawPixels);
                updateStatus("LinearAlloc failed for art");
            }
        } else {
            updateStatus("Art decode failed");
        }
    } else {
        updateStatus("Art DL error: %d", res);
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
                bool localIsPlaying = extractJsonBool(chunk.memory, "\"is_playing\"");
                long localProgress = extractJsonLong(chunk.memory, "\"progress_ms\"");
                char localArtist[128] = "";
                char localSong[128] = "";
                long localDuration = 0;
                char newSongId[128] = "";
                
                char* artistsBlock = strstr(chunk.memory, "\"artists\"");
                if (artistsBlock) extractJsonString(artistsBlock, "\"name\"", localArtist, sizeof(localArtist));
                
                bool shouldFetchArt = false;
                char targetUrl[512] = "";

                char* trackAnchor = strstr(chunk.memory, "\"duration_ms\"");
                if (trackAnchor) {
                    localDuration = extractJsonLong(trackAnchor, "\"duration_ms\"");
                    extractJsonString(trackAnchor, "\"id\"", newSongId, sizeof(newSongId));
                    extractJsonString(trackAnchor, "\"name\"", localSong, sizeof(localSong));
                    
                    LightLock_Lock(&dataLock);
                    if (strlen(newSongId) > 0 && strcmp(newSongId, currentSongId) != 0) {
                        shouldFetchArt = true;
                        strcpy(currentSongId, newSongId);
                    }
                    LightLock_Unlock(&dataLock);

                    if (shouldFetchArt) {
                        char* albumBlock = strstr(chunk.memory, "\"album\"");
                        char* imagesBlock = albumBlock ? strstr(albumBlock, "\"images\"") : NULL;
                        
                        marqueeOffset = 0.0f;
                        marqueePausedAtEnd = false;
                        marqueeWaitTimer = osGetTime() + 1000;

                        if (imagesBlock) {
                            char* current = imagesBlock;
                            char* bestUrl = NULL;
                            int urlCount = 0;
                            char* endArray = strchr(imagesBlock, ']');
                            
                            while ((current = strstr(current, "\"url\"")) != NULL && (!endArray || current < endArray)) {
                                urlCount++;
                                if (urlCount == 2) { 
                                    bestUrl = current;
                                    break;
                                }
                                current += 5; 
                            }
                            
                            if (!bestUrl && urlCount == 1) bestUrl = strstr(imagesBlock, "\"url\"");
                            
                            if (bestUrl) {
                                extractJsonString(bestUrl, "\"url\"", targetUrl, sizeof(targetUrl));
                                unescapeUrl(targetUrl); 
                            }
                        }
                    }
                }

                // update all playback state
                LightLock_Lock(&dataLock);
                isPlaying = localIsPlaying;
                currentProgressMs = localProgress;
                currentDurationMs = localDuration;
                if (strlen(localArtist) > 0) strcpy(currentArtist, localArtist);
                if (strlen(localSong) > 0) strcpy(currentSong, localSong);
                lastSyncTime = osGetTime();
                LightLock_Unlock(&dataLock);
                
                // fetch cover art if song changed
                if (shouldFetchArt) {
                    if (strlen(targetUrl) > 0) {
                        LightLock_Lock(&dataLock);
                        snprintf(debugUrl, sizeof(debugUrl), "%.46s...", targetUrl);
                        LightLock_Unlock(&dataLock);
                        fetchCoverArt(targetUrl);
                    } else {
                        LightLock_Lock(&dataLock);
                        strcpy(debugUrl, "None found");
                        LightLock_Unlock(&dataLock);
                        updateStatus("No Art URL found");
                    }
                }
                
            } else if (response_code == 204) {
                LightLock_Lock(&dataLock);
                strcpy(currentSong, "Nothing playing");
                strcpy(currentArtist, "");
                strcpy(currentSongId, "");
                isPlaying = false;
                currentProgressMs = 0;
                currentDurationMs = 0;
                LightLock_Unlock(&dataLock);
            }
        } else {
             updateStatus("Net Err: %d", res);
        }
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        free(chunk.memory);
    }
}

void sendSpotifyCommand(const char* endpoint, const char* method) {
    if (strlen(currentAccessToken) == 0) {
        updateStatus("No Token! Restart app.");
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
                updateStatus("Success: %s", endpoint);
                lastFetchTime = osGetTime() - FETCH_INTERVAL + 500; 
            } else {
                updateStatus("API Error: %ld", response_code);
            }
        } else {
            updateStatus("Net Error: %s", curl_easy_strerror(res));
        }

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    }
}

void playContext(const char* contextUri) {
    if (strlen(currentAccessToken) == 0) return;

    updateStatus("Starting playlist...");
    
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
                updateStatus("Playlist Started!");
                LightLock_Lock(&dataLock);
                isPlaying = true;
                LightLock_Unlock(&dataLock);
                lastFetchTime = osGetTime() - FETCH_INTERVAL + 500; 
            } else {
                updateStatus("Play Error: %ld", response_code);
            }
        } else {
            updateStatus("Net Error: %s", curl_easy_strerror(res));
        }

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    }
}

// thread worker for network / async calls
void networkWorker(void* arg) {
    while (!exitThread) {
        NetCommand cmd = CMD_NONE;
        char localUri[128] = "";

        // check if main thread gave us a job
        LightLock_Lock(&dataLock);
        if (currentCommand != CMD_NONE) {
            cmd = currentCommand;
            strcpy(localUri, commandUri);
            currentCommand = CMD_NONE;
        }
        LightLock_Unlock(&dataLock);

        if (cmd != CMD_NONE) {
            // execute any slow network calls OUTSIDE the lock
            if (cmd == CMD_FETCH_PLAYING) fetchCurrentlyPlaying();
            else if (cmd == CMD_PLAY) sendSpotifyCommand("/play", "PUT");
            else if (cmd == CMD_PAUSE) sendSpotifyCommand("/pause", "PUT");
            else if (cmd == CMD_NEXT) sendSpotifyCommand("/next", "POST");
            else if (cmd == CMD_PREV) sendSpotifyCommand("/previous", "POST");
            else if (cmd == CMD_FETCH_PLAYLISTS) fetchPlaylists();
            else if (cmd == CMD_PLAY_CONTEXT) playContext(localUri);

            // tell  main thread we finished
            LightLock_Lock(&dataLock);
            isRequestPending = false;
            LightLock_Unlock(&dataLock);
        }

        // sleep for 10ms to prevent eating CPU
        svcSleepThread(10000000ULL); 
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

// draw centered dynamic text w/ citro2d
void DrawTextCentered(C2D_Font font, float y, float size, u32 color, const char* format, ...) {
    char buffer[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    
    C2D_Text textObj;
    
    if (font) {
        C2D_TextFontParse(&textObj, font, g_dynamicBuf, buffer);
    } else {
        C2D_TextParse(&textObj, g_dynamicBuf, buffer);
    }
    
    C2D_TextOptimize(&textObj);
    
    float textWidth, textHeight;
    C2D_TextGetDimensions(&textObj, size, size, &textWidth, &textHeight);
    
    float x = 200.0f - (textWidth / 2.0f);
    C2D_DrawText(&textObj, C2D_WithColor, x, y, 0.5f, size, size, color);
}

void DrawTextMarquee(C2D_Font font, float y, float size, u32 color, float maxW, const char* format, ...) {
    char buffer[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    C2D_Text textObj;
    if (font) C2D_TextFontParse(&textObj, font, g_dynamicBuf, buffer);
    else C2D_TextParse(&textObj, g_dynamicBuf, buffer);
    C2D_TextOptimize(&textObj);

    float textWidth, textHeight;
    C2D_TextGetDimensions(&textObj, size, size, &textWidth, &textHeight);

    float windowLeftEdge = (400.0f - maxW) / 2.0f;

    // only scroll if text is wider than window
    if (textWidth <= maxW) {
        float centerX = 200.0f - (textWidth / 2.0f);
        C2D_DrawText(&textObj, C2D_WithColor, centerX, y, 0.5f, size, size, color);
    } else {
        float maxTravel = textWidth - maxW;
        u64 currentTime = osGetTime();

        if (currentTime > marqueeWaitTimer) {
            if (marqueePausedAtEnd) {
                marqueeOffset = 0.0f;
                marqueePausedAtEnd = false;
                // add a 1-second pause at the start before moving again
                marqueeWaitTimer = currentTime + 1000; 
            } else {
                marqueeOffset += MARQUEE_SPEED;

                if (marqueeOffset >= maxTravel) {
                    marqueeOffset = maxTravel;
                    marqueePausedAtEnd = true;
                    // pause for 1 second at the end
                    marqueeWaitTimer = currentTime + 1000; 
                }
            }
        }

        float renderX = windowLeftEdge - marqueeOffset;

        C2D_Flush(); 
        int winX = (int)windowLeftEdge;
        int winW = (int)maxW;
        
        // scissor text so doesnt draw text outside window during marquyee scroll
        C3D_SetScissor(GPU_SCISSOR_NORMAL, 0, 400 - (winX + winW), 240, 400 - winX);
        C2D_DrawText(&textObj, C2D_WithColor, renderX, y, 0.5f, size, size, color);
        C2D_Flush();
        C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0); 
    }
}

// draw borders w/ citro2d
void DrawBorderC2D(float x, float y, float w, float h, float t, u32 color) {
    C2D_DrawRectSolid(x, y, 0.5f, w, t, color);          // top
    C2D_DrawRectSolid(x, y + h - t, 0.5f, w, t, color);  // bottom
    C2D_DrawRectSolid(x, y, 0.5f, t, h, color);          // left
    C2D_DrawRectSolid(x + w - t, y, 0.5f, t, h, color);  // right
}

void loadControlTexture(C3D_Tex* tex, C2D_Image* img, const char* path) {
    int width, height, channels;
    unsigned char* data = stbi_load(path, &width, &height, &channels, 4);
    if (!data) return;

    C3D_TexInit(tex, 128, 64, GPU_RGBA8); 
    C3D_TexSetFilter(tex, GPU_LINEAR, GPU_NEAREST); 
    
    u32* vram = (u32*)tex->data;
    memset(vram, 0, 128 * 64 * 4); 

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int srcIdx = (y * width + x) * 4;
            u8 r = data[srcIdx + 0];
            u8 g = data[srcIdx + 1];
            u8 b = data[srcIdx + 2];
            u8 a = data[srcIdx + 3];

            u32 color = (r << 24) | (g << 16) | (b << 8) | a;
            vram[morton_index(x, y, 128)] = color;
        }
    }

    img->tex = tex;
    img->subtex = &controlSubTex; 
    
    stbi_image_free(data);
}

void loadTabTexture(C3D_Tex* tex, C2D_Image* img, const char* path) {
    int width, height, channels;
    unsigned char* data = stbi_load(path, &width, &height, &channels, 4);
    if (!data) return;

    C3D_TexInit(tex, 128, 64, GPU_RGBA8); 
    C3D_TexSetFilter(tex, GPU_LINEAR, GPU_NEAREST);
    
    u32* vram = (u32*)tex->data;
    memset(vram, 0, 128 * 64 * 4); 

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int srcIdx = (y * width + x) * 4;
            u8 r = data[srcIdx + 0];
            u8 g = data[srcIdx + 1];
            u8 b = data[srcIdx + 2];
            u8 a = data[srcIdx + 3];

            u32 color = (r << 24) | (g << 16) | (b << 8) | a;
            vram[morton_index(x, y, 128)] = color;
        }
    }

    img->tex = tex;
    img->subtex = &tabSubTex; 
    
    stbi_image_free(data);
}

void loadBorderTexture(C3D_Tex* tex, C2D_Image* img, const char* path) {
    int width, height, channels;
    unsigned char* data = stbi_load(path, &width, &height, &channels, 4);
    if (!data) return;

    C3D_TexInit(tex, 256, 256, GPU_RGBA8); 
    C3D_TexSetFilter(tex, GPU_NEAREST, GPU_NEAREST); 
    
    u32* vram = (u32*)tex->data;
    memset(vram, 0, 256 * 256 * 4);

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int srcIdx = (y * width + x) * 4;
            u8 r = data[srcIdx + 0];
            u8 g = data[srcIdx + 1];
            u8 b = data[srcIdx + 2];
            u8 a = data[srcIdx + 3];

            u32 color = (r << 24) | (g << 16) | (b << 8) | a;
            vram[morton_index(x, y, 256)] = color;
        }
    }

    img->tex = tex;
    img->subtex = &borderSubTex; 
    
    stbi_image_free(data);
}

void loadDynamicTexture(C3D_Tex* tex, C2D_Image* img, Tex3DS_SubTexture* subtex, const char* path, int vramW, int vramH) {
    int width, height, channels;
    unsigned char* data = stbi_load(path, &width, &height, &channels, 4);
    if (!data) return;

    C3D_TexInit(tex, vramW, vramH, GPU_RGBA8); 
    C3D_TexSetFilter(tex, GPU_LINEAR, GPU_NEAREST);
    
    u32* vram = (u32*)tex->data;
    memset(vram, 0, vramW * vramH * 4); 

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int srcIdx = (y * width + x) * 4;
            // Pack RGBA
            u32 color = (data[srcIdx + 0] << 24) | (data[srcIdx + 1] << 16) | (data[srcIdx + 2] << 8) | data[srcIdx + 3];
            vram[morton_index(x, y, vramW)] = color;
        }
    }

    // automatically map the subtexture to the actual image dimensions
    subtex->width = width;
    subtex->height = height;
    subtex->left = 0.0f;
    subtex->top = 1.0f;
    subtex->right = (float)width / (float)vramW;
    subtex->bottom = 1.0f - ((float)height / (float)vramH);

    img->tex = tex;
    img->subtex = subtex;
    
    stbi_image_free(data);
}

// replace non-standard characters with ?
void sanitizeText(char* str) {
    int i = 0, j = 0;
    while (str[i] != '\0') {
        unsigned char c = str[i];
        
        if (c >= 32 && c <= 126) {
            str[j++] = str[i++];
        } 
        else if (c >= 192) {
            str[j++] = '?';
            
            if ((c & 0xE0) == 0xC0) i += 2;      // 2-byte
            else if ((c & 0xF0) == 0xE0) i += 3; // 3-byte
            else if ((c & 0xF8) == 0xF0) i += 4; // 4-byte
            else i++;
        } 
        else {
            str[j++] = '?';
            i++;
        }
    }
    str[j] = '\0';
}

int main(int argc, char **argv) {
    // init standard services
    gfxInitDefault();
    u32 *socBuffer = (u32*)memalign(SOC_ALIGN, SOC_BUFFERSIZE);
    if(R_FAILED(socInit(socBuffer, SOC_BUFFERSIZE))) {
        printf("Failed to initialize SOC service!\n");
    }
    curl_global_init(CURL_GLOBAL_DEFAULT);

    LightLock_Init(&dataLock);
    
    // create background thread
    networkThread = threadCreate(networkWorker, NULL, 32 * 1024, 0x30, -2, false);

    // init GPU and 2D graphics engine
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();

    // create rendering targets for both screens
    C3D_RenderTarget* topTarget = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    C3D_RenderTarget* bottomTarget = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);

    // create text buffers
    g_dynamicBuf = C2D_TextBufNew(4096);
    g_staticBuf = C2D_TextBufNew(1024);

    // Parse static text to save CPU cycles
    C2D_TextParse(&txtTitleSettings, g_staticBuf, "Settings & Debug");
    C2D_TextOptimize(&txtTitleSettings);
    C2D_TextParse(&txtLoading, g_staticBuf, "Loading...");
    C2D_TextOptimize(&txtLoading);
    C2D_TextParse(&txtNoPlaylists, g_staticBuf, "No playlists found. Tap tab to retry.");
    C2D_TextOptimize(&txtNoPlaylists);

    // HEX to citro2d colors
    u32 clrGreen    = C2D_Color32(49, 49, 49, 255);
    u32 clrPlaylistText = C2D_Color32(86, 81, 72, 255);
    // u32 clrBtnBg    = C2D_Color32(225, 221, 209, 0);
    u32 clrDisabled = C2D_Color32(85, 85, 85, 255);
    u32 clrBlack    = C2D_Color32(0, 0, 0, 255);

    u32 clrCheck1    = C2D_Color32(161, 225, 245, 255);
    u32 clrCheck2    = C2D_Color32(186, 234, 249, 255);
    u32 clrGradTop   = C2D_Color32(220, 248, 222, 255);
    u32 clrGradBot   = C2D_Color32(180, 237, 195, 255);

    refreshSpotifyToken();
    
    // Dispatch initial fetch to the background thread
    LightLock_Lock(&dataLock);
    currentCommand = CMD_FETCH_PLAYING;
    isRequestPending = true;
    LightLock_Unlock(&dataLock);
    
    lastFetchTime = osGetTime();

    bool touchLock = false;

    romfsInit(); 

    fontFranxurter = C2D_FontLoad("romfs:/franxurter.bcfnt");
    
    // load tab buttons
    loadTabTexture(&tabTexPlayback, &imgTabPlayback, "romfs:/tab_playback.png");
    loadTabTexture(&tabTexLibrary, &imgTabLibrary, "romfs:/tab_library.png");
    loadTabTexture(&tabTexSettings, &imgTabSettings, "romfs:/tab_settings.png");
    tabImagesLoaded = true;

    // load control buttons
    loadControlTexture(&texControlPlay, &imgControlPlay, "romfs:/control_play.png");
    loadControlTexture(&texControlPause, &imgControlPause, "romfs:/control_pause.png");
    loadControlTexture(&texControlNext, &imgControlNext, "romfs:/control_next.png");
    loadControlTexture(&texControlPrev, &imgControlPrev, "romfs:/control_prev.png");

    loadControlTexture(&texControlPlay2, &imgControlPlay2, "romfs:/control_play2.png");
    loadControlTexture(&texControlPause2, &imgControlPause2, "romfs:/control_pause2.png");
    loadControlTexture(&texControlNext2, &imgControlNext2, "romfs:/control_next2.png");
    loadControlTexture(&texControlPrev2, &imgControlPrev2, "romfs:/control_prev2.png");
    controlImagesLoaded = true;
    
    // load border frame
    loadBorderTexture(&texBorder, &imgBorder, "romfs:/art_border.png");
    borderLoaded = true;

    // load library sprites
    loadDynamicTexture(&texPlaylistBar, &imgPlaylistBar, &subTexPlaylistBar, "romfs:/playlistBar.png", 1024, 128);
    libraryUiLoaded = true;

    while (aptMainLoop()) {
        hidScanInput();
        u32 kDown = hidKeysDown();
        u32 kHeld = hidKeysHeld();
        u32 kUp   = hidKeysUp();

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
            bool localFetched;
            int localNumPlaylists;
            LightLock_Lock(&dataLock);
            localFetched = libraryFetched;
            localNumPlaylists = numPlaylists;
            LightLock_Unlock(&dataLock);
            
            if (currentTab == TAB_LIBRARY && (!localFetched || localNumPlaylists == 0)) {
                LightLock_Lock(&dataLock);
                if (!isRequestPending) {
                    currentCommand = CMD_FETCH_PLAYLISTS;
                    isRequestPending = true;
                }
                LightLock_Unlock(&dataLock);
            }
        }

        // dpad / circle pad
        int currentNumPlaylists;
        LightLock_Lock(&dataLock);
        currentNumPlaylists = numPlaylists;
        LightLock_Unlock(&dataLock);

        float maxScrollPixels = (currentNumPlaylists * ROW_HEIGHT) - VISIBLE_HEIGHT;
        if (maxScrollPixels < 0) maxScrollPixels = 0.0f;

        if (currentTab == TAB_LIBRARY && currentNumPlaylists > 0) {
            if ((kHeld & KEY_DDOWN) || (kHeld & KEY_CPAD_DOWN)) {
                listScrollPixels += 4.0f; // scroll speed
                if (listScrollPixels > maxScrollPixels) listScrollPixels = maxScrollPixels;
            }
            if ((kHeld & KEY_DUP) || (kHeld & KEY_CPAD_UP)) {
                listScrollPixels -= 4.0f;
                if (listScrollPixels < 0.0f) listScrollPixels = 0.0f;
            }
            if ((kDown & KEY_DRIGHT) || (kDown & KEY_CPAD_RIGHT)) {
                listScrollPixels += VISIBLE_HEIGHT;
                if (listScrollPixels > maxScrollPixels) listScrollPixels = maxScrollPixels;
            }
            if ((kDown & KEY_DLEFT) || (kDown & KEY_CPAD_LEFT)) {
                listScrollPixels -= VISIBLE_HEIGHT;
                if (listScrollPixels < 0.0f) listScrollPixels = 0.0f;
            }
        }

        u64 currentTime = osGetTime();
        if (currentTime - lastFetchTime >= FETCH_INTERVAL) {
            LightLock_Lock(&dataLock);
            if (!isRequestPending) {
                currentCommand = CMD_FETCH_PLAYING;
                isRequestPending = true;
            }
            LightLock_Unlock(&dataLock);
            lastFetchTime = currentTime;
        }

        // touch screen drag
        if (kDown & KEY_TOUCH) {
            touchPosition touch;
            hidTouchRead(&touch);
            startTouchY = touch.py;
            lastTouchY = touch.py;
            dragDidMove = false;
            
            if (currentTab == TAB_LIBRARY && touch.py <= LIST_END_Y) {
                isListDragging = true;
            }
        }

        if (kHeld & KEY_TOUCH) {
            touchPosition touch;
            hidTouchRead(&touch);

            if (currentTab == TAB_LIBRARY && isListDragging) {
                int deltaY = touch.py - lastTouchY;
                
                if (abs(touch.py - startTouchY) > 5) dragDidMove = true;
                
                if (dragDidMove) {
                    listScrollPixels -= deltaY;
                    if (listScrollPixels < 0.0f) listScrollPixels = 0.0f;
                    if (listScrollPixels > maxScrollPixels) listScrollPixels = maxScrollPixels;
                }
                lastTouchY = touch.py;
            }

            if (!touchLock) {
                // tab switching
                if (isTouchInside(touch, btnTabPlayback)) {
                    currentTab = TAB_PLAYBACK;
                    touchLock = true;
                }
                else if (isTouchInside(touch, btnTabLibrary)) {
                    currentTab = TAB_LIBRARY;
                    bool localFetched;
                    int localNum;
                    LightLock_Lock(&dataLock);
                    localFetched = libraryFetched;
                    localNum = numPlaylists;
                    LightLock_Unlock(&dataLock);
                    
                    if (!localFetched || localNum == 0) {
                        LightLock_Lock(&dataLock);
                        if (!isRequestPending) {
                            currentCommand = CMD_FETCH_PLAYLISTS;
                            isRequestPending = true;
                        }
                        LightLock_Unlock(&dataLock);
                    }
                    touchLock = true;
                }
                else if (isTouchInside(touch, btnTabSettings)) {
                    currentTab = TAB_SETTINGS;
                    touchLock = true;
                }

                // playback controls tab
                if (currentTab == TAB_PLAYBACK && touchLock == false) {
                    bool localPlaying;
                    LightLock_Lock(&dataLock);
                    localPlaying = isPlaying;
                    LightLock_Unlock(&dataLock);

                    if (isTouchInside(touch, btnPrev)) {
                        updateStatus("Sending: Previous...");
                        LightLock_Lock(&dataLock);
                        if (!isRequestPending) {
                            currentProgressMs = 0; 
                            lastSyncTime = osGetTime();
                            prevClickedTime = osGetTime();
                            
                            currentCommand = CMD_PREV;
                            isRequestPending = true;
                        }
                        LightLock_Unlock(&dataLock);
                        touchLock = true;
                    } 
                    else if (isTouchInside(touch, btnPause) && localPlaying) {
                        updateStatus("Sending: Pause...");
                        LightLock_Lock(&dataLock);
                        if (!isRequestPending) {
                            isPlaying = false;
                            currentProgressMs += (osGetTime() - lastSyncTime); 
                            lastSyncTime = osGetTime();
                            pauseClickedTime = osGetTime();
                            
                            currentCommand = CMD_PAUSE;
                            isRequestPending = true;
                        }
                        LightLock_Unlock(&dataLock);
                        touchLock = true;
                    } 
                    else if (isTouchInside(touch, btnPlay) && !localPlaying) {
                        updateStatus("Sending: Play...");
                        LightLock_Lock(&dataLock);
                        if (!isRequestPending) {
                            isPlaying = true;
                            lastSyncTime = osGetTime(); 
                            playClickedTime = osGetTime();
                            
                            currentCommand = CMD_PLAY;
                            isRequestPending = true;
                        }
                        LightLock_Unlock(&dataLock);
                        touchLock = true;
                    }
                    else if (isTouchInside(touch, btnNext)) {
                        updateStatus("Sending: Next...");
                        LightLock_Lock(&dataLock);
                        if (!isRequestPending) {
                            currentProgressMs = 0; 
                            lastSyncTime = osGetTime();
                            nextClickedTime = osGetTime();
                            
                            currentCommand = CMD_NEXT;
                            isRequestPending = true;
                        }
                        LightLock_Unlock(&dataLock);
                        touchLock = true;
                    }
                }
            }
        } else {
            touchLock = false;
        }

        if (kUp & KEY_TOUCH) {
            if (currentTab == TAB_LIBRARY && isListDragging && !dragDidMove) {
                float absoluteY = (startTouchY - LIST_START_Y) + listScrollPixels;
                int clickedIndex = absoluteY / ROW_HEIGHT;
                
                if (clickedIndex >= 0 && clickedIndex < currentNumPlaylists) {
                    LightLock_Lock(&dataLock);
                    if (!isRequestPending) {
                        strncpy(commandUri, userPlaylists[clickedIndex].uri, 127);
                        commandUri[127] = '\0';
                        currentCommand = CMD_PLAY_CONTEXT;
                        isRequestPending = true;
                    }
                    LightLock_Unlock(&dataLock);
                    
                    touchLock = true;
                    currentTab = TAB_PLAYBACK;
                }
            }
            isListDragging = false;
            dragDidMove = false;
        }

        LightLock_Lock(&dataLock);
        if (newArtReady && tempArtBuffer) {
            if (!texLoaded) {
                C3D_TexInit(&albumTex, 256, 256, GPU_RGBA8);
                C3D_TexSetFilter(&albumTex, GPU_LINEAR, GPU_NEAREST);
                texLoaded = true;
            }
            
            // copy from linear RAM to VRAM
            memcpy(albumTex.data, tempArtBuffer, 256 * 256 * 4);
            
            linearFree(tempArtBuffer);
            tempArtBuffer = NULL;
            newArtReady = false;

            albumSubTex.width = TARGET_IMG_SIZE;
            albumSubTex.height = TARGET_IMG_SIZE;
            albumSubTex.left = 0.0f;
            albumSubTex.top = 1.0f; 
            albumSubTex.right = TARGET_IMG_SIZE / 256.0f;
            albumSubTex.bottom = 1.0f - (TARGET_IMG_SIZE / 256.0f);

            albumImg.tex = &albumTex;
            albumImg.subtex = &albumSubTex;
        }
        LightLock_Unlock(&dataLock);

        // fetch display state locally to prevent UI tearing during drawing
        bool dispPlaying;
        long dispDurMs, dispProgMs;
        char dispSong[128], dispArtist[128];
        int dispNumPlaylists;
        bool dispLibraryFetched;
        char dispStatus[256];
        
        LightLock_Lock(&dataLock);
        dispPlaying = isPlaying;
        dispDurMs = currentDurationMs;
        dispProgMs = currentProgressMs;
        strcpy(dispSong, currentSong);
        strcpy(dispArtist, currentArtist);
        dispNumPlaylists = numPlaylists;
        dispLibraryFetched = libraryFetched;
        strcpy(dispStatus, lastStatus);
        
        if (dispPlaying) dispProgMs += (osGetTime() - lastSyncTime);
        if (dispProgMs > dispDurMs) dispProgMs = dispDurMs;
        LightLock_Unlock(&dataLock);

        // rendering
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        C2D_TextBufClear(g_dynamicBuf); 

        // ============= TOP SCREEN =================
        C2D_TargetClear(topTarget, clrCheck1);
        C2D_SceneBegin(topTarget);
        
        // bg checkerboard
        int tileSize = 20;
        for (int y = 0; y < 240; y += tileSize) {
            for (int x = 0; x < 400; x += tileSize) {
                int row = y / tileSize;
                int col = x / tileSize;
                
                if ((row + col) % 2 != 0) {
                    C2D_DrawRectSolid(x, y, 0.5f, tileSize, tileSize, clrCheck2);
                }
            }
        }

        char progressDisplay[32] = "0:00 / 0:00";
        if (dispDurMs > 0) {
            int p_sec = (dispProgMs / 1000) % 60;
            int p_min = (dispProgMs / 1000) / 60;
            int d_sec = (dispDurMs / 1000) % 60;
            int d_min = (dispDurMs / 1000) / 60;
            sprintf(progressDisplay, "%d:%02d / %d:%02d", p_min, p_sec, d_min, d_sec);
        }
        
        DrawText(8, 222,  0.5f, clrBlack, "%s", progressDisplay);
        
        // centered album art
        if (texLoaded) {
            C2D_DrawImageAt(albumImg, 125.0f, 24.0f, 0.5f, NULL, 0.75f, 0.75f);
        }
        if (borderLoaded) {
            C2D_DrawImageAt(imgBorder, 114.0f, 16.0f, 0.5f, NULL, 1.0f, 1.0f);
        }

        DrawTextMarquee(fontFranxurter, 182, 1.0f, clrBlack, 300.0f, "%s", dispSong);
        DrawTextCentered(NULL, 216, 0.6f, clrBlack, "%.22s", dispArtist);

        // ============= BOTTOM SCREEN =================
        C2D_TargetClear(bottomTarget, clrGradBot);
        C2D_SceneBegin(bottomTarget);

        // bg gradient
        C2D_DrawRectangle(0, 0, 0.5f, 320, 240, clrGradTop, clrGradTop, clrGradBot, clrGradBot);
        
        if (currentTab == TAB_PLAYBACK) {

            // PLAY BTN
            if (osGetTime() - playClickedTime < 300) {
                C2D_DrawImageAt(imgControlPlay2, btnPlay.x, btnPlay.y, 0.5f, NULL, 1.0f, 1.0f);
            } else {
                C2D_DrawImageAt(imgControlPlay, btnPlay.x, btnPlay.y, 0.5f, NULL, 1.0f, 1.0f);
            }

            // PAUSE BTN
            if (osGetTime() - pauseClickedTime < 300) {
                C2D_DrawImageAt(imgControlPause2, btnPause.x, btnPause.y, 0.5f, NULL, 1.0f, 1.0f);
            } else {
                C2D_DrawImageAt(imgControlPause, btnPause.x, btnPause.y, 0.5f, NULL, 1.0f, 1.0f);
            }

            // PREV BTN
            if (osGetTime() - prevClickedTime < 300) {
                C2D_DrawImageAt(imgControlPrev2, btnPrev.x, btnPrev.y, 0.5f, NULL, 1.0f, 1.0f);
            } else {
                C2D_DrawImageAt(imgControlPrev, btnPrev.x, btnPrev.y, 0.5f, NULL, 1.0f, 1.0f);
            }
            
            // NEXT BTN
            if (osGetTime() - nextClickedTime < 300) {
                C2D_DrawImageAt(imgControlNext2, btnNext.x, btnNext.y, 0.5f, NULL, 1.0f, 1.0f);
            } else {
                C2D_DrawImageAt(imgControlNext, btnNext.x, btnNext.y, 0.5f, NULL, 1.0f, 1.0f);
            }
            
        } else if (currentTab == TAB_LIBRARY) {            
            if (!dispLibraryFetched) {
                C2D_DrawText(&txtLoading, C2D_WithColor, 10, 10, 0.5f, 0.5f, 0.5f, clrDisabled);
            } else if (dispNumPlaylists == 0) {
                C2D_DrawText(&txtNoPlaylists, C2D_WithColor, 10, 10, 0.5f, 0.5f, 0.5f, clrDisabled);
            } else {
                LightLock_Lock(&dataLock);

                for (int i = 0; i < dispNumPlaylists; i++) {
                    float yPos = LIST_START_Y + (i * ROW_HEIGHT) - listScrollPixels;
                    
                    if (yPos + ROW_HEIGHT > 0 && yPos < LIST_END_Y) {
                        
                        if (libraryUiLoaded) {
                            C2D_DrawImageAt(imgPlaylistBar, 8, yPos, 0.5f, NULL, 0.42f, 0.42f);
                        }

                        C2D_Text playlistText;
                        char buf[64];
                        if (strlen(userPlaylists[i].name) > 32) {
                            snprintf(buf, sizeof(buf), "%.32s...", userPlaylists[i].name);
                        } else {
                            snprintf(buf, sizeof(buf), "%s", userPlaylists[i].name);
                        }
                        
                        sanitizeText(buf);

                        C2D_TextParse(&playlistText, g_dynamicBuf, buf);
                        C2D_TextOptimize(&playlistText);
                        
                        C2D_DrawText(&playlistText, C2D_WithColor, 44, yPos + 10, 0.5f, 0.6f, 0.6f, clrPlaylistText);
                    }
                }
                LightLock_Unlock(&dataLock);

                // calculate and draw scrollbar
                float maxScrollPixelsRender = (dispNumPlaylists * ROW_HEIGHT) - VISIBLE_HEIGHT;
                if (maxScrollPixelsRender > 0) {
                    float scrollRatio = listScrollPixels / maxScrollPixelsRender;
                    
                    float paddingTop = 4.0f;
                    
                    float adjustedVisibleHeight = VISIBLE_HEIGHT - paddingTop;

                    float thumbHeight = (VISIBLE_HEIGHT / (dispNumPlaylists * ROW_HEIGHT)) * VISIBLE_HEIGHT;
                    if (thumbHeight < 20.0f) thumbHeight = 20.0f; 
                    
                    float thumbY = LIST_START_Y + paddingTop + (scrollRatio * (adjustedVisibleHeight - thumbHeight));
                    
                    C2D_DrawRectSolid(313, LIST_START_Y + paddingTop, 0.5f, 4, adjustedVisibleHeight, clrDisabled);
                    
                    C2D_DrawRectSolid(312, thumbY, 0.5f, 6, thumbHeight, clrGreen);
                }
            }
        } else if (currentTab == TAB_SETTINGS) {
            C2D_DrawText(&txtTitleSettings, C2D_WithColor, 10, 10, 0.5f, 0.6f, 0.6f, clrGreen);

            char displayToken[25] = "";
            if(strlen(currentAccessToken) > 0) {
                snprintf(displayToken, sizeof(displayToken), "%.20s...", currentAccessToken);
            } else {
                strcpy(displayToken, "None");
            }
            
            DrawText(10, 40, 0.45f, clrGreen, "Play State: %s", dispPlaying ? "Playing" : "Paused");
            DrawText(10, 60, 0.45f, clrGreen, "API Status: %.35s", dispStatus);
            DrawText(10, 80, 0.45f, clrGreen, "Token: %.18s", displayToken);
            
            LightLock_Lock(&dataLock);
            DrawText(10, 100, 0.45f, clrGreen, "Art URL: %.35s", debugUrl);
            LightLock_Unlock(&dataLock);
        }

        // render tabs
        float activeOffset = 4.0f;
        // Playback
        C2D_DrawImageAt(imgTabPlayback, btnTabPlayback.x, 
                        currentTab == TAB_PLAYBACK ? btnTabPlayback.y - activeOffset : btnTabPlayback.y, 
                        0.5f, NULL, 1.0f, 1.0f);

        // Library
        C2D_DrawImageAt(imgTabLibrary, btnTabLibrary.x, 
                        currentTab == TAB_LIBRARY ? btnTabLibrary.y - activeOffset : btnTabLibrary.y, 
                        0.5f, NULL, 1.0f, 1.0f);

        // Settings
        C2D_DrawImageAt(imgTabSettings, btnTabSettings.x, 
                        currentTab == TAB_SETTINGS ? btnTabSettings.y - activeOffset : btnTabSettings.y, 
                        0.5f, NULL, 1.0f, 1.0f);
        
        C3D_FrameEnd(0);
    }

    // Shut down background thread
    exitThread = true;
    threadJoin(networkThread, U64_MAX);
    threadFree(networkThread);

    if (tempArtBuffer) {
        linearFree(tempArtBuffer);
    }

    if (texLoaded) {
        C3D_TexDelete(&albumTex); 
    }
    if (fontFranxurter) {
        C2D_FontFree(fontFranxurter);
    }
    
    C2D_TextBufDelete(g_dynamicBuf);
    C2D_TextBufDelete(g_staticBuf);
    C2D_Fini();
    C3D_Fini();
    
    if (tabImagesLoaded) {
        C3D_TexDelete(&tabTexPlayback);
        C3D_TexDelete(&tabTexLibrary);
        C3D_TexDelete(&tabTexSettings);
    }

    if (controlImagesLoaded) {
        C3D_TexDelete(&texControlPlay);
        C3D_TexDelete(&texControlPause);
        C3D_TexDelete(&texControlNext);
        C3D_TexDelete(&texControlPrev);

        C3D_TexDelete(&texControlPlay2);
        C3D_TexDelete(&texControlPause2);
        C3D_TexDelete(&texControlNext2);
        C3D_TexDelete(&texControlPrev2);
    }
    if (borderLoaded) {
        C3D_TexDelete(&texBorder);
    }
    if (libraryUiLoaded) {
        C3D_TexDelete(&texPlaylistBar);
    }
    
    romfsExit();
    
    curl_global_cleanup();
    socExit();
    free(socBuffer);
    gfxExit();
    
    return 0;
}
