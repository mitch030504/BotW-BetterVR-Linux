#include "pch.h"
#include "cemu_hooks.h"
#include "instance.h"
#include "rendering/openxr.h"
#include "utils/game_utils.h"

std::string CemuHooks::s_currentEvent = {};
std::string CemuHooks::s_currentPlayerNormalState = {};
std::string CemuHooks::s_lastRequestedPlayerNormalState = {};
std::string CemuHooks::s_lastBlockedPlayerNormalState = {};
CemuHooks::HybridEventSettings CemuHooks::s_currentEventSettings = {};
std::unordered_map<std::string, CemuHooks::HybridEventSettings> CemuHooks::s_eventSettings = {};

uint32_t CemuHooks::s_playerAddress = 0;
uint32_t CemuHooks::s_isLadderClimbing = 0;
uint32_t CemuHooks::s_isRiding = 0;
uint32_t CemuHooks::s_isRidingSandSeal = 0;

constexpr CemuHooks::HybridEventSettings defaultFirstPersonSettings = {
    .firstPerson = true,
    .disablePlayerDrivenLinkHands = false,
    .ignoreCameraRotation = true
};

constexpr uint32_t orig_PlayerNormalChangeStateInnerChangeChildFuncAddr = 0x037B8284;
constexpr uint32_t orig_PlayerNormalChangeStateBlockedExitFuncAddr = 0x02D0A994;

static bool ShouldBlockFirstPersonPlayerNormalState(std::string_view stateName, uint32_t stateNamePtr) {
    switch (stateNamePtr) {
    case 0x101E0234:
    case 0x101E0910:
    case 0x101E0930:
    case 0x101E0940:
    case 0x101E0C1C:
        return true;
    default:
        break;
    }

    return stateName == "DamageSUpper";
}

bool CemuHooks::HasActiveCutscene() {
    return !s_currentEvent.empty();
}

EventMode CemuHooks::GetEventModeWithOverride() {
    if (!HasActiveCutscene()) {
        return EventMode::NO_EVENT;
    }

    EventMode mode = GetSettings().GetCutsceneCameraMode();
    // todo: check if user has overriden the cutscene mode during active cutscenes

    // if the camera is controllable, treat it as no event
    // todo: Apparently this is a bad way to check it.
    if (IsInGame()) {
        //Log::print<VERBOSE>("Camera is controllable during cutscene '{}' due to frames since last camera update being {}. Treating as no event.", s_currentEvent, GetFramesSinceLastCameraUpdate());
        //return EventMode::NO_EVENT;
    }
    return mode;
}

std::optional<CemuHooks::HybridEventSettings> CemuHooks::GetFirstPersonSettingsForActiveEvent() {
    auto mode = GetEventModeWithOverride();

    if (mode == EventMode::NO_EVENT) {
        return std::nullopt;
    }
    if (mode == EventMode::ALWAYS_THIRD_PERSON) {
        return std::nullopt;
    }
    // resolve settings if in hybrid mode
    if (mode == EventMode::FOLLOW_DEFAULT_EVENT_SETTINGS) {
        //if (!s_currentEventSettings.firstPerson) {
        //    return std::nullopt;
        //}
    }

    HybridEventSettings settings = s_currentEventSettings;
    if (GetSettings().AlwaysPreventFirstPersonCutsceneCameraMovement()) {
        settings.ignoreCameraRotation = true;
    }

    return settings;
}

bool CemuHooks::IsFirstPerson() {
    if (HasActiveCutscene()) {
        // always third-person
        if (GetSettings().GetCutsceneCameraMode() == EventMode::ALWAYS_THIRD_PERSON) {
            return false;
        }

        // check settings
        if (auto settings = GetFirstPersonSettingsForActiveEvent(); settings.has_value()) {
            // there's an active event

            auto mode = GetEventModeWithOverride();
            if (mode == EventMode::FOLLOW_DEFAULT_EVENT_SETTINGS && !settings->firstPerson) {
                return false;
            }

            // cutscene with first-person settings
            return true;
        }
    }
    else {
        // no event. Check if gameplay is in first-person mode
        if (GetSettings().GetCameraMode() == CameraMode::FIRST_PERSON) {
            return true;
        }
        return false;
    }

    return false;
}

bool CemuHooks::IsThirdPerson() {
    return !IsFirstPerson();
}

bool CemuHooks::UseBlackBarsDuringEvents() {
    if (!HasActiveCutscene() || IsFirstPerson()) {
        return false;
    }

    return GetSettings().UseBlackBarsForCutscenes();
}

