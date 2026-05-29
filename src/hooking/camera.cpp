#include "pch.h"

#include "cemu_hooks.h"
#include "instance.h"
#include "rendering/openxr.h"
#include "utils/debug_draw.h"
#include "utils/game_utils.h"
#include "utils/render_utils.h"

bool CemuHooks::UseMonoFrameBufferTemporarilyDuringMenusOrPictures() {
    auto* renderer = VRManager::instance().XR->GetRenderer();
    const bool isCapturingFrozenFrame = renderer != nullptr && renderer->IsGameCapturing3DFrameBuffer();
#ifndef _WIN32
    // On Linux, the Vulkan framebuffer copy path does not use this helper to
    // suppress 3D copies. Keeping pause menus "mono" here only prevents the
    // stereo camera/projection hooks from rewriting the pause menu's 3D Link
    // preview and any shared render state it touches.
    return isCapturingFrozenFrame
        || IsScreenVisible(ScreenId::PauseMenu_00)
        || IsScreenVisible(ScreenId::PauseMenuInfo_00);
#else
    return IsScreenOpen(ScreenId::PauseMenuInfo_00) || isCapturingFrozenFrame;
#endif
}

static std::optional<XrFovf> TryGetRenderFOV(OpenXR::EyeSide side, long frameIdx = -1) {
    auto* renderer = VRManager::instance().XR->GetRenderer();
    if (renderer == nullptr) {
        return std::nullopt;
    }

    return RenderUtils::GetRenderFov(renderer->GetFOV(side, frameIdx), renderer->m_gameRenderAspectRatio);
}

void CemuHooks::hook_BeginCameraSide(PPCInterpreter_t* hCPU) {
    hCPU->instructionPointer = hCPU->sprNew.LR;

    OpenXR::EyeSide side = hCPU->gpr[0] == 0 ? OpenXR::EyeSide::LEFT : OpenXR::EyeSide::RIGHT;

    Log::print<RENDERING>("");
    Log::print<RENDERING>("===============================================================================");
    Log::print<RENDERING>("{0} {0} {0} {0} {0} {0} {0} {0} {0} {0} {0} {0} {0} {0} {0} {0} {0} {0}", side);
}

static std::optional<XrFovf> GetGameProjectionFOV(OpenXR::EyeSide side, const BESeadPerspectiveProjection& perspectiveProjection, long frameIdx = -1) {
    auto* renderer = VRManager::instance().XR->GetRenderer();
    if (renderer == nullptr) {
        return std::nullopt;
    }

    return RenderUtils::ResolveGameProjectionFov(
        renderer->GetFOV(side, frameIdx),
        renderer->m_gameRenderAspectRatio,
        perspectiveProjection.fovYRadiansOrAngle.getLE(),
        perspectiveProjection.aspect.getLE()
    );
}


float hardcodedSwimOffset = 0.0f;
float hardcodedRidingOffset = 0.65f;
float hardcodedCrouchOffset = 0.3f;

glm::fvec3 s_wsCameraPosition = glm::fvec3();
glm::fquat s_wsCameraRotation = glm::identity<glm::fquat>();
glm::fvec3 s_lastGameplayCameraTarget = glm::fvec3();
bool s_hasGameplayCameraTarget = false;
bool s_isSwimming = false;
bool s_isCrouching = false;
bool s_wasCrouching = false;
float actualCrouchOffset = 0.0f;
std::chrono::steady_clock::time_point crouch_state_change_time;

static glm::fvec3 GetAppliedHeadsetOffset(glm::fvec3 eyePos) {
    if (CemuHooks::IsFirstPerson()) {
        glm::fvec3 appliedHeadPos = CemuHooks::GetAppliedRoomscaleHeadPosition();
        eyePos.x -= appliedHeadPos.x;
        eyePos.z -= appliedHeadPos.z;
    }
    return eyePos;
}

static glm::mat4 GetAppliedHeadsetPose(glm::mat4 pose) {
    if (CemuHooks::IsFirstPerson()) {
        glm::fvec3 appliedHeadPos = CemuHooks::GetAppliedRoomscaleHeadPosition();
        pose[3].x -= appliedHeadPos.x;
        pose[3].z -= appliedHeadPos.z;
    }
    return pose;
}

static std::optional<std::pair<glm::fvec3, glm::fquat>> TryGetAppliedEyePose(OpenXR::EyeSide side) {
    auto* renderer = VRManager::instance().XR->GetRenderer();
    if (renderer == nullptr) {
        return std::nullopt;
    }

    auto eyePoseOpt = renderer->GetPose(side);
    if (!eyePoseOpt.has_value()) {
        return std::nullopt;
    }

    return { { GetAppliedHeadsetOffset(ToGLM(eyePoseOpt.value().position)), ToGLM(eyePoseOpt.value().orientation) } };
}

static bool TryUpdateGameplayCameraTarget(const glm::fvec3& cameraPos, const glm::fvec3& cameraTarget, const glm::fvec3& cameraUp) {
    if (!glm::all(glm::isfinite(cameraPos)) || !glm::all(glm::isfinite(cameraTarget)) || !glm::all(glm::isfinite(cameraUp))) {
        return false;
    }

    glm::fvec3 forward = cameraTarget - cameraPos;
    if (glm::dot(forward, forward) <= 1.0e-6f || glm::dot(cameraUp, cameraUp) <= 1.0e-6f) {
        return false;
    }

    s_lastGameplayCameraTarget = cameraTarget;
    s_hasGameplayCameraTarget = true;
    return true;
}

