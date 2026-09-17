#pragma once

#include <mooncake_templates.h>

#include <cstdio>

// AppLauncherBase queues an app ID and opens it from onSleeping(), but it does
// not wait for the currently running app to finish its close callback.  This
// small production base class keeps that request deferred until the source is
// actually Sleeping.  It has no board/view dependencies and is therefore
// also used directly by the host lifecycle tests.
class DeferredAppLauncher : public mooncake::templates::AppLauncherBase {
public:
    bool requestAppAfterClose(int sourceId, int targetId)
    {
        if (currentState() != StateSleeping) {
            reject("launcher is not sleeping");
            return false;
        }
        if (getRunningAppId() != sourceId) {
            reject("source is not the running app");
            return false;
        }
        if (sourceId < 0 || targetId < 0 || sourceId == targetId || sourceId == getID() ||
            targetId == getID()) {
            reject("invalid source or target id");
            return false;
        }
        if (_deferred_source_id >= 0 || _going_to_open_app_id >= 0) {
            reject("another launch is already pending");
            return false;
        }
        if (!mooncake::GetMooncake().isAppExist(sourceId) ||
            !mooncake::GetMooncake().isAppExist(targetId)) {
            reject("source or target is not installed");
            return false;
        }
        if (mooncake::GetMooncake().getAppCurrentState(sourceId) != mooncake::AppAbility::StateRunning) {
            reject("source is not running");
            return false;
        }
        if (mooncake::GetMooncake().getAppCurrentState(targetId) != mooncake::AppAbility::StateSleeping) {
            reject("target is not sleeping");
            return false;
        }

        _deferred_source_id = sourceId;
        _deferred_target_id = targetId;
        std::printf("[Launcher] deferred launch accepted source=%d target=%d\n", sourceId, targetId);
        return true;
    }

    void onSleeping() override
    {
        if (_deferred_source_id < 0) {
            AppLauncherBase::onSleeping();
            return;
        }

        const int source_id = _deferred_source_id;
        const int target_id = _deferred_target_id;
        if (!mooncake::GetMooncake().isAppExist(source_id)) {
            discard("source disappeared");
            recoverFromMissingSource(source_id);
            return;
        }
        if (!mooncake::GetMooncake().isAppExist(target_id)) {
            discard("target disappeared");
            AppLauncherBase::onSleeping();
            return;
        }
        if (getRunningAppId() != source_id) {
            discard("running app changed");
            AppLauncherBase::onSleeping();
            return;
        }

        const auto source_state = mooncake::GetMooncake().getAppCurrentState(source_id);
        if (source_state == mooncake::AppAbility::StateRunning ||
            source_state == mooncake::AppAbility::StateGoClose) {
            // Keep the source as the running app and do not queue the target
            // until its onClose() has completed.
            return;
        }
        if (source_state == mooncake::AppAbility::StateNull) {
            discard("source became null");
            recoverFromMissingSource(source_id);
            return;
        }
        if (source_state != mooncake::AppAbility::StateSleeping) {
            discard("source reopened unexpectedly");
            AppLauncherBase::onSleeping();
            return;
        }
        if (mooncake::GetMooncake().getAppCurrentState(target_id) != mooncake::AppAbility::StateSleeping) {
            discard("target is no longer sleeping");
            AppLauncherBase::onSleeping();
            return;
        }

        if (!AppLauncherBase::openApp(target_id)) {
            discard("target could not be queued");
            AppLauncherBase::onSleeping();
            return;
        }

        _deferred_source_id = -1;
        _deferred_target_id = -1;
        std::printf("[Launcher] deferred launch dispatch source=%d target=%d\n", source_id, target_id);
        AppLauncherBase::onSleeping();
    }

private:
    void reject(const char *reason) const
    {
        std::printf("[Launcher] deferred launch rejected: %s\n", reason);
    }

    void discard(const char *reason)
    {
        std::printf("[Launcher] deferred launch discarded: %s\n", reason);
        _deferred_source_id = -1;
        _deferred_target_id = -1;
    }

    void recoverFromMissingSource(int source_id)
    {
        if (getRunningAppId() == source_id) {
            _running_app_id = -1;
            open();
            std::printf("[Launcher] recovered after source disappearance\n");
            return;
        }
        AppLauncherBase::onSleeping();
    }

    int _deferred_source_id = -1;
    int _deferred_target_id = -1;
};