CemuHooks::ScreenDebugState CemuHooks::GetScreenDebugState(ScreenId screen) {
    ScreenDebugState result = {};
    uint32_t screenManagerInstance = getMemory<BEType<uint32_t>>(0x1047E650).getLE();
    if (screenManagerInstance != 0) {
        uint32_t screenPtrs = getMemory<BEType<uint32_t>>(screenManagerInstance + 0x18).getLE();
        uint32_t screenPtr = getMemory<BEType<uint32_t>>(screenPtrs + (std::to_underlying(screen) * 4)).getLE();
        result.ptr = screenPtr;
        result.open = screenPtr != 0;
        if (screenPtr != 0) {
            // IsVisible returns true when the screen is open or when its transitioning to be closed/opened
            // matches Screen::isClosed() checks, though inverted.
            result.openPriority = static_cast<int8_t>(getMemory<BEType<uint8_t>>(screenPtr + 0x95).getLE());
            result.state = static_cast<int8_t>(getMemory<BEType<uint8_t>>(screenPtr + 0x96).getLE());
            result.visible = result.state != 0 || result.openPriority > 0;
        }
    }
    return result;
}

bool CemuHooks::IsScreenOpen(ScreenId screen) {
    return GetScreenDebugState(screen).open;
}

bool CemuHooks::IsScreenVisible(ScreenId screen) {
    return GetScreenDebugState(screen).visible;
}

bool CemuHooks::IsAnyFadeScreenVisible() {
    return IsScreenVisible(ScreenId::FadeDemo_00) || IsScreenVisible(ScreenId::Fade) || IsScreenVisible(ScreenId::FadeStatus_00);
}

void CemuHooks::initCutsceneDefaultSettings(uint32_t ppc_TableOfCutsceneEventsSettingsOffset) {
    if (!s_eventSettings.empty()) {
        return;
    }

    char* currPtr = reinterpret_cast<char*>(s_memoryBaseAddress + ppc_TableOfCutsceneEventsSettingsOffset);
    while (true) {
        const std::string line(currPtr);
        if (line.empty()) {
            break;
        }
        size_t commaPos = line.find(',');
        if (commaPos != std::string::npos) {
            HybridEventSettings entry = {};
            std::string settingsStr = line.substr(commaPos + 1);
            std::string eventName = line.substr(0, commaPos);
            size_t pos = 0;
            while ((pos = settingsStr.find(',')) != std::string::npos) {
                std::string setting = settingsStr.substr(0, pos);
                if (setting == "FP_ON") entry.firstPerson = true;
                else if (setting == "FP_OFF")
                    entry.firstPerson = false;
                else if (setting == "HND_ON")
                    entry.disablePlayerDrivenLinkHands = false;
                else if (setting == "HND_OFF")
                    entry.disablePlayerDrivenLinkHands = true;
                else if (setting == "PAN_ON")
                    entry.ignoreCameraRotation = false;
                else if (setting == "PAN_OFF")
                    entry.ignoreCameraRotation = true;
                else if (setting == "CTRL_ON")
                    entry.demoEnableCameraInput = false;
                else if (setting == "CTRL_OFF")
                    entry.demoEnableCameraInput = true;
                else {
                    Log::print<WARNING>("Unknown cutscene default setting: {}", setting);
                }
                settingsStr.erase(0, pos + 1);
            }
            // last setting
            std::string setting = settingsStr;
            if (setting == "FP_ON") entry.firstPerson = true;
            else if (setting == "FP_OFF")
                entry.firstPerson = false;
            else if (setting == "HND_ON")
                entry.disablePlayerDrivenLinkHands = false;
            else if (setting == "HND_OFF")
                entry.disablePlayerDrivenLinkHands = true;
            else if (setting == "PAN_ON")
                entry.ignoreCameraRotation = false;
            else if (setting == "PAN_OFF")
                entry.ignoreCameraRotation = true;
            else if (setting == "CTRL_ON")
                entry.demoEnableCameraInput = false;
            else if (setting == "CTRL_OFF")
                entry.demoEnableCameraInput = true;
            else {
                Log::print<WARNING>("Unknown cutscene default setting: {}", setting);
            }
            s_eventSettings.insert_or_assign(eventName, entry);
        }
        currPtr += line.length() + 1;
    }

    Log::print<VERBOSE>("Initialized cutscene default settings for {} events.", s_eventSettings.size());
}