static glm::fvec3 ResolveGameplayAnchorPosition(const glm::fvec3& gameplayPos) {
    glm::fvec3 newPos = gameplayPos;
    if (CemuHooks::IsFirstPerson()) {
        BEMatrix34 playerMtx = {};
        CemuHooks::readMemory(CemuHooks::s_playerMtxAddress, &playerMtx);
        glm::fvec3 playerPos = playerMtx.getPos().getLE();

        if (CemuHooks::IsRiding()) {
            playerPos.y -= hardcodedRidingOffset + GetSettings().GetPlayerHeightOffset();
        }
        else if (s_isSwimming) {
            playerPos.y += hardcodedSwimOffset + GetSettings().GetPlayerHeightOffset();
        }
        else {
            playerPos.y += GetSettings().GetPlayerHeightOffset() - actualCrouchOffset;
        }

#ifndef _WIN32
        // Push camera forward from head center toward eyes in Link's facing direction.
        // Linux-only: compensates for the Vulkan capture/pose path; upstream Windows is unchanged.
        glm::fquat playerRot = playerMtx.getRotLE();
        glm::fvec3 playerForward = playerRot * glm::fvec3(0.0f, 0.0f, 1.0f);
        constexpr float kEyeForwardOffset = 0.15f;
        playerPos += playerForward * kEyeForwardOffset;
#endif

        newPos = playerPos;
    }

    return newPos;
}

static std::pair<glm::vec3, glm::fquat> BuildGameplayCameraPose(const glm::fvec3& gameplayPos, const glm::fquat& gameplayRot, OpenXR::EyeSide side) {
    glm::fvec3 basePos = ResolveGameplayAnchorPosition(gameplayPos);
    glm::fquat baseYaw = RenderUtils::swingTwistY(gameplayRot).second;

    auto eyePoseOpt = TryGetAppliedEyePose(side);
    if (!eyePoseOpt.has_value()) {
        return { basePos, gameplayRot };
    }

    auto [eyePos, eyeRot] = eyePoseOpt.value();
    return { basePos + (baseYaw * eyePos), baseYaw * eyeRot };
}

static void UpdateGameplayReferenceCameraMtx(const glm::fvec3& gameplayPos, const glm::fquat& gameplayRot) {
    glm::fvec3 basePos = ResolveGameplayAnchorPosition(gameplayPos);
    glm::fquat baseYaw = RenderUtils::swingTwistY(gameplayRot).second;
    CemuHooks::s_lastCameraMtx = glm::translate(glm::identity<glm::fmat4>(), basePos) * glm::mat4(baseYaw);
}

static std::pair<glm::fvec3, glm::fquat> ResolveGameplayBasePose(const BESeadLookAtCamera& camera) {
    glm::mat4x3 viewMatrix = camera.mtx.getLEMatrix();
    glm::mat4 worldGame = glm::inverse(glm::mat4(viewMatrix));
    glm::fvec3 basePos = glm::vec3(worldGame[3]);
    glm::fquat baseRot = glm::quat_cast(worldGame);

    if (!s_hasGameplayCameraTarget || CemuHooks::GetFramesSinceLastCameraUpdate() > 4) {
        return { basePos, baseRot };
    }

    return { s_wsCameraPosition, s_wsCameraRotation };
}

