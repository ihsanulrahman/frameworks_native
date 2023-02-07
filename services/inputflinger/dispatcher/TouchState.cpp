/*
 * Copyright (C) 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <android-base/stringprintf.h>
#include <gui/WindowInfo.h>

#include "InputTarget.h"
#include "TouchState.h"

using namespace android::ftl::flag_operators;
using android::base::StringPrintf;
using android::gui::WindowInfo;
using android::gui::WindowInfoHandle;

namespace android::inputdispatcher {

void TouchState::reset() {
    *this = TouchState();
}

void TouchState::removeTouchedPointer(int32_t pointerId) {
    for (TouchedWindow& touchedWindow : windows) {
        touchedWindow.removeTouchingPointer(pointerId);
    }
}

void TouchState::removeTouchedPointerFromWindow(
        int32_t pointerId, const sp<android::gui::WindowInfoHandle>& windowHandle) {
    for (TouchedWindow& touchedWindow : windows) {
        if (touchedWindow.windowHandle == windowHandle) {
            touchedWindow.removeTouchingPointer(pointerId);
            return;
        }
    }
}

void TouchState::clearHoveringPointers() {
    for (TouchedWindow& touchedWindow : windows) {
        touchedWindow.clearHoveringPointers();
    }
}

void TouchState::clearWindowsWithoutPointers() {
    std::erase_if(windows, [](const TouchedWindow& w) {
        return w.pointerIds.none() && !w.hasHoveringPointers();
    });
}

void TouchState::addOrUpdateWindow(const sp<WindowInfoHandle>& windowHandle,
                                   ftl::Flags<InputTarget::Flags> targetFlags,
                                   std::bitset<MAX_POINTER_ID + 1> pointerIds,
                                   std::optional<nsecs_t> firstDownTimeInTarget) {
    for (TouchedWindow& touchedWindow : windows) {
        // We do not compare windows by token here because two windows that share the same token
        // may have a different transform
        if (touchedWindow.windowHandle == windowHandle) {
            touchedWindow.targetFlags |= targetFlags;
            if (targetFlags.test(InputTarget::Flags::DISPATCH_AS_SLIPPERY_EXIT)) {
                touchedWindow.targetFlags.clear(InputTarget::Flags::DISPATCH_AS_IS);
            }
            // For cases like hover enter/exit or DISPATCH_AS_OUTSIDE a touch window might not have
            // downTime set initially. Need to update existing window when an pointer is down for
            // the window.
            touchedWindow.pointerIds |= pointerIds;
            if (!touchedWindow.firstDownTimeInTarget.has_value()) {
                touchedWindow.firstDownTimeInTarget = firstDownTimeInTarget;
            }
            return;
        }
    }
    TouchedWindow touchedWindow;
    touchedWindow.windowHandle = windowHandle;
    touchedWindow.targetFlags = targetFlags;
    touchedWindow.pointerIds = pointerIds;
    touchedWindow.firstDownTimeInTarget = firstDownTimeInTarget;
    windows.push_back(touchedWindow);
}

void TouchState::addHoveringPointerToWindow(const sp<WindowInfoHandle>& windowHandle,
                                            int32_t hoveringDeviceId, int32_t hoveringPointerId) {
    for (TouchedWindow& touchedWindow : windows) {
        if (touchedWindow.windowHandle == windowHandle) {
            touchedWindow.addHoveringPointer(hoveringDeviceId, hoveringPointerId);
            return;
        }
    }

    TouchedWindow touchedWindow;
    touchedWindow.windowHandle = windowHandle;
    touchedWindow.addHoveringPointer(hoveringDeviceId, hoveringPointerId);
    windows.push_back(touchedWindow);
}

void TouchState::removeWindowByToken(const sp<IBinder>& token) {
    for (size_t i = 0; i < windows.size(); i++) {
        if (windows[i].windowHandle->getToken() == token) {
            windows.erase(windows.begin() + i);
            return;
        }
    }
}

void TouchState::filterNonAsIsTouchWindows() {
    for (size_t i = 0; i < windows.size();) {
        TouchedWindow& window = windows[i];
        if (window.targetFlags.any(InputTarget::Flags::DISPATCH_AS_IS |
                                   InputTarget::Flags::DISPATCH_AS_SLIPPERY_ENTER)) {
            window.targetFlags.clear(InputTarget::DISPATCH_MASK);
            window.targetFlags |= InputTarget::Flags::DISPATCH_AS_IS;
            i += 1;
        } else {
            windows.erase(windows.begin() + i);
        }
    }
}

void TouchState::cancelPointersForWindowsExcept(std::bitset<MAX_POINTER_ID + 1> pointerIds,
                                                const sp<IBinder>& token) {
    if (pointerIds.none()) return;
    std::for_each(windows.begin(), windows.end(), [&pointerIds, &token](TouchedWindow& w) {
        if (w.windowHandle->getToken() != token) {
            w.pointerIds &= ~pointerIds;
        }
    });
    std::erase_if(windows, [](const TouchedWindow& w) { return w.pointerIds.none(); });
}

/**
 * For any pointer that's being pilfered, remove it from all of the other windows that currently
 * aren't pilfering it. For example, if we determined that pointer 1 is going to both window A and
 * window B, but window A is currently pilfering pointer 1, then pointer 1 should not go to window
 * B.
 */
