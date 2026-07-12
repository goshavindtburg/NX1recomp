/**
 * @file        rex/input/mnk/mouse_look.h
 * @brief       Shared raw mouse-look accumulator for recompiled game hooks.
 */
#pragma once

#include <cstdint>

namespace rex::input::mnk {

void AccumulateNativeMouseMovement(int32_t delta_x, int32_t delta_y);
bool ConsumeNativeMouseMovement(float* out_x, float* out_y);
void ClearNativeMouseMovement();

void SetMenuPointerVirtualPosition(float x, float y, bool valid);
bool QueryMenuPointerVirtualPosition(float* out_x, float* out_y, uint32_t* out_generation);
void ClearMenuPointerVirtualPosition();
void ReportMenuDirectHoverApplied(uint32_t pointer_generation);
void ClearMenuDirectHoverApplied();
bool QueryMenuDirectHoverApplied(uint32_t pointer_generation);

constexpr uint32_t kMenuMouseButtonLeft = 1u << 0;
constexpr uint32_t kMenuMouseButtonRight = 1u << 1;
constexpr uint32_t kMenuMouseButtonMiddle = 1u << 2;

void SetMenuMouseButtonState(uint32_t button_mask, bool down);
void ClearMenuMouseButtons();
bool ConsumeMenuMouseButtonSnapshot(uint32_t* out_down, uint32_t* out_pressed,
                                    uint32_t* out_released,
                                    uint32_t* out_generation);
void SetForceMenuMouseMode(bool active);
bool QueryForceMenuMouseMode();

void SetGameKeyCatcherMask(int32_t local_client_num, uint32_t catcher_mask);
void AddGameKeyCatcherMask(int32_t local_client_num, uint32_t catcher_mask);
void RemoveGameKeyCatcherMask(int32_t local_client_num, uint32_t catcher_mask);
void ClearGameKeyCatcherState();
bool QueryGameKeyCatcherMask(uint32_t* out_catcher_mask, uint32_t* out_generation);

}  // namespace rex::input::mnk
