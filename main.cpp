#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <malloc.h>
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
u64 lastSyncTime = 0; // smoothly fake-animate between 5-second refresh polls

// image data
u8* coverPixels = NULL;
const int TARGET_IMG_SIZE = 200; 

// polling timer variables
u64 lastFetchTime = 0;
const u64 FETCH_INTERVAL = 5000;

#define SOC_ALIGN       0x1000
#define SOC_BUFFERSIZE  0x100000

// ui
struct Button {
    u16 x, y, width, height;
    const char* label;
};

Button btnPrev  = { 10,  120, 65, 80, "Prev" };
Button btnPause = { 85,  120, 65, 80, "Pause" };
Button btnPlay  = { 160, 120, 65, 80, "Play" };
Button btnNext  = { 235, 120, 65, 80, "Next" };

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

// extracts integers for track duration and progress
long extractJsonLong(const char* json, const char* key) {
    char* keyPtr = strstr(json, key);
    if (!keyPtr) return 0;
    char* start = keyPtr + strlen(key);
    while(*start == ' ' || *start == ':') start++;
    return atol(start); // atol safely stops reading when it hits a comma or bracket
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
        
        if (coverPixels) {
            free(coverPixels);
            coverPixels = NULL;
        }

        int origW, origH, channels;
        u8* rawPixels = stbi_load_from_memory((const stbi_uc*)chunk.memory, chunk.size, &origW, &origH, &channels, 3);
        
        if (rawPixels) {
            coverPixels = (u8*)malloc(TARGET_IMG_SIZE * TARGET_IMG_SIZE * 3);
            
            for (int y = 0; y < TARGET_IMG_SIZE; y++) {
                for (int x = 0; x < TARGET_IMG_SIZE; x++) {
                    int srcX = (x * origW) / TARGET_IMG_SIZE;
                    int srcY = (y * origH) / TARGET_IMG_SIZE;
                    
                    int srcIdx = (srcY * origW + srcX) * 3;
                    int dstIdx = (y * TARGET_IMG_SIZE + x) * 3;
                    
                    coverPixels[dstIdx + 0] = rawPixels[srcIdx + 0];
                    coverPixels[dstIdx + 1] = rawPixels[srcIdx + 1];
                    coverPixels[dstIdx + 2] = rawPixels[srcIdx + 2];
                }
            }
            
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
                
                // grab time and sync local ticker info
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
                            strncpy(debugUrl, newCoverUrl, 50); 
                            strcat(debugUrl, "...");
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

void drawRect(gfxScreen_t screen, int screenW, int screenH, int startX, int startY, int width, int height, u8 r, u8 g, u8 b) {
    u16 fbWidth, fbHeight;
    u8* fb = gfxGetFramebuffer(screen, GFX_LEFT, &fbWidth, &fbHeight);
    if (!fb) return;
    
    GSPGPU_FramebufferFormat format = gfxGetScreenFormat(screen);
    int bytesPerPixel = (format == GSP_RGB565_OES) ? 2 : 3;
    
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int screenX = startX + x;
            int screenY = startY + y;
            
            if (screenX < 0 || screenX >= screenW || screenY < 0 || screenY >= screenH) continue;
            
            u32 fbPixelIndex = (screenX * screenH + ((screenH - 1) - screenY));
            
            if (bytesPerPixel == 3) {
                u32 fbIndex = fbPixelIndex * 3;
                fb[fbIndex + 0] = b; 
                fb[fbIndex + 1] = g; 
                fb[fbIndex + 2] = r; 
            } else if (bytesPerPixel == 2) {
                u32 fbIndex = fbPixelIndex * 2;
                u16 color565 = ((b >> 3) & 0x1F) | (((g >> 2) & 0x3F) << 5) | (((r >> 3) & 0x1F) << 11);
                fb[fbIndex + 0] = color565 & 0xFF;         
                fb[fbIndex + 1] = (color565 >> 8) & 0xFF;  
            }
        }
    }
}

void drawBorder(gfxScreen_t screen, int screenW, int screenH, int startX, int startY, int width, int height, int thickness, u8 r, u8 g, u8 b) {
    drawRect(screen, screenW, screenH, startX, startY, width, thickness, r, g, b); 
    drawRect(screen, screenW, screenH, startX, startY + height - thickness, width, thickness, r, g, b); 
    drawRect(screen, screenW, screenH, startX, startY, thickness, height, r, g, b); 
    drawRect(screen, screenW, screenH, startX + width - thickness, startY, thickness, height, r, g, b); 
}