// todo: Fix sped-up cutscenes. You can go in-game into a save file that didn't have DLC yet, and it'll give you a sped up Demo200_0.
void CemuHooks::hook_GetEventName(PPCInterpreter_t* hCPU) {
    hCPU->instructionPointer = hCPU->sprNew.LR;

    uint32_t isEventActive = hCPU->gpr[3];
    uint32_t eventNamePtr = hCPU->gpr[4];
    uint32_t entryPointNamePtr = hCPU->gpr[5];

    if (isEventActive) {
        std::string eventName = GameUtils::GetString(eventNamePtr);
        std::string entryPointName = GameUtils::GetString(entryPointNamePtr);

        if (s_currentEvent == eventName) {
            return;
        }
        Log::print<INFO>("Event '{}' is now active (using entry point '{}').", eventName, entryPointName);
        s_currentEvent = eventName;

        auto it = s_eventSettings.find(eventName);
        if (it != s_eventSettings.end()) {
            HybridEventSettings settings = it->second;
            Log::print<INFO>(" - First Person: {}", settings.firstPerson ? "ON" : "OFF");
            Log::print<INFO>(" - Ignore Camera Rotation: {}", settings.ignoreCameraRotation ? "ON" : "OFF");
            Log::print<INFO>(" - Disable Player-Driven Link Hands: {}", settings.disablePlayerDrivenLinkHands ? "ON" : "OFF");
            s_currentEventSettings = settings;
        }
        else {
            Log::print<INFO>(" - No specific settings found for this event, using defaults.");
            s_currentEventSettings = defaultFirstPersonSettings;
        }

        // In cutscene's there's somethings a mention of Demo_EnableCameraInput/Demo_EnableCameraControlByUser/Demo_DisableCameraInput
        // These don't actually seem to be hooked up so won't do anything in real-time, but they do flag a cutscene as having camera control disabled for the player.
        // This can be read using the settings.demoEnableCameraInput in the HybridEventSettings struct.
    }
    else if (!s_currentEvent.empty()) {
        Log::print<INFO>("Event '{}' has now ended", s_currentEvent);
        s_currentEvent = "";
    }
}

void CemuHooks::hook_PlayerNormalChangeState(PPCInterpreter_t* hCPU) {
    uint32_t stateNamePtr = hCPU->gpr[4];
    if (stateNamePtr == 0) {
        hCPU->instructionPointer = orig_PlayerNormalChangeStateInnerChangeChildFuncAddr;
        return;
    }

    std::string requestedState = GameUtils::GetString(stateNamePtr);
    s_lastRequestedPlayerNormalState = requestedState;

    if (IsFirstPerson() && ShouldBlockFirstPersonPlayerNormalState(requestedState, stateNamePtr)) {
        if (requestedState != s_lastBlockedPlayerNormalState) {
            Log::print<PPC>("Blocking PlayerNormal state '{}' in first person", requestedState);
            s_lastBlockedPlayerNormalState = requestedState;
        }
        hCPU->instructionPointer = orig_PlayerNormalChangeStateBlockedExitFuncAddr;
        return;
    }

    if (requestedState != s_currentPlayerNormalState) {
        Log::print<PPC>("PlayerNormal state changed to '{}'", requestedState);
        s_currentPlayerNormalState = requestedState;
    }

    hCPU->instructionPointer = orig_PlayerNormalChangeStateInnerChangeChildFuncAddr;
}

void CemuHooks::hook_ShouldSkipEventCamera(PPCInterpreter_t* hCPU) {
    hCPU->instructionPointer = hCPU->sprNew.LR;

    if (IsFirstPerson()) {
        // disable camera rotation for first-person events, according to the event settings
        if (auto eventSettings = GetFirstPersonSettingsForActiveEvent()) {
            if (eventSettings->ignoreCameraRotation) {
                hCPU->gpr[3] = 1;
                return;
            }
        }
    }

    // return 0 to just follow regular camera rotation
    hCPU->gpr[3] = 0;
}

void CemuHooks::hook_PlayerIsRiding(PPCInterpreter_t* hCPU) {
    hCPU->instructionPointer = hCPU->sprNew.LR;

    bool isRiding = hCPU->gpr[3] == 1;
    if (isRiding && IsFirstPerson()) {
        s_isRiding = 2;
    }
}

void CemuHooks::hook_PlayerIsRidingSandSeal(PPCInterpreter_t* hCPU) {
    hCPU->instructionPointer = hCPU->sprNew.LR;

    bool isRiding = hCPU->gpr[3] == 1;
    if (isRiding && IsFirstPerson()) {
        s_isRidingSandSeal = 2;
    }
}

void CemuHooks::hook_PlayerLadderFix(PPCInterpreter_t* hCPU) {
    hCPU->gpr[0] = hCPU->sprNew.LR;
    hCPU->instructionPointer = 0x02D07CEC;

    if (IsFirstPerson()) {
        s_isLadderClimbing = 2;
    }
}