void CemuHooks::hook_UpdateCameraForGameplay(PPCInterpreter_t* hCPU) {
    hCPU->instructionPointer = hCPU->sprNew.LR;

    if (UseBlackBarsDuringEvents()) {
        return;
    }

    // read the camera matrix from the game's memory
    uint32_t ppc_cameraMatrixOffsetIn = hCPU->gpr[31];
    ActCamera actCam = {};
    readMemory(ppc_cameraMatrixOffsetIn, &actCam);

    // extract components from the existing camera matrix
    glm::fvec3 oldCameraPosition = actCam.finalCamMtx.pos.getLE();
    glm::fvec3 oldCameraTarget = actCam.finalCamMtx.target.getLE();
    glm::fvec3 oldCameraUp = actCam.finalCamMtx.up.getLE();
    float oldCameraDistance = glm::distance(oldCameraPosition, oldCameraTarget);

    Log::print<RENDERING>("Getting gameplay camera (pos = {})", oldCameraPosition);

    if (IsFirstPerson()) {
        // remove verticality from the camera position to avoid pitch changes that aren't from the VR headset
        oldCameraPosition.y = oldCameraTarget.y;
    }

    // construct glm matrix from the existing camera parameters
    glm::mat4 existingGameMtx = glm::lookAtRH(oldCameraPosition, oldCameraTarget, oldCameraUp);
    glm::fquat gameplayRotation = glm::quat_cast(glm::inverse(existingGameMtx));

    s_wsCameraPosition = oldCameraPosition;
    s_wsCameraRotation = gameplayRotation;

    // rebase the rotation to the player position
    if (IsFirstPerson()) {
        // check if player is swimming
        Player actor;
        readMemory(s_playerAddress, &actor);

        PlayerMoveBitFlags moveBits = actor.moveBitFlags.getLE();
        s_isSwimming = HAS_FLAG(moveBits, PlayerMoveBitFlags::IS_SWIMMING_OR_CLIMBING | PlayerMoveBitFlags::IS_SWIMMING);
        s_isCrouching = HAS_FLAG(moveBits, PlayerMoveBitFlags::IS_CROUCHING);

        // Todo: move those and their hooks in controls.cpp ?
        auto gameState = VRManager::instance().XR->m_gameState.load();
        // Unreliable flag, need to investigate
        gameState.is_climbing = HAS_FLAG(moveBits, PlayerMoveBitFlags::IS_SWIMMING_OR_CLIMBING | PlayerMoveBitFlags::IS_CLIMBING_WALL) || s_isLadderClimbing == 2;
        gameState.is_riding_mount = CemuHooks::IsRiding(false);
        gameState.is_paragliding = HAS_FLAG(moveBits, PlayerMoveBitFlags::IS_GLIDER_ACTIVE);
        VRManager::instance().XR->m_gameState.store(gameState);

        auto now = std::chrono::steady_clock::now();
        std::chrono::milliseconds crouchLerpDuration{ 150 };
        if (s_isCrouching != s_wasCrouching) {
            crouch_state_change_time = now;
        }
        auto test = 0.8f;
        if (now <= crouch_state_change_time + crouchLerpDuration) {
            auto elapsed = std::chrono::duration<float>(now - crouch_state_change_time);
            auto duration = std::chrono::duration<float>(crouchLerpDuration);
            float t = elapsed.count() / duration.count();
            t = glm::clamp(t, 0.0f, 1.0f);
            if (s_isCrouching) {
                actualCrouchOffset = glm::mix(0.0f, test, t);
            }
            else {
                actualCrouchOffset = glm::mix(test, 0.0f, t);
            }
        }
        else {
            actualCrouchOffset = s_isCrouching ? test : 0.0f;
        }

        glm::fvec3 playerPos = actor.mtx.getPos().getLE();

        if (CemuHooks::IsRiding()) {
            playerPos.y -= hardcodedRidingOffset + GetSettings().GetPlayerHeightOffset();
        }
        else if (s_isSwimming) {
            float playerHeight = VRManager::instance().XR->GetRenderer()->GetMiddlePose().value()[3].y;
            playerPos.y += 1.73f - playerHeight;
        }
        else {
            playerPos.y += GetSettings().GetPlayerHeightOffset() - actualCrouchOffset;
        }


        if (s_isLadderClimbing > 0) {
            s_isLadderClimbing--;
        }
        if (s_isRiding > 0) {
            s_isRiding--;
        }
        if (s_isRidingSandSeal > 0) {
            s_isRidingSandSeal--;
        }

        s_wasCrouching = s_isCrouching;

        glm::mat4 playerMtx4 = glm::inverse(glm::translate(glm::identity<glm::mat4>(), playerPos) * glm::mat4(gameplayRotation));
        existingGameMtx = playerMtx4;
    }

    // current VR headset camera matrix
    auto viewsOpt = VRManager::instance().XR->GetRenderer()->GetMiddlePose();
    if (!viewsOpt) {
        Log::print<ERROR>("hook_UpdateCameraForGameplay: No views available for the middle pose.");
        return;
    }
    glm::mat4 views = GetAppliedHeadsetPose(viewsOpt.value());

    // calculate final camera matrix
    glm::mat4 finalPose = glm::inverse(existingGameMtx) * views;

    // extract camera up, forward and position from the final matrix
    glm::fvec3 camPos = glm::fvec3(finalPose[3]);
    glm::fvec3 forward = -glm::normalize(glm::fvec3(finalPose[2]));
    glm::fvec3 up = glm::normalize(glm::fvec3(finalPose[1]));
    glm::fvec3 target = camPos + forward * oldCameraDistance;

    if (!TryUpdateGameplayCameraTarget(camPos, target, up)) {
        return;
    }

    UpdateGameplayReferenceCameraMtx(s_wsCameraPosition, s_wsCameraRotation);

    actCam.finalCamMtx.pos = camPos;
    actCam.finalCamMtx.target = target;
    actCam.finalCamMtx.up = up;

    // write back the modified camera matrix to the game's memory
    uint32_t ppc_cameraMatrixOffsetOut = hCPU->gpr[31];
    writeMemory(ppc_cameraMatrixOffsetOut, &actCam);
    s_framesSinceLastCameraUpdate = 0;
}

// the gameplay camera is just a look-at camera that doesn't seem to have a pivot point at the point we hook it
// so this hook adjusts it afterwards to have properly working gameplay camera rotations
void CemuHooks::hook_AdjustGameplayCameraPivot(PPCInterpreter_t* hCPU) {
    hCPU->instructionPointer = hCPU->sprNew.LR;

    if (IsThirdPerson() || !s_hasGameplayCameraTarget || GetFramesSinceLastCameraUpdate() > 4 || UseBlackBarsDuringEvents()) {
        return;
    }

    uint32_t pivotPtr = hCPU->gpr[4];
    if (pivotPtr == 0) {
        return;
    }

    BEVec3 pivot = {};
    readMemory(pivotPtr, &pivot);

    glm::fvec3 currentPivot = pivot.getLE();
    glm::fvec3 pivotDelta = s_lastGameplayCameraTarget - currentPivot;
    if (!std::isfinite(pivotDelta.x) || !std::isfinite(pivotDelta.y) || !std::isfinite(pivotDelta.z) || glm::dot(pivotDelta, pivotDelta) <= 1.0e-6f) {
        return;
    }

    pivot = currentPivot + pivotDelta;
    writeMemory(pivotPtr, &pivot);
}

void CemuHooks::hook_FixStaminaGaugeScreenPosition(PPCInterpreter_t* hCPU) {
    hCPU->instructionPointer = hCPU->sprNew.LR;

    if (IsThirdPerson()) {
        return;
    }

    BEVec2 staminaGauge2DPos;
    readMemory(hCPU->gpr[4], &staminaGauge2DPos);
    BEVec3 playerPosToCameraPos;
    readMemory(hCPU->gpr[5], &playerPosToCameraPos);

    Log::print<PPC>("Fixing stamina gauge position (oldPos = {}, playerPosToCameraPos = {})", staminaGauge2DPos.getLE(), playerPosToCameraPos);

    staminaGauge2DPos = BEVec2{ -482.0f, +170.0f }; // nested under hearts
    //staminaGauge2DPos = BEVec2{ 380.0f, 300.0f }; // above the status bars but at the top of the screen
    //staminaGauge2DPos = BEVec2{ -220.0f, 300.0f }; // above the status bars but at the top of the screen
    playerPosToCameraPos = BEVec3{ 0.0f, 0.0, 0.0f };

    writeMemory(hCPU->gpr[4], &staminaGauge2DPos);
    writeMemory(hCPU->gpr[5], &playerPosToCameraPos);
}