void TouchState::cancelPointersForNonPilferingWindows() {
    // First, find all pointers that are being pilfered, across all windows
    std::bitset<MAX_POINTER_ID + 1> allPilferedPointerIds;
    std::for_each(windows.begin(), windows.end(), [&allPilferedPointerIds](const TouchedWindow& w) {
        allPilferedPointerIds |= w.pilferedPointerIds;
    });

    // Optimization: most of the time, pilfering does not occur
    if (allPilferedPointerIds.none()) return;

    // Now, remove all pointers from every window that's being pilfered by other windows.
    // For example, if window A is pilfering pointer 1 (only), and window B is pilfering pointer 2
    // (only), the remove pointer 2 from window A and pointer 1 from window B. Usually, the set of
    // pilfered pointers will be disjoint across all windows, but there's no reason to cause that
    // limitation here.
    std::for_each(windows.begin(), windows.end(), [&allPilferedPointerIds](TouchedWindow& w) {
        std::bitset<MAX_POINTER_ID + 1> pilferedByOtherWindows =
                w.pilferedPointerIds ^ allPilferedPointerIds;
        w.pointerIds &= ~pilferedByOtherWindows;
    });
    std::erase_if(windows, [](const TouchedWindow& w) { return w.pointerIds.none(); });
}

sp<WindowInfoHandle> TouchState::getFirstForegroundWindowHandle() const {
    for (size_t i = 0; i < windows.size(); i++) {
        const TouchedWindow& window = windows[i];
        if (window.targetFlags.test(InputTarget::Flags::FOREGROUND)) {
            return window.windowHandle;
        }
    }
    return nullptr;
}

bool TouchState::isSlippery() const {
    // Must have exactly one foreground window.
    bool haveSlipperyForegroundWindow = false;
    for (const TouchedWindow& window : windows) {
        if (window.targetFlags.test(InputTarget::Flags::FOREGROUND)) {
            if (haveSlipperyForegroundWindow ||
                !window.windowHandle->getInfo()->inputConfig.test(
                        WindowInfo::InputConfig::SLIPPERY)) {
                return false;
            }
            haveSlipperyForegroundWindow = true;
        }
    }
    return haveSlipperyForegroundWindow;
}

sp<WindowInfoHandle> TouchState::getWallpaperWindow() const {
    for (size_t i = 0; i < windows.size(); i++) {
        const TouchedWindow& window = windows[i];
        if (window.windowHandle->getInfo()->inputConfig.test(
                    gui::WindowInfo::InputConfig::IS_WALLPAPER)) {
            return window.windowHandle;
        }
    }
    return nullptr;
}

const TouchedWindow& TouchState::getTouchedWindow(const sp<WindowInfoHandle>& windowHandle) const {
    auto it = std::find_if(windows.begin(), windows.end(),
                           [&](const TouchedWindow& w) { return w.windowHandle == windowHandle; });
    LOG_ALWAYS_FATAL_IF(it == windows.end(), "Could not find %s", windowHandle->getName().c_str());
    return *it;
}

bool TouchState::isDown() const {
    return std::any_of(windows.begin(), windows.end(),
                       [](const TouchedWindow& window) { return window.pointerIds.any(); });
}

std::set<sp<WindowInfoHandle>> TouchState::getWindowsWithHoveringPointer(int32_t hoveringDeviceId,
                                                                         int32_t pointerId) const {
    std::set<sp<WindowInfoHandle>> out;
    for (const TouchedWindow& window : windows) {
        if (window.hasHoveringPointer(hoveringDeviceId, pointerId)) {
            out.insert(window.windowHandle);
        }
    }
    return out;
}

void TouchState::removeHoveringPointer(int32_t hoveringDeviceId, int32_t hoveringPointerId) {
    for (TouchedWindow& window : windows) {
        window.removeHoveringPointer(hoveringDeviceId, hoveringPointerId);
    }
    std::erase_if(windows, [](const TouchedWindow& w) {
        return w.pointerIds.none() && !w.hasHoveringPointers();
    });
}

std::string TouchState::dump() const {
    std::string out;
    out += StringPrintf("deviceId=%d, source=%s\n", deviceId,
                        inputEventSourceToString(source).c_str());
    if (!windows.empty()) {
        out += "  Windows:\n";
        for (size_t i = 0; i < windows.size(); i++) {
            const TouchedWindow& touchedWindow = windows[i];
            out += StringPrintf("    %zu : ", i) + touchedWindow.dump();
        }
    } else {
        out += "  Windows: <none>\n";
    }
    return out;
}

} // namespace android::inputdispatcher
