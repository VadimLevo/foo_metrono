#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#define _CRT_SECURE_NO_WARNINGS

#include <windows.h>
#include <objbase.h>
#include <ole2.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <vector>
#include <string>
#include <mmsystem.h>

// ==================== ПОДКЛЮЧЕНИЕ SDK И РЕСУРСОВ ====================
#include <foobar2000.h>
#include <pfc/pfc.h>
#include "resource.h"

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uxtheme.lib")

using namespace pfc;

// ==================== КОНСТАНТЫ BPM ====================
#define BPM_MIN 10
#define BPM_MAX 600

// ==================== GUID ДЛЯ НАСТРОЕК ====================
static const GUID guid_bpm_tag = { 0x11111111, 0x1111, 0x1111, { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11 } };
static const GUID guid_default_bpm = { 0x22222222, 0x2222, 0x2222, { 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22 } };
static const GUID guid_bpm_step = { 0x55555555, 0x5555, 0x5555, { 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55 } };
static const GUID guid_output_device = { 0x33333333, 0x3333, 0x3333, { 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33 } };
static const GUID guid_always_on_top = { 0x44444444, 0x4444, 0x4444, { 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44 } };

// ==================== ГЛОБАЛЬНЫЕ НАСТРОЙКИ ====================
static cfg_string g_bpm_tag(guid_bpm_tag, "BPM");
static cfg_int g_default_bpm(guid_default_bpm, 120);
static cfg_float g_bpm_step(guid_bpm_step, 0.5f);
static cfg_string g_output_device(guid_output_device, "");
static cfg_bool g_always_on_top(guid_always_on_top, true);

static HINSTANCE g_hInst = NULL;
static class MetronomeWindow* g_metronomeWindow = NULL;

// ==================== ТЁМНАЯ ТЕМА ====================
static HBRUSH g_hbrDarkBackground = NULL;
static HBRUSH g_hbrDarkEdit = NULL;

static bool IsDarkMode() {
    // g_is_dark_mode() безопасен: возвращает false, если ui_config_manager недоступен (fb2k < 2.0)
    return ui_config_manager::g_is_dark_mode();
}

static HBRUSH GetDarkBackgroundBrush() {
    if (!g_hbrDarkBackground) g_hbrDarkBackground = CreateSolidBrush(RGB(45, 45, 45));
    return g_hbrDarkBackground;
}

static HBRUSH GetDarkEditBrush() {
    if (!g_hbrDarkEdit) g_hbrDarkEdit = CreateSolidBrush(RGB(30, 30, 30));
    return g_hbrDarkEdit;
}

static void CleanupDarkBrushes() {
    if (g_hbrDarkBackground) { DeleteObject(g_hbrDarkBackground); g_hbrDarkBackground = NULL; }
    if (g_hbrDarkEdit) { DeleteObject(g_hbrDarkEdit); g_hbrDarkEdit = NULL; }
}

// Применяем тёмную тему к ComboBox через SetWindowTheme
static void ApplyDarkThemeToComboBox(HWND hCombo) {
    if (!hCombo) return;
    SetWindowTheme(hCombo, L"DarkMode_CFD", NULL);
    COMBOBOXINFO cbi = { sizeof(cbi) };
    if (GetComboBoxInfo(hCombo, &cbi) && cbi.hwndList) {
        SetWindowTheme(cbi.hwndList, L"DarkMode_Explorer", NULL);
    }
}

// ==================== ГЕНЕРАТОР ЗВУКА ====================
class AudioGenerator {
private:
    HWAVEOUT hAudioOut;
    WAVEFORMATEX wfx;
    std::vector<short> buffer;
    WAVEHDR waveHeader;
    CRITICAL_SECTION cs;
    bool initialized;
    std::string currentDeviceName;

public:
    AudioGenerator() : hAudioOut(NULL), initialized(false) {
        InitializeCriticalSection(&cs);
        ZeroMemory(&wfx, sizeof(wfx));
        ZeroMemory(&waveHeader, sizeof(waveHeader));
        wfx.wFormatTag = WAVE_FORMAT_PCM;
        wfx.nChannels = 1;
        wfx.nSamplesPerSec = 44100;
        wfx.wBitsPerSample = 16;
        wfx.nBlockAlign = wfx.nChannels * wfx.wBitsPerSample / 8;
        wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
    }

    ~AudioGenerator() {
        Stop();
        DeleteCriticalSection(&cs);
    }