void CemuHooks::hook_FixExtraStaminaGaugeIconPositions(PPCInterpreter_t* hCPU) {
    hCPU->instructionPointer = hCPU->sprNew.LR;

    // original instruction that got replaced
    hCPU->fpr[29].fp0 = 1.0f;

    if (IsThirdPerson()) {
        return;
    }

    // manually jump
    hCPU->instructionPointer = 0x02FB29C4;
}

glm::mat4 CemuHooks::s_lastCameraMtx = glm::mat4(1.0f);

void CemuHooks::hook_GetRenderCamera(PPCInterpreter_t* hCPU) {
    hCPU->instructionPointer = hCPU->sprNew.LR;
    uint32_t cameraIn = hCPU->gpr[3];
    uint32_t cameraOut = hCPU->gpr[12];
    EyeSide side = hCPU->gpr[11] == 0 ? EyeSide::LEFT : EyeSide::RIGHT;

    bool suppressCamera = UseBlackBarsDuringEvents();
#ifndef _WIN32
    // Linux-only: also suppress the camera write during mono/menu frames.
    suppressCamera = suppressCamera || UseMonoFrameBufferTemporarilyDuringMenusOrPictures();
#endif
    if (suppressCamera) {
        return;
    }

    BESeadLookAtCamera camera = {};
    readMemory(cameraIn, &camera);

    Log::print<RENDERING>("[{}] Getting render camera", side);
    auto [gameplayPos, gameplayRot] = ResolveGameplayBasePose(camera);
    UpdateGameplayReferenceCameraMtx(gameplayPos, gameplayRot);
    auto [newPos, newRot] = BuildGameplayCameraPose(gameplayPos, gameplayRot, side);

    glm::mat4 newWorldVR = glm::translate(glm::mat4(1.0f), newPos) * glm::mat4_cast(newRot);
    glm::mat4 newViewVR = glm::inverse(newWorldVR);
    DebugDraw::instance().UpdateEyeView(side, newViewVR);

    camera.mtx.setLEMatrix(newViewVR);

    camera.pos = newPos;

    // Set look-at point by offsetting position in view direction
    glm::vec3 viewDir = -glm::vec3(newViewVR[2]); // Forward direction is -Z in view space
    camera.at = newPos + viewDir;

    // Transform world up vector by new rotation
    glm::vec3 upDir = glm::vec3(newViewVR[1]); // Up direction is +Y in view space
    camera.up = upDir;


    //glm::mat4 workingMtx = glm::inverse(glm::lookAtRH(newPos, newPos + glm::vec3(newViewVR[2]), glm::fvec3(0, 1, 0)));
    //s_lastCameraMtx = workingMtx;

    writeMemory(cameraOut, &camera);
    hCPU->gpr[3] = cameraOut;
}

constexpr uint32_t seadOrthoProjection = 0x1027B5BC;
constexpr uint32_t seadPerspectiveProjection = 0x1027B54C;

void CemuHooks::hook_GetRenderProjection(PPCInterpreter_t* hCPU) {
    hCPU->instructionPointer = hCPU->sprNew.LR;

    if (UseBlackBarsDuringEvents()) {
        return;
    }

    if (UseMonoFrameBufferTemporarilyDuringMenusOrPictures()) {
        return;
    }


    uint32_t projectionIn = hCPU->gpr[3];
    uint32_t projectionOut = hCPU->gpr[12];
    OpenXR::EyeSide side = hCPU->gpr[0] == 0 ? EyeSide::LEFT : EyeSide::RIGHT;

    BESeadPerspectiveProjection perspectiveProjection = {};
    readMemory(projectionIn, &perspectiveProjection);

    Log::print<RENDERING>("[{}] Render Proj. (LR: {:08X}): {}", side, hCPU->sprNew.LR, perspectiveProjection);

    if (perspectiveProjection.zFar == 10000.0f) {
        return;
    }

    perspectiveProjection.zFar = GetSettings().GetZFar();
    perspectiveProjection.zNear = GetSettings().GetZNear();

    auto currFovOpt = GetGameProjectionFOV(side, perspectiveProjection);
    if (!currFovOpt.has_value()) {
        return;
    }
    XrFovf currFOV = currFovOpt.value();
    auto newProjection = RenderUtils::CalculateFOVAndOffset(currFOV);

    perspectiveProjection.aspect = newProjection.aspectRatio;
    perspectiveProjection.fovYRadiansOrAngle = newProjection.fovY;
    float halfAngle = newProjection.fovY.getLE() * 0.5f;
    perspectiveProjection.fovySin = sinf(halfAngle);
    perspectiveProjection.fovyCos = cosf(halfAngle);
    perspectiveProjection.fovyTan = tanf(halfAngle);
    perspectiveProjection.offset.x = newProjection.offsetX;
    perspectiveProjection.offset.y = newProjection.offsetY;

    glm::fmat4 newMatrix = RenderUtils::CalculateProjectionMatrix(perspectiveProjection.zNear.getLE(), perspectiveProjection.zFar.getLE(), currFOV);
    perspectiveProjection.matrix = newMatrix;

    // calculate device matrix
    glm::fmat4 newDeviceMatrix = newMatrix;

    float zScale = perspectiveProjection.deviceZScale.getLE();
    float zOffset = perspectiveProjection.deviceZOffset.getLE();

    newDeviceMatrix[2][0] *= zScale;
    newDeviceMatrix[2][1] *= zScale;
    newDeviceMatrix[2][2] = (newDeviceMatrix[2][2] + newDeviceMatrix[3][2] * zOffset) * zScale;
    newDeviceMatrix[2][3] = newDeviceMatrix[2][3] * zScale + newDeviceMatrix[3][3] * zOffset;

    DebugDraw::instance().UpdateEyeProjection(side, glm::transpose(newDeviceMatrix));
    perspectiveProjection.deviceMatrix = newDeviceMatrix;

    perspectiveProjection.dirty = false;
    perspectiveProjection.deviceDirty = false;

    writeMemory(projectionOut, &perspectiveProjection);
    hCPU->gpr[3] = projectionOut;
}


