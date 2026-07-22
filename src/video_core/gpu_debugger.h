// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <algorithm>
#include <atomic>
#include <functional>
#include <mutex>
#include <vector>
#include "common/logging/log.h"
#include "core/hle/service/gsp/gsp_command.h"

namespace VideoCore {

class GraphicsDebugger {
public:
    // Base class for all objects which need to be notified about GPU events
    class DebuggerObserver {
        friend class GraphicsDebugger;

    public:
        DebuggerObserver() = default;
        virtual ~DebuggerObserver() {
            if (observed) {
                observed->UnregisterObserver(this);
            }
        }

        /**
         * Called when a GX command has been processed and is ready for being
         * read via GraphicsDebugger::ReadGXCommandHistory.
         * @param total_command_count Total number of commands in the GX history
         * @note All methods in this class are called from the GSP thread
         */
        virtual void GXCommandProcessed(int total_command_count) {
            [[maybe_unused]] const Service::GSP::Command& cmd =
                observed->ReadGXCommandHistory(total_command_count - 1);
            LOG_TRACE(Debug_GPU, "Received command: id={:x}", (int)cmd.id.Value());
        }

    protected:
        const GraphicsDebugger* GetDebugger() const {
            return observed;
        }

    private:
        GraphicsDebugger* observed{};
    };

    void GXCommandProcessed(Service::GSP::Command& command_data) {
        if (observers.empty() && !capture_enabled) {
            return;
        }

        {
            std::lock_guard lock{history_mutex};
            gx_command_history.emplace_back(command_data);
        }
        ForEachObserver([this](DebuggerObserver* observer) {
            observer->GXCommandProcessed(static_cast<int>(this->GetGXCommandCount()));
        });
    }

    Service::GSP::Command ReadGXCommandHistory(int index) const {
        std::lock_guard lock{history_mutex};
        return gx_command_history[index];
    }

    void StartCapture() {
        std::lock_guard lock{history_mutex};
        gx_command_history.clear();
        capture_enabled = true;
    }

    void StopCapture() {
        capture_enabled = false;
    }

    void ClearCapture() {
        std::lock_guard lock{history_mutex};
        gx_command_history.clear();
    }

    bool IsCapturing() const {
        return capture_enabled;
    }

    u32 GetGXCommandCount() const {
        std::lock_guard lock{history_mutex};
        return static_cast<u32>(gx_command_history.size());
    }

    std::vector<Service::GSP::Command> ReadGXCommands(u32 start, u32 count, u32 command_id) const {
        std::vector<Service::GSP::Command> result;
        std::lock_guard lock{history_mutex};
        for (u32 i = std::min(start, static_cast<u32>(gx_command_history.size()));
             i < gx_command_history.size() && result.size() < count; ++i) {
            if (command_id == UINT32_MAX ||
                static_cast<u32>(gx_command_history[i].id.Value()) == command_id) {
                result.push_back(gx_command_history[i]);
            }
        }
        return result;
    }

    void RegisterObserver(DebuggerObserver* observer) {
        // TODO: Check for duplicates
        observers.push_back(observer);
        observer->observed = this;
    }

    void UnregisterObserver(DebuggerObserver* observer) {
        observers.erase(std::remove(observers.begin(), observers.end(), observer), observers.end());
        observer->observed = nullptr;
    }

private:
    void ForEachObserver(std::function<void(DebuggerObserver*)> func) {
        std::for_each(observers.begin(), observers.end(), func);
    }

    std::vector<DebuggerObserver*> observers;
    mutable std::mutex history_mutex;
    std::vector<Service::GSP::Command> gx_command_history;
    std::atomic_bool capture_enabled{};
};

} // namespace VideoCore