    bool Init(const char* deviceName = NULL) {
        EnterCriticalSection(&cs);
        Stop();

        UINT devId = WAVE_MAPPER;

        if (deviceName && strlen(deviceName) > 0) {
            pfc::stringcvt::string_wide_from_utf8 wDeviceName(deviceName);
            UINT numDevs = waveOutGetNumDevs();
            for (UINT i = 0; i < numDevs; i++) {
                WAVEOUTCAPSW caps;
                if (waveOutGetDevCapsW(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR) {
                    if (wcsstr(caps.szPname, wDeviceName.get_ptr()) != NULL) {
                        devId = i;
                        break;
                    }
                }
            }
        }

        MMRESULT result = waveOutOpen(&hAudioOut, devId, &wfx, 0, 0, CALLBACK_NULL);
        initialized = (result == MMSYSERR_NOERROR);
        if (initialized) {
            currentDeviceName = deviceName ? deviceName : "";
        }

        LeaveCriticalSection(&cs);
        return initialized;
    }

    void Stop() {
        EnterCriticalSection(&cs);
        if (hAudioOut) {
            waveOutReset(hAudioOut);
            if (waveHeader.dwFlags & WHDR_PREPARED) {
                waveOutUnprepareHeader(hAudioOut, &waveHeader, sizeof(WAVEHDR));
            }
            waveOutClose(hAudioOut);
            hAudioOut = NULL;
        }
        ZeroMemory(&waveHeader, sizeof(waveHeader));
        initialized = false;
        LeaveCriticalSection(&cs);
    }

    void PlayTick(float frequency, float amplitude = 0.7f, int durationMs = 30) {
        EnterCriticalSection(&cs);

        if (!initialized || !hAudioOut) {
            LeaveCriticalSection(&cs);
            return;
        }

        if (waveHeader.dwFlags & WHDR_PREPARED) {
            waveOutUnprepareHeader(hAudioOut, &waveHeader, sizeof(WAVEHDR));
            ZeroMemory(&waveHeader, sizeof(waveHeader));
        }

        int sampleCount = (int)(wfx.nSamplesPerSec * durationMs / 1000.0f);
        if (sampleCount <= 0) sampleCount = 100;

        buffer.resize(sampleCount);

        float amp = amplitude * 30000.0f;
        float phase = 0;
        float phaseInc = 2.0f * 3.14159265f * frequency / wfx.nSamplesPerSec;

        for (int i = 0; i < sampleCount; i++) {
            float envelope = (float)exp(-4.0f * i / sampleCount);
            buffer[i] = (short)(amp * sin(phase) * envelope);
            phase += phaseInc;
            if (phase > 2.0f * 3.14159265f) phase -= 2.0f * 3.14159265f;
        }

        waveHeader.lpData = (LPSTR)buffer.data();
        waveHeader.dwBufferLength = (DWORD)(buffer.size() * sizeof(short));
        waveHeader.dwFlags = 0;

        if (waveOutPrepareHeader(hAudioOut, &waveHeader, sizeof(WAVEHDR)) == MMSYSERR_NOERROR) {
            waveOutWrite(hAudioOut, &waveHeader, sizeof(WAVEHDR));
        }

        LeaveCriticalSection(&cs);
    }

    bool IsInitialized() const { return initialized; }

    void Reinit(const char* deviceName = NULL) {
        std::string newDevice = deviceName ? deviceName : "";
        if (!initialized || currentDeviceName != newDevice) {
            Init(deviceName);
        }
    }
};

// ==================== ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ ====================
static pfc::string8 TrimString(const char* str) {
    pfc::string8 result = str;
    size_t start = 0;
    while (start < result.length() && (result[start] == ' ' || result[start] == '\t' || result[start] == '\r' || result[start] == '\n')) {
        ++start;
    }
    if (start > 0) {
        result.remove_chars(0, start);
    }
    size_t end = result.length();
    while (end > 0 && (result[end - 1] == ' ' || result[end - 1] == '\t' || result[end - 1] == '\r' || result[end - 1] == '\n')) {
        --end;
    }
    if (end < result.length()) {
        result.truncate(end);
    }
    return result;
}

float ExtractBPMFromText(const wchar_t* text) {
    if (!text || wcslen(text) == 0) return 0;

    const wchar_t* bpmPos = wcsstr(text, L"BPM");
    if (bpmPos) {
        const wchar_t* numStart = wcsstr(bpmPos, L":");
        if (numStart) {
            numStart++;
            while (*numStart == L' ' || *numStart == L'\t') numStart++;
            float bpm = (float)_wtof(numStart);
            if (bpm > 0 && bpm < 1000) return bpm;
        }
        const wchar_t* numStart2 = bpmPos + 3;
        while (*numStart2 == L' ' || *numStart2 == L':') numStart2++;
        float bpm = (float)_wtof(numStart2);
        if (bpm > 0 && bpm < 1000) return bpm;
    }

    const wchar_t* p = text;
    while (*p) {
        if ((*p >= L'0' && *p <= L'9') || *p == L'.') {
            float bpm = (float)_wtof(p);
            if (bpm > 0 && bpm < 1000) {
                const wchar_t* end = p;
                while (*end && (*end >= L'0' || *end == L'.' || *end == L' ')) end++;
                if (*end == L'\0' || *end == L'\t' || *end == L' ' || *end == L',') {
                    return bpm;
                }
            }
        }
        p++;
    }

    return 0;
}

// ==================== ПОИСК BPM В ОКНЕ "BPM & Key Detector" ====================
float GetBPMFromDetector(metadb_handle_ptr track) {
    if (!track.is_valid()) return 0;

    file_info_impl info;
    if (!track->get_info(info)) return 0;

    const char* title = info.meta_get("TITLE", 0);
    if (!title || strlen(title) == 0) return 0;

    HWND hDetectorWnd = FindWindowW(NULL, L"BPM & Key Detector");
    if (!hDetectorWnd || !IsWindowVisible(hDetectorWnd)) return 0;

    HWND hList = FindWindowExW(hDetectorWnd, NULL, L"SysListView32", NULL);
    if (!hList) {
        hList = FindWindowExW(hDetectorWnd, NULL, L"ListBox", NULL);
    }
    if (!hList) return 0;

    int itemCount = (int)SendMessageW(hList, LVM_GETITEMCOUNT, 0, 0);
    if (itemCount == 0) {
        itemCount = (int)SendMessageW(hList, LB_GETCOUNT, 0, 0);
        if (itemCount == 0) return 0;
    }

    wchar_t wTitle[256];
    MultiByteToWideChar(CP_UTF8, 0, title, -1, wTitle, 256);

    for (int i = 0; i < itemCount; i++) {
        wchar_t itemText[512] = { 0 };
        LVITEMW lvItem;
        ZeroMemory(&lvItem, sizeof(lvItem));
        lvItem.mask = LVIF_TEXT;
        lvItem.iItem = i;
        lvItem.iSubItem = 0;
        lvItem.pszText = itemText;
        lvItem.cchTextMax = 512;

        bool gotItem = false;
        if (SendMessageW(hList, LVM_GETITEM, 0, (LPARAM)&lvItem)) {
            gotItem = true;
        }
        else {
            SendMessageW(hList, LB_GETTEXT, i, (LPARAM)itemText);
            if (wcslen(itemText) > 0) gotItem = true;
        }

        if (!gotItem || wcslen(itemText) == 0) continue;

        if (wcsstr(itemText, wTitle) != NULL) {
            float bpm = 0;
            wchar_t bpmText[256] = { 0 };
            lvItem.iSubItem = 1;
            lvItem.pszText = bpmText;
            lvItem.cchTextMax = 256;

            if (SendMessageW(hList, LVM_GETITEM, 0, (LPARAM)&lvItem)) {
                bpm = ExtractBPMFromText(bpmText);
            }

            if (bpm <= 0) {
                for (int col = 2; col < 5; col++) {
                    wchar_t subText[256] = { 0 };
                    lvItem.iSubItem = col;
                    lvItem.pszText = subText;
                    lvItem.cchTextMax = 256;
                    if (SendMessageW(hList, LVM_GETITEM, 0, (LPARAM)&lvItem)) {
                        bpm = ExtractBPMFromText(subText);
                        if (bpm > 0) break;
                    }
                }
            }

            if (bpm > 0 && bpm < 1000) return bpm;
            break;
        }
    }

    return 0;
}

// ==================== ОКНО МЕТРОНОМА ====================
class MetronomeWindow {
private:
    static const int ID_TIMER = 1001;