void CemuHooks::hook_ModifyLightPrePassProjectionMatrix(PPCInterpreter_t* hCPU) {
    hCPU->instructionPointer = hCPU->sprNew.LR;

    if (VRManager::instance().XR->GetRenderer() == nullptr) {
        return;
    }

    if (UseBlackBarsDuringEvents()) {
        return;
    }
    
    if (UseMonoFrameBufferTemporarilyDuringMenusOrPictures()) {
        return;
    }

    uint32_t projectionIn = hCPU->gpr[3];
    OpenXR::EyeSide side = hCPU->gpr[11] == 0 ? EyeSide::LEFT : EyeSide::RIGHT;

    BESeadPerspectiveProjection perspectiveProjection = {};
    readMemory(projectionIn, &perspectiveProjection);

    auto currFovOpt = GetGameProjectionFOV(side, perspectiveProjection);
    if (!currFovOpt.has_value()) {
        return;
    }

    Log::print<RENDERING>("[{}] Modify light prepass projection", side);


    XrFovf currFOV = currFovOpt.value();
    auto newProjection = RenderUtils::CalculateFOVAndOffset(currFOV);

    perspectiveProjection.aspect = newProjection.aspectRatio;
    perspectiveProjection.fovYRadiansOrAngle = newProjection.fovY;
    float halfAngle = newProjection.fovY.getLE() * 0.5f;
    perspectiveProjection.fovySin = sinf(halfAngle);
    perspectiveProjection.fovyCos = cosf(halfAngle);
    perspectiveProjection.fovyTan = tanf(halfAngle);
    perspectiveProjection.offset.x = newProjection.offsetX;
    perspectiveProjection.offset.y = newProjection.offsetY;

    glm::fmat4 newMatrix = RenderUtils::CalculateProjectionMatrix(perspectiveProjection.zNear.getLE(), perspectiveProjection.zFar.getLE(), currFOV);
    perspectiveProjection.matrix = newMatrix;

    // calculate device matrix
    glm::fmat4 newDeviceMatrix = newMatrix;

    float zScale = perspectiveProjection.deviceZScale.getLE();
    float zOffset = perspectiveProjection.deviceZOffset.getLE();

    newDeviceMatrix[2][0] *= zScale;
    newDeviceMatrix[2][1] *= zScale;
    newDeviceMatrix[2][2] = (newDeviceMatrix[2][2] + newDeviceMatrix[3][2] * zOffset) * zScale;
    newDeviceMatrix[2][3] = newDeviceMatrix[2][3] * zScale + newDeviceMatrix[3][3] * zOffset;

    perspectiveProjection.deviceMatrix = newDeviceMatrix;

    perspectiveProjection.dirty = false;
    perspectiveProjection.deviceDirty = false;

    writeMemory(projectionIn, &perspectiveProjection);
}

void CemuHooks::hook_OverwriteSeadPerspectiveProjectionSet(PPCInterpreter_t* hCPU) {
    hCPU->instructionPointer = hCPU->sprNew.LR;
}

void CemuHooks::hook_ModifyProjectionUsingCamera(PPCInterpreter_t* hCPU) {
    hCPU->instructionPointer = hCPU->sprNew.LR;

    if (VRManager::instance().XR->GetRenderer() == nullptr) {
        return;
    }

    if (UseBlackBarsDuringEvents() || UseMonoFrameBufferTemporarilyDuringMenusOrPictures()) {
        return;
    }

    uint32_t projectionPtr = hCPU->gpr[4];
    uint32_t cameraPtr = hCPU->gpr[7];
    OpenXR::EyeSide side = hCPU->gpr[5] == 0 ? EyeSide::LEFT : EyeSide::RIGHT;

    // this is always true, since we currently only hook one caller
    if (hCPU->gpr[6] == 0x02C43454) {
        BESeadLookAtCamera camera = {};
        readMemory(cameraPtr, &camera);

        Log::print<RENDERING>("[{}] ModifyProjectionUsingCamera at {:08X}: {}", side, cameraPtr, camera);
        auto [gameplayPos, gameplayRot] = ResolveGameplayBasePose(camera);
        auto [newPos, newRot] = BuildGameplayCameraPose(gameplayPos, gameplayRot, side);

        glm::mat4 newWorldVR = glm::translate(glm::mat4(1.0f), newPos) * glm::mat4_cast(newRot);
        glm::mat4 newViewVR = glm::inverse(newWorldVR);
        camera.mtx.setLEMatrix(newViewVR);

        camera.pos = newPos;

        // Set look-at point by offsetting position in view direction
        glm::vec3 viewDir = -glm::vec3(newViewVR[2]); // Forward direction is -Z in view space
        camera.at = newPos + viewDir;

        // Transform world up vector by new rotation
        glm::vec3 upDir = glm::vec3(newViewVR[1]); // Up direction is +Y in view space
        camera.up = upDir;

        writeMemory(cameraPtr, &camera);
    }

    BESeadPerspectiveProjection perspectiveProjection = {};
    readMemory(projectionPtr, &perspectiveProjection);

    auto currFovOpt = GetGameProjectionFOV(side, perspectiveProjection);
    if (!currFovOpt.has_value()) {
        return;
    }

    Log::print<RENDERING>("[{}] ModifyProjectionUsingCamera: {}", side, perspectiveProjection);

    XrFovf currFOV = currFovOpt.value();
    auto newProjection = RenderUtils::CalculateFOVAndOffset(currFOV);

    perspectiveProjection.aspect = newProjection.aspectRatio;
    perspectiveProjection.fovYRadiansOrAngle = newProjection.fovY;
    float halfAngle = newProjection.fovY.getLE() * 0.5f;
    perspectiveProjection.fovySin = sinf(halfAngle);
    perspectiveProjection.fovyCos = cosf(halfAngle);
    perspectiveProjection.fovyTan = tanf(halfAngle);
    perspectiveProjection.offset.x = newProjection.offsetX;
    perspectiveProjection.offset.y = newProjection.offsetY;

    glm::fmat4 newMatrix = RenderUtils::CalculateProjectionMatrix(perspectiveProjection.zNear.getLE(), perspectiveProjection.zFar.getLE(), currFOV);
    perspectiveProjection.matrix = newMatrix;

    // calculate device matrix
    glm::fmat4 newDeviceMatrix = newMatrix;

    float zScale = perspectiveProjection.deviceZScale.getLE();
    float zOffset = perspectiveProjection.deviceZOffset.getLE();

    newDeviceMatrix[2][0] *= zScale;
    newDeviceMatrix[2][1] *= zScale;
    newDeviceMatrix[2][2] = (newDeviceMatrix[2][2] + newDeviceMatrix[3][2] * zOffset) * zScale;
    newDeviceMatrix[2][3] = newDeviceMatrix[2][3] * zScale + newDeviceMatrix[3][3] * zOffset;

    perspectiveProjection.deviceMatrix = newDeviceMatrix;

    perspectiveProjection.dirty = false;
    perspectiveProjection.deviceDirty = false;

    writeMemory(projectionPtr, &perspectiveProjection);
}