void drawPreScaledImage(int startX, int startY, u8* imgData) {
    if (!imgData) return;
    
    u16 fbWidth, fbHeight;
    u8* fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &fbWidth, &fbHeight);
    if (!fb) return;
    
    GSPGPU_FramebufferFormat format = gfxGetScreenFormat(GFX_TOP);
    int bytesPerPixel = (format == GSP_RGB565_OES) ? 2 : 3;
    
    for (int y = 0; y < TARGET_IMG_SIZE; y++) {
        for (int x = 0; x < TARGET_IMG_SIZE; x++) {
            int screenX = startX + x;
            int screenY = startY + y;
            
            if (screenX < 0 || screenX >= 400 || screenY < 0 || screenY >= 240) continue;
            
            u32 imgIndex = (y * TARGET_IMG_SIZE + x) * 3;
            u8 r = imgData[imgIndex + 0];
            u8 g = imgData[imgIndex + 1];
            u8 b = imgData[imgIndex + 2];
            
            u32 fbPixelIndex = (screenX * 240 + (239 - screenY));
            
            if (bytesPerPixel == 3) {
                u32 fbIndex = fbPixelIndex * 3;
                fb[fbIndex + 0] = b; 
                fb[fbIndex + 1] = g; 
                fb[fbIndex + 2] = r; 
            } else if (bytesPerPixel == 2) {
                u32 fbIndex = fbPixelIndex * 2;
                u16 color565 = ((b >> 3) & 0x1F) | (((g >> 2) & 0x3F) << 5) | (((r >> 3) & 0x1F) << 11);
                fb[fbIndex + 0] = color565 & 0xFF;         
                fb[fbIndex + 1] = (color565 >> 8) & 0xFF;  
            }
        }
    }
}

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
    printf("\x1b[48;2;18;18;18m\x1b[38;2;29;185;84m\x1b[2J"); 
    
    consoleSelect(&bottomScreen);
    printf("\x1b[48;2;18;18;18m\x1b[38;2;29;185;84m\x1b[2J");

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
                    currentProgressMs = 0; // optimistic reset to 0
                    lastSyncTime = osGetTime();
                    sendSpotifyCommand("/previous", "POST");
                    touchLock = true;
                } 
                else if (isTouchInside(touch, btnPause) && isPlaying) {
                    sprintf(lastStatus, "Sending: Pause...");
                    isPlaying = false;
                    currentProgressMs += (osGetTime() - lastSyncTime); // pause time exactly where it is
                    lastSyncTime = osGetTime();
                    sendSpotifyCommand("/pause", "PUT");
                    touchLock = true;
                } 
                else if (isTouchInside(touch, btnPlay) && !isPlaying) {
                    sprintf(lastStatus, "Sending: Play...");
                    isPlaying = true;
                    lastSyncTime = osGetTime(); // restart local timer
                    sendSpotifyCommand("/play", "PUT");
                    touchLock = true;
                }
                else if (isTouchInside(touch, btnNext)) {
                    sprintf(lastStatus, "Sending: Next...");
                    currentProgressMs = 0; // optimistic reset to 0
                    lastSyncTime = osGetTime();
                    sendSpotifyCommand("/next", "POST");
                    touchLock = true;
                }
            }
        } else {
            touchLock = false;
        }

        // ============= TOP SCREEN =================
        consoleSelect(&topScreen);
        printf("\x1b[H\x1b[48;2;18;18;18m\x1b[38;2;29;185;84m"); 
        
        printf("\x1b[2;15H=== 3DSpotify ===                         \n");
        
        char displayToken[15] = "";
        if(strlen(currentAccessToken) > 0) {
            strncpy(displayToken, currentAccessToken, 10);
            strcat(displayToken, "...");
        } else {
            strcpy(displayToken, "None");
        }

        // calculate and display the current / total time
        char progressDisplay[32] = "[0:00 / 0:00]";
        if (currentDurationMs > 0) {
            long displayMs = currentProgressMs;
            
            // interpolate time locally if playing
            if (isPlaying) {
                displayMs += (osGetTime() - lastSyncTime);
            }
            if (displayMs > currentDurationMs) displayMs = currentDurationMs; // overrun cap

            int p_sec = (displayMs / 1000) % 60;
            int p_min = (displayMs / 1000) / 60;
            int d_sec = (currentDurationMs / 1000) % 60;
            int d_min = (currentDurationMs / 1000) / 60;

            sprintf(progressDisplay, "[%d:%02d / %d:%02d]", p_min, p_sec, d_min, d_sec);
        }
        
        printf("\x1b[5;2HToken:  %-35s\n", displayToken);
        printf("\x1b[8;2HStatus: %-35s\n", isPlaying ? "[> Playing]" : "[|| Paused]");
        printf("\x1b[10;2HTime:   %-35s\n", progressDisplay);
        printf("\x1b[12;2HSong:   %-35s\n", currentSong);
        printf("\x1b[14;2HArtist: %-35s\n", currentArtist);
        
        printf("\x1b[28;2HPress START to exit.");

        if (coverPixels) {
            drawPreScaledImage(190, 20, coverPixels);
        }

        // ============= BOTTOM SCREEN =================
        consoleSelect(&bottomScreen);
        printf("\x1b[H\x1b[48;2;18;18;18m\x1b[38;2;29;185;84m"); 
        printf("\x1b[2;10H--- Controls ---\n");
        printf("\x1b[5;2HStatus: %-30s\n", lastStatus);
        printf("\x1b[7;2HArt URL: %-30s\n", debugUrl);

        u8 actBgR = 33, actBgG = 33, actBgB = 33;
        u8 actFgR = 29, actFgG = 185, actFgB = 84;
        
        u8 disBgR = 33, disBgG = 33, disBgB = 33;
        u8 disFgR = 85, disFgG = 85, disFgB = 85;

        // PREV
        drawRect(GFX_BOTTOM, 320, 240, btnPrev.x, btnPrev.y, btnPrev.width, btnPrev.height, actBgR, actBgG, actBgB);
        drawBorder(GFX_BOTTOM, 320, 240, btnPrev.x, btnPrev.y, btnPrev.width, btnPrev.height, 2, actFgR, actFgG, actFgB);

        // PAUSE
        if (isPlaying) {
            drawRect(GFX_BOTTOM, 320, 240, btnPause.x, btnPause.y, btnPause.width, btnPause.height, actBgR, actBgG, actBgB);
            drawBorder(GFX_BOTTOM, 320, 240, btnPause.x, btnPause.y, btnPause.width, btnPause.height, 2, actFgR, actFgG, actFgB);
        } else {
            drawRect(GFX_BOTTOM, 320, 240, btnPause.x, btnPause.y, btnPause.width, btnPause.height, disBgR, disBgG, disBgB);
            drawBorder(GFX_BOTTOM, 320, 240, btnPause.x, btnPause.y, btnPause.width, btnPause.height, 2, disFgR, disFgG, disFgB);
        }

        // PLAY
        if (!isPlaying) {
            drawRect(GFX_BOTTOM, 320, 240, btnPlay.x, btnPlay.y, btnPlay.width, btnPlay.height, actBgR, actBgG, actBgB);
            drawBorder(GFX_BOTTOM, 320, 240, btnPlay.x, btnPlay.y, btnPlay.width, btnPlay.height, 2, actFgR, actFgG, actFgB);
        } else {
            drawRect(GFX_BOTTOM, 320, 240, btnPlay.x, btnPlay.y, btnPlay.width, btnPlay.height, disBgR, disBgG, disBgB);
            drawBorder(GFX_BOTTOM, 320, 240, btnPlay.x, btnPlay.y, btnPlay.width, btnPlay.height, 2, disFgR, disFgG, disFgB);
        }

        // NEXT
        drawRect(GFX_BOTTOM, 320, 240, btnNext.x, btnNext.y, btnNext.width, btnNext.height, actBgR, actBgG, actBgB);
        drawBorder(GFX_BOTTOM, 320, 240, btnNext.x, btnNext.y, btnNext.width, btnNext.height, 2, actFgR, actFgG, actFgB);

        printf("\x1b[48;2;33;33;33m\x1b[38;2;29;185;84m\x1b[20;4HPrev");
        
        if (isPlaying) printf("\x1b[48;2;33;33;33m\x1b[38;2;29;185;84m\x1b[20;13HPause");
        else           printf("\x1b[48;2;33;33;33m\x1b[38;2;85;85;85m\x1b[20;13HPause");

        if (!isPlaying) printf("\x1b[48;2;33;33;33m\x1b[38;2;29;185;84m\x1b[20;23HPlay");
        else            printf("\x1b[48;2;33;33;33m\x1b[38;2;85;85;85m\x1b[20;23HPlay");

        printf("\x1b[48;2;33;33;33m\x1b[38;2;29;185;84m\x1b[20;32HNext");

        printf("\x1b[48;2;18;18;18m"); 

        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }

    if (coverPixels) {
        free(coverPixels); 
    }
    curl_global_cleanup();
    socExit();
    free(socBuffer);
    gfxExit();
    
    return 0;
}