    HWND hWnd;
    HWND hChkEnable, hBtnMute, hSliderVolume;
    HWND hEditBPM, hUpDownBPM;
    HWND hEditOffset, hUpDownOffset;
    HWND hChkTimeSignature;
    HWND hStatusLabel;
    HWND hTrackLabel;
    HWND hBtnBpmX2, hBtnBpmDiv2, hBtnBpmRound;

    AudioGenerator* audio;
    bool isEnabled;
    bool isMuted;
    float volume;
    float bpm;
    int offset;
    bool is34;

    bool isPlaying;
    double positionAtStart;
    int currentBeat;

    bool m_isUpdatingControls;

    metadb_handle_ptr currentTrack;
    CRITICAL_SECTION cs;

    float GetBPMFromEdit() {
        wchar_t buf[32] = { 0 };
        GetWindowTextW(hEditBPM, buf, 32);
        for (int i = 0; buf[i]; i++) {
            if (buf[i] == L',') buf[i] = L'.';
        }
        return (float)_wtof(buf);
    }

    int GetOffsetFromEdit() {
        wchar_t buf[16] = { 0 };
        GetWindowTextW(hEditOffset, buf, 16);
        return _wtoi(buf);
    }

    void ResetMetronomeState() {
        positionAtStart = 0;
        currentBeat = -1;
        isPlaying = false;
        if (audio) audio->Stop();
    }

public:
    MetronomeWindow(metadb_handle_ptr initialTrack = NULL) : hWnd(NULL), audio(NULL), isEnabled(true), isMuted(false),
        volume(0.7f), bpm(120.0f), offset(0), is34(false),
        isPlaying(false), positionAtStart(0), currentBeat(-1), m_isUpdatingControls(false), currentTrack(initialTrack) {
        InitializeCriticalSection(&cs);

        audio = new AudioGenerator();
        const char* dev = g_output_device.get_ptr();
        if (dev && strlen(dev) > 0) {
            audio->Init(dev);
        }
        else {
            audio->Init(NULL);
        }
    }

    ~MetronomeWindow() {
        StopMetronome();
        if (audio) {
            delete audio;
            audio = NULL;
        }
        DeleteCriticalSection(&cs);
    }

    bool Create(HWND parent) {
        hWnd = CreateDialogParam(g_hInst, MAKEINTRESOURCE(IDD_MAIN), parent, DialogProc, (LPARAM)this);
        if (!hWnd) return false;

        ApplyAlwaysOnTop();
        CreateControls();

        if (!currentTrack.is_valid()) {
            static_api_ptr_t<playback_control> pc;
            pc->get_now_playing(currentTrack);
            if (!currentTrack.is_valid()) {
                currentTrack = GetSelectedTrack();
            }
        }
        RefreshBPM();
        UpdateControls();

        SetTimer(hWnd, ID_TIMER, 10, NULL);
        return true;
    }