std::pair<glm::vec3, glm::fquat> CemuHooks::CalculateVRWorldPose(const BESeadLookAtCamera& camera, uint8_t side) {
    auto [gameplayPos, gameplayRot] = ResolveGameplayBasePose(camera);
    return BuildGameplayCameraPose(gameplayPos, gameplayRot, (OpenXR::EyeSide)side);
}

void CemuHooks::hook_CheckIfCameraCanSeePos(PPCInterpreter_t* hCPU) {
    hCPU->instructionPointer = hCPU->sprNew.LR;

    if (VRManager::instance().XR->GetRenderer() == nullptr) {
        hCPU->gpr[3] = 0;
        return;
    }

    uint32_t camPtr = hCPU->gpr[3];
    uint32_t posPtr = hCPU->gpr[4];
    float radius = hCPU->fpr[1].fp0;
    float nearClip = hCPU->fpr[2].fp0;
    float farClip = hCPU->fpr[3].fp0;

    BESeadLookAtCamera camera = {};
    readMemory(camPtr, &camera);

    BEVec3 center;
    readMemory(posPtr, &center);

    Frustum frustum;
    bool visible = false;

    for (int i = 0; i < 2; ++i) {
        OpenXR::EyeSide side = (i == 0) ? EyeSide::LEFT : EyeSide::RIGHT;
        if (auto fovOpt = TryGetRenderFOV(side)) {
            auto [pos, rot] = CalculateVRWorldPose(camera, side);

            // pull the camera backwards a bit to account for it being a third-person game that encompassed a bigger area
            pos += rot * glm::vec3(0.0f, 0.0f, 1.0f);

            glm::mat4 view = glm::inverse(glm::translate(glm::mat4(1.0f), pos) * glm::mat4_cast(rot));
            glm::mat4 proj = glm::transpose(RenderUtils::CalculateProjectionMatrix(nearClip, farClip, fovOpt.value()));
            glm::mat4 vp = proj * view;

            frustum.update(vp);
            if (frustum.checkSphere(center.getLE(), radius)) {
                visible = true;
                break;
            }
        }
    }

    Log::print<PPC>("Checking visibility of {} (rad = {}, near = {}, far = {}): {}", center, radius, nearClip, farClip, visible ? "visible" : "invisible");

    hCPU->gpr[3] = visible ? 1 : 0;
}


void CemuHooks::hook_ModifyPixelUniformBlockData(PPCInterpreter_t* hCPU) {
    hCPU->instructionPointer = hCPU->sprNew.LR;

    if (UseMonoFrameBufferTemporarilyDuringMenusOrPictures()) {
        return;
    }

    auto currFovOpt = TryGetRenderFOV(EyeSide::RIGHT);
    if (!currFovOpt.has_value()) {
        return;
    }

    glm::fvec4 ubData = {};
    readMemory(hCPU->gpr[5], &ubData);

    XrFovf currFOV = currFovOpt.value();
    auto newProjection = RenderUtils::CalculateFOVAndOffset(currFOV);

    ubData.x = 0.5f + newProjection.offsetX.getLE();
    ubData.y = 0.5f + newProjection.offsetY.getLE();

    writeMemory(hCPU->gpr[5], &ubData);
}

void CemuHooks::hook_EndCameraSide(PPCInterpreter_t* hCPU) {
    hCPU->instructionPointer = hCPU->sprNew.LR;

    OpenXR::EyeSide side = hCPU->gpr[3] == 0 ? OpenXR::EyeSide::LEFT : OpenXR::EyeSide::RIGHT;

    // todo: sometimes this can deadlock apparently?
    if (VRManager::instance().XR->GetRenderer()->IsInitialized() && side == OpenXR::EyeSide::RIGHT) {
        m_heldWeaponsLastUpdate[0]++;
        m_heldWeaponsLastUpdate[1]++;
        if (m_heldWeaponsLastUpdate[0] >= 6) {
            m_heldWeapons[0] = 0;
        }
        if (m_heldWeaponsLastUpdate[1] >= 6) {
            m_heldWeapons[1] = 0;
        }
    }

    Log::print<RENDERING>("{0} {0} {0} {0} {0} {0} {0} {0} {0} {0} {0} {0} {0} {0} {0} {0} {0} {0}", side);
    Log::print<RENDERING>("===============================================================================");
    Log::print<RENDERING>("");
}

void CemuHooks::hook_UseCameraDistance(PPCInterpreter_t* hCPU) {
    hCPU->instructionPointer = hCPU->sprNew.LR;

    if (IsFirstPerson()) {
        hCPU->fpr[13].fp0 = 0.0f;
    }
    else if (GetSettings().GetCameraMode() == CameraMode::THIRD_PERSON) {
        hCPU->fpr[13].fp0 = GetSettings().thirdPlayerDistance;
    }
    else {
        hCPU->fpr[13].fp0 = 0.5f; // use default distance when using the first-person camera
    }
}

void CemuHooks::hook_SetActorOpacity(PPCInterpreter_t* hCPU) {
    hCPU->instructionPointer = hCPU->sprNew.LR;

    float toBeSetOpacity = hCPU->fpr[1].fp0;
    uint32_t actorPtr = hCPU->gpr[3];

    ActorWiiU actor;
    readMemory(actorPtr, &actor);

    // normal behavior if it wasn't the player or a held weapon
    if (actor.modelOpacity.getLE() != toBeSetOpacity) {
        uint8_t opacityOrDoFlushOpacityToGPU = 1;
        writeMemoryBE(actorPtr + offsetof(ActorWiiU, modelOpacity), &toBeSetOpacity);
        writeMemoryBE(actorPtr + offsetof(ActorWiiU, opacityOrDoFlushOpacityToGPU), &opacityOrDoFlushOpacityToGPU);
    }
}

void CemuHooks::hook_CalculateModelOpacity(PPCInterpreter_t* hCPU) {
    hCPU->instructionPointer = hCPU->sprNew.LR;

    // overwrites return value to 1 if in first person mode
    if (IsFirstPerson()) {
        hCPU->fpr[1].fp0 = 1.0f;
    }
}


struct CameraParamValueOffset {
    std::string name;
    uint32_t offsetInsideCamera;
    bool storedOriginalValue = false;
    float originalValue;
};

struct ActionFloatParamPointerOverride {
    std::string name;
    uint32_t destPointerAddress;
    bool storedOriginalPointer = false;
    uint32_t originalPointer = 0;
};

// key = vtable address, value = list of parameter names and their offsets inside the camera object
std::mutex storedCameraParametersLock;
std::unordered_map<uint32_t, std::vector<CameraParamValueOffset>> storedCameraParameters;
std::mutex storedActionFloatParamsLock;
std::unordered_map<uint32_t, std::vector<ActionFloatParamPointerOverride>> storedActionFloatParams;

static void ApplyStoredActionFloatParamOverrides() {
    constexpr uint32_t kPlayerLaunchZeroFloat = 0x101D3CC8;

    std::scoped_lock lock(storedActionFloatParamsLock);

    for (auto& entries : storedActionFloatParams | std::views::values) {
        for (auto& entry : entries) {
            uint32_t currentPointer = CemuHooks::getMemory<BEType<uint32_t>>(entry.destPointerAddress).getLE();
            if (!entry.storedOriginalPointer && currentPointer != 0 && currentPointer != kPlayerLaunchZeroFloat) {
                entry.originalPointer = currentPointer;
                entry.storedOriginalPointer = true;
            }

            if (!entry.storedOriginalPointer) {
                continue;
            }

            uint32_t targetPointer = CemuHooks::IsFirstPerson() ? kPlayerLaunchZeroFloat : entry.originalPointer;
            if (currentPointer != targetPointer) {
                CemuHooks::setMemory<uint32_t>(entry.destPointerAddress, targetPointer);
            }
        }
    }
}

void CemuHooks::hook_ReplaceCameraMode(PPCInterpreter_t* hCPU) {
    hCPU->instructionPointer = hCPU->sprNew.LR;

    uint32_t currCameraInstance = hCPU->gpr[3];
    uint32_t cameraChaseInstance = hCPU->gpr[4]; // this is currently a pointer to the regular camera mode (CameraChase)
    uint32_t currentCameraVtbl = hCPU->gpr[5];


    // check if any patched parameters exist for this camera vtbl
    {
        std::scoped_lock(storedCameraParametersLock);

        auto it = storedCameraParameters.find(currCameraInstance);
        if (it != storedCameraParameters.end()) {
            for (auto& paramEntry : it->second) {
                uint32_t originalValuePtr = getMemory<BEType<uint32_t>>(paramEntry.offsetInsideCamera).getLE();
                BEType<float>* paramValueBE = (BEType<float>*)(s_memoryBaseAddress + originalValuePtr);

                // on first patch, store original value
                if (!paramEntry.storedOriginalValue) {
                    paramEntry.originalValue = paramValueBE->getLE();
                    paramEntry.storedOriginalValue = true;
                }

                if (IsFirstPerson()) {
                    // set to zero in first person
                    *paramValueBE = 0.0f;
                }
                else {
                    // restore original value in third person
                    *paramValueBE = paramEntry.originalValue;
                }
            }
        }
    }

    constexpr uint32_t kCameraTailVtbl = 0x101BC278;
    constexpr uint32_t kCameraMagneCatchVtbl = 0x101BAB4C;

    static uint32_t s_lastLoggedCameraVtbl = 0;
    static uint32_t s_lastLoggedCameraInstance = 0;
    static uint32_t s_lastLoggedCameraChaseInstance = 0;
    bool vtblChanged = currentCameraVtbl != s_lastLoggedCameraVtbl;
    bool cameraInstanceChanged = currCameraInstance != s_lastLoggedCameraInstance;
    bool cameraChaseInstanceChanged = cameraChaseInstance != s_lastLoggedCameraChaseInstance;
    if (vtblChanged || cameraInstanceChanged || cameraChaseInstanceChanged) {
        Log::print<INFO>("Camera changed (vtbl={:#X}, camera={:#X}, chase={:#X}, vtbl_changed={}, camera_changed={}, chase_changed={})", currentCameraVtbl, currCameraInstance, cameraChaseInstance, vtblChanged, cameraInstanceChanged, cameraChaseInstanceChanged);
        s_lastLoggedCameraVtbl = currentCameraVtbl;
        s_lastLoggedCameraInstance = currCameraInstance;
        s_lastLoggedCameraChaseInstance = cameraChaseInstance;
    }

    //hCPU->gpr[3] = cameraChaseInstance;

    if (hCPU->gpr[5] == kCameraMagneCatchVtbl) {
        if (IsFirstPerson()) {
            //hCPU->gpr[3] = cameraChaseMode;
        }
    }

    if (hCPU->gpr[5] == kCameraTailVtbl) {
        //Log::print<RENDERING>("Current camera mode: {:#X}, tail mode: {:#X}, vtbl: {:#X}", currentCameraMode, cameraTailMode, currentCameraVtbl);
        if (IsFirstPerson()) {
            // overwrite to tail mode
            //hCPU->gpr[3] = cameraTailMode;
        }
    }

    //Log::print<INFO>("Camera mode: {:#X}, tail mode: {:#X}, vtbl: {:#X}", currCameraInstance, cameraChaseInstance, currentCameraVtbl);
}