    void ApplyAlwaysOnTop() {
        if (hWnd) {
            SetWindowPos(hWnd, g_always_on_top ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        }
    }

    void ReinitAudio() {
        EnterCriticalSection(&cs);
        if (audio) {
            audio->Reinit(g_output_device.get_ptr());
        }
        LeaveCriticalSection(&cs);
    }

    void RefreshBPM() {
        if (currentTrack.is_valid()) {
            OnTrackChanged(currentTrack);
        }
        else {
            float newBpm = (float)(int)g_default_bpm;
            if (newBpm > 0 && newBpm != bpm) {
                bpm = newBpm;
                UpdateControls();
            }
        }
    }

    void Show() {
        if (hWnd) {
            ShowWindow(hWnd, SW_SHOW);
            SetForegroundWindow(hWnd);
        }
    }

    HWND GetWnd() { return hWnd; }

    void CreateControls() {
        hChkEnable = GetDlgItem(hWnd, IDC_ENABLE_METRONOME);
        hBtnMute = GetDlgItem(hWnd, IDC_BTN_MUTE);
        hSliderVolume = GetDlgItem(hWnd, IDC_SLIDER_VOLUME);
        hEditBPM = GetDlgItem(hWnd, IDC_EDIT_BPM);
        hUpDownBPM = GetDlgItem(hWnd, IDC_UPDOWN_BPM);
        hChkTimeSignature = GetDlgItem(hWnd, IDC_CHK_3_4);
        hEditOffset = GetDlgItem(hWnd, IDC_EDIT_OFFSET);
        hUpDownOffset = GetDlgItem(hWnd, IDC_UPDOWN_OFFSET);
        hStatusLabel = GetDlgItem(hWnd, IDC_STATUS);
        hTrackLabel = GetDlgItem(hWnd, IDC_TRACK);
        hBtnBpmX2 = GetDlgItem(hWnd, IDC_BTN_BPM_X2);
        hBtnBpmDiv2 = GetDlgItem(hWnd, IDC_BTN_BPM_DIV2);
        hBtnBpmRound = GetDlgItem(hWnd, IDC_BTN_BPM_ROUND);

        SendMessage(hSliderVolume, TBM_SETRANGE, TRUE, MAKELONG(0, 100));
        SendMessage(hSliderVolume, TBM_SETPOS, TRUE, (int)(volume * 100));
        SendMessage(hUpDownBPM, UDM_SETBUDDY, (WPARAM)hEditBPM, 0);
        SendMessage(hUpDownBPM, UDM_SETRANGE, 0, MAKELONG(BPM_MAX, BPM_MIN));
        SendMessage(hUpDownOffset, UDM_SETBUDDY, (WPARAM)hEditOffset, 0);
        SendMessage(hUpDownOffset, UDM_SETRANGE, 0, MAKELONG(64, -64));
    }

    void UpdateControls() {
        if (m_isUpdatingControls) return;
        m_isUpdatingControls = true;

        wchar_t buf[64];
        swprintf(buf, 64, L"%.2f", bpm);
        SetWindowTextW(hEditBPM, buf);

        swprintf(buf, 64, L"%d", offset);
        SetWindowTextW(hEditOffset, buf);

        SendMessage(hChkEnable, BM_SETCHECK, isEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessage(hChkTimeSignature, BM_SETCHECK, is34 ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessage(hSliderVolume, TBM_SETPOS, TRUE, (int)(volume * 100));

        SetWindowTextW(hBtnMute, isMuted ? L"🔇 Unmute" : L"🔊 Mute");
        SetWindowTextW(hStatusLabel, isPlaying ? L"Playing" : L"Stopped");

        pfc::string8 trackDisplay;
        if (currentTrack.is_valid()) {
            file_info_impl info;
            if (currentTrack->get_info(info)) {
                const char* artist = info.meta_get("ARTIST", 0);
                const char* title = info.meta_get("TITLE", 0);
                const char* album = info.meta_get("ALBUM", 0);

                if (artist && *artist && title && *title) {
                    pfc::string_formatter formatter;
                    if (album && *album) {
                        formatter << artist << " - " << title << " [" << album << "]";
                    }
                    else {
                        formatter << artist << " - " << title;
                    }
                    trackDisplay = formatter;
                }
                else if (title && *title) {
                    trackDisplay = title;
                }
                else if (artist && *artist) {
                    trackDisplay = artist;
                }
                else {
                    pfc::string8 path = currentTrack->get_path();
                    const char* lastSlash = strrchr(path.get_ptr(), '\\');
                    if (!lastSlash) lastSlash = strrchr(path.get_ptr(), '/');
                    trackDisplay = lastSlash ? (lastSlash + 1) : path.get_ptr();
                }
            }
        }

        SetWindowTextW(hTrackLabel, trackDisplay.is_empty() ? L"(none)" : pfc::stringcvt::string_wide_from_utf8(trackDisplay).get_ptr());
        m_isUpdatingControls = false;
    }

    void StartMetronome() {
        EnterCriticalSection(&cs);
        if (isPlaying || !isEnabled) {
            LeaveCriticalSection(&cs);
            return;
        }

        static_api_ptr_t<playback_control> pc;
        if (!pc->is_playing()) {
            LeaveCriticalSection(&cs);
            return;
        }

        if (audio) {
            audio->Reinit(g_output_device.get_ptr());
        }

        positionAtStart = pc->playback_get_position();
        if (positionAtStart < 0) positionAtStart = 0;

        currentBeat = -1;
        isPlaying = true;

        UpdateControls();
        LeaveCriticalSection(&cs);
    }

    void StopMetronome() {
        EnterCriticalSection(&cs);
        if (!isPlaying) {
            LeaveCriticalSection(&cs);
            return;
        }

        isPlaying = false;
        if (audio) audio->Stop();

        UpdateControls();
        LeaveCriticalSection(&cs);
    }

    void TickMetronome() {
        static_api_ptr_t<playback_control> pc;
        bool isPcPlaying = pc->is_playing();

        if (!isPcPlaying) {
            if (isPlaying) StopMetronome();
            metadb_handle_ptr newTrack = GetSelectedTrack();
            if (newTrack.is_valid() && newTrack != currentTrack) {
                currentTrack = newTrack;
                OnTrackChanged(newTrack);
                offset = 0;
                UpdateControls();
            }
            return;
        }

        if (!isEnabled) {
            if (isPlaying) StopMetronome();
            return;
        }

        metadb_handle_ptr newTrack;
        pc->get_now_playing(newTrack);

        if (newTrack.is_valid() && newTrack != currentTrack) {
            currentTrack = newTrack;
            ResetMetronomeState();
            OnTrackChanged(newTrack);
            offset = 0;
            UpdateControls();
            StartMetronome();
            return;
        }

        if (!isPlaying) {
            StartMetronome();
            return;
        }

        EnterCriticalSection(&cs);

        double currentPos = pc->playback_get_position();
        if (currentPos < 0) {
            StopMetronome();
            LeaveCriticalSection(&cs);
            return;
        }

        double elapsed = currentPos - positionAtStart;
        if (elapsed > 0.5) {
            positionAtStart = currentPos - elapsed;
        }

        double beatDuration = 60.0 / bpm;
        double offsetSeconds = (offset / 16.0) * beatDuration;
        double adjustedPos = elapsed - offsetSeconds;

        if (adjustedPos < 0) {
            LeaveCriticalSection(&cs);
            return;
        }

        int totalBeats = (int)(adjustedPos / beatDuration);
        int beatNumber = totalBeats % (is34 ? 3 : 4);

        if (beatNumber != currentBeat) {
            currentBeat = beatNumber;

            if (!isMuted && audio && audio->IsInitialized()) {
                if (beatNumber == 0) {
                    audio->PlayTick(440.0f, volume * 0.8f, 30);
                }
                else {
                    audio->PlayTick(880.0f, volume * 0.5f, 20);
                }
            }
        }

        LeaveCriticalSection(&cs);
    }

    void OnTrackChanged(metadb_handle_ptr track) {
        if (!track.is_valid()) return;

        currentTrack = track;
        float newBpm = 0;

        // 1. BPM & Key Detector
        newBpm = GetBPMFromDetector(track);

        // 2. Tag
        if (newBpm <= 0) {
            newBpm = GetBPMFromTag(track);
        }

        // 3. Default
        if (newBpm <= 0) {
            newBpm = (float)(int)g_default_bpm;
        }

        if (newBpm > 0 && newBpm != bpm) {
            bpm = newBpm;
            UpdateControls();
        }
    }

    float GetBPMFromTag(metadb_handle_ptr track) {
        if (!track.is_valid()) return 0;

        file_info_impl info;
        if (track->get_info(info)) {
            const char* tagValue = info.meta_get(g_bpm_tag.get_ptr(), 0);
            if (tagValue && strlen(tagValue) > 0) {
                pfc::string8 val = TrimString(tagValue);
                return (float)atof(val);
            }
        }
        return 0;
    }

    metadb_handle_ptr GetSelectedTrack() {
        static_api_ptr_t<playlist_manager> pm;
        t_size playlist = pm->get_active_playlist();
        if (playlist == pfc::infinite_size) return NULL;
        t_size count = pm->playlist_get_item_count(playlist);
        for (t_size i = 0; i < count; ++i) {
            if (pm->playlist_is_item_selected(playlist, i)) {
                return pm->playlist_get_item_handle(playlist, i);
            }
        }
        return NULL;
    }

    static INT_PTR CALLBACK DialogProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp) {
        MetronomeWindow* self = (MetronomeWindow*)GetWindowLongPtr(hWnd, GWLP_USERDATA);

        switch (msg) {
        case WM_INITDIALOG:
        {
            self = (MetronomeWindow*)lp;
            SetWindowLongPtr(hWnd, GWLP_USERDATA, (LONG_PTR)self);

            HICON hIcon = LoadIcon(g_hInst, MAKEINTRESOURCE(IDI_METRONO));
            if (hIcon != NULL) {
                SendMessage(hWnd, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);
                SendMessage(hWnd, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
            }
        }
        return TRUE;

        case WM_TIMER:
            if (self && wp == ID_TIMER) {
                self->TickMetronome();
            }
            return 0;

        case WM_NOTIFY:
            if (self) {
                NMHDR* pnmh = (NMHDR*)lp;
                if (pnmh->hwndFrom == self->hUpDownBPM && pnmh->code == UDN_DELTAPOS) {
                    NMUPDOWN* pnmud = (NMUPDOWN*)pnmh;
                    float currentBpm = self->GetBPMFromEdit();
                    if (currentBpm <= 0) currentBpm = self->bpm;
                    float step = (float)g_bpm_step;
                    if (step <= 0) step = 0.5f;
                    float newBpm = currentBpm + pnmud->iDelta * step;
                    if (newBpm < BPM_MIN) newBpm = BPM_MIN;
                    if (newBpm > BPM_MAX) newBpm = BPM_MAX;
                    self->bpm = newBpm;
                    self->UpdateControls();
                    return TRUE;
                }
                if (pnmh->hwndFrom == self->hUpDownOffset && pnmh->code == UDN_DELTAPOS) {
                    NMUPDOWN* pnmud = (NMUPDOWN*)pnmh;
                    int currentOffset = self->GetOffsetFromEdit();
                    int newOffset = currentOffset + pnmud->iDelta;
                    if (newOffset < -64) newOffset = -64;
                    if (newOffset > 64) newOffset = 64;
                    self->offset = newOffset;
                    self->UpdateControls();
                    return TRUE;
                }
            }
            break;

        case WM_COMMAND:
            if (!self) return FALSE;

            switch (LOWORD(wp)) {
            case IDC_ENABLE_METRONOME:
                self->isEnabled = (SendMessage(self->hChkEnable, BM_GETCHECK, 0, 0) == BST_CHECKED);
                self->UpdateControls();
                if (!self->isEnabled && self->isPlaying) {
                    self->StopMetronome();
                }
                return TRUE;

            case IDC_BTN_MUTE:
                self->isMuted = !self->isMuted;
                self->UpdateControls();
                return TRUE;

            case IDC_EDIT_BPM:
                if (HIWORD(wp) == EN_CHANGE && !self->m_isUpdatingControls) {
                    float newBpm = self->GetBPMFromEdit();
                    if (newBpm >= BPM_MIN && newBpm <= BPM_MAX) {
                        self->bpm = newBpm;
                    }
                }
                return TRUE;

            case IDC_BTN_BPM_X2:
            {
                float newBpm = self->bpm * 2.0f;
                if (newBpm > BPM_MAX) newBpm = BPM_MAX;
                self->bpm = newBpm;
                self->UpdateControls();
                return TRUE;
            }

            case IDC_BTN_BPM_DIV2:
            {
                float newBpm = self->bpm / 2.0f;
                if (newBpm < BPM_MIN) newBpm = BPM_MIN;
                self->bpm = newBpm;
                self->UpdateControls();
                return TRUE;
            }

            case IDC_BTN_BPM_ROUND:
            {
                float newBpm = (float)(int)(self->bpm + 0.5f);
                if (newBpm < BPM_MIN) newBpm = BPM_MIN;
                if (newBpm > BPM_MAX) newBpm = BPM_MAX;
                self->bpm = newBpm;
                self->UpdateControls();
                return TRUE;
            }

            case IDC_CHK_3_4:
                self->is34 = (SendMessage(self->hChkTimeSignature, BM_GETCHECK, 0, 0) == BST_CHECKED);
                self->UpdateControls();
                return TRUE;

            case IDC_EDIT_OFFSET:
                if (HIWORD(wp) == EN_CHANGE && !self->m_isUpdatingControls) {
                    int newOffset = self->GetOffsetFromEdit();
                    if (newOffset >= -64 && newOffset <= 64) {
                        self->offset = newOffset;
                    }
                }
                return TRUE;
            }
            break;

        case WM_HSCROLL:
            if (self && (HWND)lp == self->hSliderVolume) {
                self->volume = SendMessage(self->hSliderVolume, TBM_GETPOS, 0, 0) / 100.0f;
            }
            return TRUE;

        case WM_CLOSE:
            DestroyWindow(hWnd);
            return TRUE;

        case WM_NCDESTROY:
            if (self) {
                KillTimer(hWnd, ID_TIMER);
                self->hWnd = NULL;
                SetWindowLongPtr(hWnd, GWLP_USERDATA, 0);
                delete self;
                g_metronomeWindow = NULL;
            }
            return TRUE;
        }
        return FALSE;
    }
};

// ==================== КОНТЕКСТНОЕ МЕНЮ ====================
class MetronomeContextMenu : public contextmenu_item_simple {
public:
    virtual unsigned get_num_items() {
        return 1;
    }

    virtual void get_item_name(unsigned p_index, pfc::string_base& p_out) {
        p_out = "Metronome";
    }

    virtual GUID get_item_guid(unsigned p_index) {
        static const GUID guid = { 0x87654321, 0x9ABC, 0xDEF0, { 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0 } };
        return guid;
    }

    virtual bool get_item_description(unsigned p_index, pfc::string_base& p_out) {
        p_out = "Opens a metronome window synchronized with playback";
        return true;
    }

    virtual void context_command(unsigned p_index, metadb_handle_list_cref p_data, const GUID& p_caller) {
        metadb_handle_ptr track = NULL;
        if (p_data.get_count() > 0) {
            track = p_data[0];
        }

        if (g_metronomeWindow) {
            g_metronomeWindow->Show();
            if (track.is_valid()) {
                g_metronomeWindow->OnTrackChanged(track);
            }
            return;
        }

        MetronomeWindow* window = new MetronomeWindow(track);
        if (window->Create(core_api::get_main_window())) {
            window->Show();
            g_metronomeWindow = window;
        }
        else {
            delete window;
        }
    }

    virtual contextmenu_item::t_enabled_state get_enabled_state(unsigned p_index) {
        return contextmenu_item::DEFAULT_ON;
    }
};

// ==================== СТРАНИЦА НАСТРОЕК ====================
class MetronomePreferences : public preferences_page_v3 {
public:
    virtual const char* get_name() {
        return "Metronome";
    }

    virtual GUID get_guid() {
        static const GUID guid = { 0xABCDEF12, 0x3456, 0x7890, { 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0 } };
        return guid;
    }

    virtual GUID get_parent_guid() {
        return preferences_page::guid_tools;
    }

    virtual double get_sort_priority() {
        return 0;
    }

#ifdef _WIN32
    virtual preferences_page_instance::ptr instantiate(fb2k::hwnd_t parent, preferences_page_callback::ptr callback) {
        return new service_impl_t<PreferencesInstance>(parent, callback);
    }
#endif

    class PreferencesInstance : public preferences_page_instance {
    private:
        HWND hWnd;
        preferences_page_callback::ptr callback;
        bool m_changed;
        pfc::string8 m_initialBpmTag;
        int m_initialDefaultBpm;
        float m_initialBpmStep;
        pfc::string8 m_initialOutputDevice;
        bool m_initialAlwaysOnTop;

        void LoadValues() {
            m_initialBpmTag = TrimString(g_bpm_tag.get_ptr());
            m_initialDefaultBpm = (int)g_default_bpm;
            m_initialBpmStep = (float)g_bpm_step;
            m_initialOutputDevice = TrimString(g_output_device.get_ptr());
            m_initialAlwaysOnTop = g_always_on_top;
            m_changed = false;
        }

        void CheckChanges() {
            char buf[256];
            GetDlgItemTextA(hWnd, IDC_BPM_TAG, buf, 256);
            pfc::string8 currentTag = TrimString(buf);

            GetDlgItemTextA(hWnd, IDC_DEFAULT_BPM, buf, 16);
            int currentDefaultBpm = atoi(buf);

            GetDlgItemTextA(hWnd, IDC_BPM_STEP, buf, 16);
            float currentBpmStep = (float)atof(buf);
            if (currentBpmStep <= 0) currentBpmStep = 0.5f;

            HWND hCombo = GetDlgItem(hWnd, IDC_OUTPUT_DEVICE);
            int idx = SendMessage(hCombo, CB_GETCURSEL, 0, 0);
            pfc::string8 currentDevice;
            if (idx != CB_ERR) {
                wchar_t wbuf[256];
                SendMessageW(hCombo, CB_GETLBTEXT, idx, (LPARAM)wbuf);
                char bufOut[256];
                WideCharToMultiByte(CP_ACP, 0, wbuf, -1, bufOut, 256, NULL, NULL);
                currentDevice = TrimString(bufOut);
            }

            bool currentAlwaysOnTop = (IsDlgButtonChecked(hWnd, IDC_ALWAYS_ON_TOP) == BST_CHECKED);

            bool changed = false;
            if (strcmp(currentTag, m_initialBpmTag) != 0) changed = true;
            if (currentDefaultBpm != m_initialDefaultBpm) changed = true;
            if (fabs(currentBpmStep - m_initialBpmStep) > 0.001f) changed = true;
            if (strcmp(currentDevice, m_initialOutputDevice) != 0) changed = true;
            if (currentAlwaysOnTop != m_initialAlwaysOnTop) changed = true;

            if (changed != m_changed) {
                m_changed = changed;
                if (callback.is_valid()) {
                    callback->on_state_changed();
                }
            }
        }

    public:
        PreferencesInstance(HWND parent, preferences_page_callback::ptr cb) : hWnd(NULL), callback(cb), m_changed(false) {
            LoadValues();

            hWnd = CreateDialogParam(g_hInst, MAKEINTRESOURCE(IDD_DIALOG1), parent, DialogProc, (LPARAM)this);
            if (hWnd) {
                SetDlgItemTextA(hWnd, IDC_BPM_TAG, g_bpm_tag.get_ptr());

                char buf[16];
                sprintf(buf, "%d", (int)g_default_bpm);
                SetDlgItemTextA(hWnd, IDC_DEFAULT_BPM, buf);

                char stepBuf[16];
                sprintf(stepBuf, "%.2f", (float)g_bpm_step);
                SetDlgItemTextA(hWnd, IDC_BPM_STEP, stepBuf);

                HWND hCombo = GetDlgItem(hWnd, IDC_OUTPUT_DEVICE);

                SendMessageW(hCombo, CB_RESETCONTENT, 0, 0);
                SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"Default");

                UINT numDevs = waveOutGetNumDevs();
                for (UINT i = 0; i < numDevs; i++) {
                    WAVEOUTCAPSW caps;
                    if (waveOutGetDevCapsW(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR) {
                        SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)caps.szPname);
                    }
                }

                const char* current = g_output_device.get_ptr();
                if (current && strlen(current) > 0) {
                    pfc::stringcvt::string_wide_from_utf8 wcurrent(current);
                    int idx = SendMessageW(hCombo, CB_FINDSTRINGEXACT, -1, (LPARAM)wcurrent.get_ptr());
                    if (idx != CB_ERR) {
                        SendMessageW(hCombo, CB_SETCURSEL, idx, 0);
                    }
                }
                else {
                    SendMessageW(hCombo, CB_SETCURSEL, 0, 0);
                }

                CheckDlgButton(hWnd, IDC_ALWAYS_ON_TOP, g_always_on_top ? BST_CHECKED : BST_UNCHECKED);

                // Применяем тёмную тему к ComboBox (только если тёмный режим включён)
                if (IsDarkMode()) {
                    ApplyDarkThemeToComboBox(hCombo);
                }

                LoadValues();
            }
        }

        ~PreferencesInstance() {
            if (hWnd) {
                DestroyWindow(hWnd);
                hWnd = NULL;
            }
        }

        virtual t_uint32 get_state() {
            t_uint32 state = 0;
            if (m_changed) state |= preferences_state::changed;
            state |= preferences_state::resettable | preferences_state::dark_mode_supported;
            return state;
        }

        virtual fb2k::hwnd_t get_wnd() {
            return hWnd;
        }

        virtual void apply() {
            char buf[256];
            GetDlgItemTextA(hWnd, IDC_BPM_TAG, buf, 256);
            if (strlen(buf) > 0) g_bpm_tag = buf;

            GetDlgItemTextA(hWnd, IDC_DEFAULT_BPM, buf, 16);
            int val = atoi(buf);
            if (val >= BPM_MIN && val <= BPM_MAX) g_default_bpm = val;

            GetDlgItemTextA(hWnd, IDC_BPM_STEP, buf, 16);
            float step = (float)atof(buf);
            if (step > 0) g_bpm_step = step;

            HWND hCombo = GetDlgItem(hWnd, IDC_OUTPUT_DEVICE);
            int idx = SendMessageW(hCombo, CB_GETCURSEL, 0, 0);
            if (idx != CB_ERR) {
                wchar_t wbuf[256] = { 0 };
                SendMessageW(hCombo, CB_GETLBTEXT, idx, (LPARAM)wbuf);
                if (wcscmp(wbuf, L"Default") != 0) {
                    pfc::stringcvt::string_utf8_from_wide utf8_buf(wbuf);
                    g_output_device = utf8_buf.get_ptr();
                }
                else {
                    g_output_device = "";
                }
            }

            g_always_on_top = (IsDlgButtonChecked(hWnd, IDC_ALWAYS_ON_TOP) == BST_CHECKED);

            if (g_metronomeWindow) {
                g_metronomeWindow->ApplyAlwaysOnTop();
                g_metronomeWindow->ReinitAudio();
                g_metronomeWindow->RefreshBPM();
            }

            LoadValues();
            m_changed = false;
        }

        virtual void reset() {
            g_bpm_tag = "BPM";
            g_default_bpm = 120;
            g_bpm_step = 0.5f;
            g_output_device = "";
            g_always_on_top = true;

            SetDlgItemTextA(hWnd, IDC_BPM_TAG, g_bpm_tag.get_ptr());
            char buf[16];
            sprintf(buf, "%d", (int)g_default_bpm);
            SetDlgItemTextA(hWnd, IDC_DEFAULT_BPM, buf);
            SetDlgItemTextA(hWnd, IDC_BPM_STEP, "0.50");

            HWND hCombo = GetDlgItem(hWnd, IDC_OUTPUT_DEVICE);
            SendMessageW(hCombo, CB_SETCURSEL, 0, 0);

            CheckDlgButton(hWnd, IDC_ALWAYS_ON_TOP, g_always_on_top ? BST_CHECKED : BST_UNCHECKED);

            if (g_metronomeWindow) {
                g_metronomeWindow->ApplyAlwaysOnTop();
                g_metronomeWindow->ReinitAudio();
                g_metronomeWindow->RefreshBPM();
            }

            LoadValues();
            m_changed = false;
            if (callback.is_valid()) {
                callback->on_state_changed();
            }
        }

        static INT_PTR CALLBACK DialogProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
            PreferencesInstance* self = (PreferencesInstance*)GetWindowLongPtr(hwnd, GWLP_USERDATA);

            switch (msg) {
            case WM_INITDIALOG:
                self = (PreferencesInstance*)lp;
                SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)self);
                return TRUE;

            case WM_COMMAND:
                if (self) {
                    self->CheckChanges();
                }
                return TRUE;

            case WM_CTLCOLORDLG:
            {
                if (IsDarkMode()) {
                    return (INT_PTR)GetDarkBackgroundBrush();
                }
            }
            break;

            case WM_CTLCOLORSTATIC:
            {
                if (IsDarkMode()) {
                    HDC hdc = (HDC)wp;
                    SetTextColor(hdc, RGB(220, 220, 220));
                    SetBkMode(hdc, TRANSPARENT);
                    return (INT_PTR)GetDarkBackgroundBrush();
                }
            }
            break;

            case WM_CTLCOLOREDIT:
            {
                if (IsDarkMode()) {
                    HDC hdc = (HDC)wp;
                    SetTextColor(hdc, RGB(220, 220, 220));
                    SetBkColor(hdc, RGB(30, 30, 30));
                    return (INT_PTR)GetDarkEditBrush();
                }
            }
            break;

            case WM_CTLCOLORLISTBOX:
            {
                if (IsDarkMode()) {
                    HDC hdc = (HDC)wp;
                    SetTextColor(hdc, RGB(220, 220, 220));
                    SetBkColor(hdc, RGB(30, 30, 30));
                    return (INT_PTR)GetDarkEditBrush();
                }
            }
            break;
            }
            return FALSE;
        }
    };
};

// ==================== ИНФОРМАЦИЯ О КОМПОНЕНТЕ ====================
DECLARE_COMPONENT_VERSION(
    "Metronome",
    "1.0.1",
    "Metronome synchronized with foobar2000 playback.\n"
    "- Reads BPM from BPM & Key Detector, tags, default\n"
    "- Shift resets to 0 on track change\n"
    "- Configurable arrow step"
);

// ==================== РЕГИСТРАЦИЯ СЕРВИСОВ ====================
static contextmenu_item_factory_t<MetronomeContextMenu> g_metronome_menu;
static preferences_page_factory_t<MetronomePreferences> g_metronome_prefs;

// ==================== DllMain ====================
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        g_hInst = (HINSTANCE)hModule;
        DisableThreadLibraryCalls(hModule);

        INITCOMMONCONTROLSEX icc;
        icc.dwSize = sizeof(icc);
        icc.dwICC = ICC_UPDOWN_CLASS | ICC_BAR_CLASSES;
        InitCommonControlsEx(&icc);
    }
    else if (ul_reason_for_call == DLL_PROCESS_DETACH) {
        CleanupDarkBrushes();
    }
    return TRUE;
}