void CemuHooks::UpdateFloatParamOverrides() {
    ApplyStoredActionFloatParamOverrides();
}

constexpr uint32_t orig_GetStaticParam_float_funcAddr = 0x030E9BE0;

static bool ShouldZeroFirstPersonDamageFloatParam(std::string_view paramName) {
    return paramName.find("Speed") != std::string_view::npos || paramName.starts_with("JumpHeight") || paramName.starts_with("AddLinearImpulse") || paramName.starts_with("AddRollImpulse") || paramName == "NoRagdollTime";
}

// hook for ksys::act::ai::ActionBase::getStaticParam<FLOAT> calls
void CemuHooks::hook_OverwriteFloatParam(PPCInterpreter_t* hCPU) {
    uint32_t actionPtr = hCPU->gpr[3];
    uint32_t destFloatPtr = hCPU->gpr[4];
    uint32_t paramNameArgPtr = hCPU->gpr[5];
    if (actionPtr == 0 || destFloatPtr == 0 || paramNameArgPtr == 0) {
        hCPU->instructionPointer = orig_GetStaticParam_float_funcAddr;
        return;
    }

    uint32_t paramNamePtr = getMemory<uint32_t>(paramNameArgPtr).getLE();
    if (paramNamePtr == 0) {
        hCPU->instructionPointer = orig_GetStaticParam_float_funcAddr;
        return;
    }

    const char* paramName = (const char*)(s_memoryBaseAddress + paramNamePtr);
    std::string_view paramNameStr = paramName;
    if (ShouldZeroFirstPersonDamageFloatParam(paramNameStr)) {
        std::string paramNameOwned = std::string(paramNameStr);

        {
            std::scoped_lock lock(storedActionFloatParamsLock);

            auto& paramList = storedActionFloatParams[actionPtr];
            auto it = std::ranges::find_if(paramList, [&paramNameOwned](const ActionFloatParamPointerOverride& entry) {
                return entry.name == paramNameOwned;
            });
            if (it == paramList.end()) {
                Log::print<PPC>("Storing float param '{}' offset {:08X} for action at {:08X}", paramNameOwned, destFloatPtr, actionPtr);
                paramList.push_back({ paramNameOwned, destFloatPtr });
            }
        }

        hCPU->instructionPointer = orig_GetStaticParam_float_funcAddr;
        return;
    }

    {
        std::string paramNameOwned = std::string(paramNameStr);

        std::scoped_lock(storedCameraParametersLock);

        auto& paramList = storedCameraParameters[actionPtr];
        // store parameter offset if not already stored
        auto it = std::ranges::find_if(paramList, [&paramNameOwned](const CameraParamValueOffset& entry) {
            return entry.name == paramNameOwned;
        });
        if (it == paramList.end()) {
            hCPU->instructionPointer = orig_GetStaticParam_float_funcAddr;

            Log::print<PPC>("Storing float param '{}' offset {:08X} for action at {:08X}", paramNameOwned, destFloatPtr, actionPtr);
            paramList.push_back({ paramNameOwned, destFloatPtr });
        }
    }

    hCPU->instructionPointer = orig_GetStaticParam_float_funcAddr;
}

void CemuHooks::hook_FixLadder(PPCInterpreter_t* hCPU) {
    hCPU->instructionPointer = hCPU->sprNew.LR;

    auto input = VRManager::instance().XR->m_input.load();

    if (input.shared.in_game && s_isLadderClimbing == 0) {
        return;
    }

    if (input.inGame.move.currentState.y >= -0.05) {
        //Log::print<INFO>("PLAYER LADDER MODE UP {}", input.inGame.move.currentState.y);
        hCPU->gpr[3] = 4; // allows pressing A to jump upwards, regardless of camera orientation
    }
    else {
        //Log::print<INFO>("PLAYER LADDER MODE DOWN {}", input.inGame.move.currentState.y);
        hCPU->gpr[3] = 1; // allows sliding down when holding move stick downwards
    }
}


void CemuHooks::hook_VisualizeRayCastHits(PPCInterpreter_t* hCPU) {
    hCPU->instructionPointer = hCPU->sprNew.LR;

    if (!GetSettings().ShouldShowRaycastLines()) {
        return;
    }

    uint32_t rayCastResultPtr = hCPU->gpr[3];
    glm::fvec3 raycastHitPos = getMemory<BEVec3>(hCPU->gpr[4]).getLE();

    ksys::phys::RayCast rayCast = {};
    readMemory(rayCastResultPtr, &rayCast);

    glm::fvec3 rayStart = rayCast.from.getLE();
    glm::fvec3 rayEnd = rayCast.to.getLE();
    
    rayStart.y += 0.5f;
    rayEnd.y += 0.5f;
    raycastHitPos.y += 0.5f;

    DebugDraw::instance().Line(rayStart, raycastHitPos, IM_COL32(255, 0, 255, 255));
    DebugDraw::instance().Line(raycastHitPos, rayEnd, IM_COL32(128, 0, 128, 128));
}
