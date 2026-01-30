/*
 * The Wine project - Xinput Joystick Library
 * Copyright 2008 Andrew Fenn
 * Copyright 2018 Aric Stewart
 * Copyright 2021 Rémi Bernon for CodeWeavers
 * Copyright 2025 Pablo Labs
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <assert.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>

#include "windef.h"
#include "winbase.h"
#include "winerror.h"
#include "winuser.h"
#include "winreg.h"
#include "wingdi.h"
#include "winnls.h"
#include "winternl.h"
#include <winsock2.h>

#include "dbt.h"
#include "setupapi.h"
#include "devpkey.h"
#include "initguid.h"
#include "devguid.h"
#include "xinput.h"

#include "wine/debug.h"

DEFINE_GUID(GUID_DEVINTERFACE_WINEXINPUT,0x6c53d5fd,0x6480,0x440f,0xb6,0x18,0x47,0x67,0x50,0xc5,0xe1,0xa6);

/* Not defined in the headers, used only by XInputGetStateEx */
#define XINPUT_GAMEPAD_GUIDE 0x0400

#define CONTROLLER_BUFFER_SIZE 11
#define BUFFER_SIZE (CONTROLLER_BUFFER_SIZE * 4)
#define SERVER_PORT 7941

#define REQUEST_GET_CONNECTION 1
#define REQUEST_GET_CONTROLLER_STATE 2

#define A_BUTTON 0x01
#define B_BUTTON 0x02
#define X_BUTTON 0x04
#define Y_BUTTON 0x08
#define RB_BUTTON 0x10
#define LB_BUTTON 0x20
#define LS_BUTTON 0x40
#define RS_BUTTON 0x80

#define START_BUTTON 0x01
#define SELECT_BUTTON 0x02

WINE_DEFAULT_DEBUG_CHANNEL(xinput);

static bool xinput_is_ready = false;

struct xinput_controller
{
    CRITICAL_SECTION crit;
    XINPUT_CAPABILITIES caps;
    XINPUT_STATE state;
    XINPUT_GAMEPAD last_keystroke;
    XINPUT_VIBRATION vibration;
    HANDLE device;
    WCHAR device_path[MAX_PATH];
    BOOL enabled;
    BOOL connected;
    int id;
};

static struct xinput_controller controllers[XUSER_MAX_COUNT];
static CRITICAL_SECTION_DEBUG controller_critsect_debug[XUSER_MAX_COUNT] =
{
    {
        0, 0, &controllers[0].crit,
        { &controller_critsect_debug[0].ProcessLocksList, &controller_critsect_debug[0].ProcessLocksList },
          0, 0, { (DWORD_PTR)(__FILE__ ": controllers[0].crit") }
    },
    {
        0, 0, &controllers[1].crit,
        { &controller_critsect_debug[1].ProcessLocksList, &controller_critsect_debug[1].ProcessLocksList },
          0, 0, { (DWORD_PTR)(__FILE__ ": controllers[1].crit") }
    },
    {
        0, 0, &controllers[2].crit,
        { &controller_critsect_debug[2].ProcessLocksList, &controller_critsect_debug[2].ProcessLocksList },
          0, 0, { (DWORD_PTR)(__FILE__ ": controllers[2].crit") }
    },
    {
        0, 0, &controllers[3].crit,
        { &controller_critsect_debug[3].ProcessLocksList, &controller_critsect_debug[3].ProcessLocksList },
          0, 0, { (DWORD_PTR)(__FILE__ ": controllers[3].crit") }
    },
};

static struct xinput_controller controllers[XUSER_MAX_COUNT] =
{
    {{ &controller_critsect_debug[0], -1, 0, 0, 0, 0 }},
    {{ &controller_critsect_debug[1], -1, 0, 0, 0, 0 }},
    {{ &controller_critsect_debug[2], -1, 0, 0, 0, 0 }},
    {{ &controller_critsect_debug[3], -1, 0, 0, 0, 0 }},
};

static HMODULE xinput_instance;
static HANDLE start_event;

static BOOL controller_check_caps(struct xinput_controller *controller)
{
    XINPUT_CAPABILITIES *caps = &controller->caps;

    /* Count buttons */
    memset(caps, 0, sizeof(XINPUT_CAPABILITIES));

    caps->Gamepad.wButtons = 0xffff;
    caps->Gamepad.bLeftTrigger = (1u << (sizeof(caps->Gamepad.bLeftTrigger) + 1)) - 1;
    caps->Gamepad.bRightTrigger = (1u << (sizeof(caps->Gamepad.bRightTrigger) + 1)) - 1;
    caps->Gamepad.sThumbLX = (1u << (sizeof(caps->Gamepad.sThumbLX) + 1)) - 1;
    caps->Gamepad.sThumbLY = (1u << (sizeof(caps->Gamepad.sThumbLY) + 1)) - 1;
    caps->Gamepad.sThumbRX = (1u << (sizeof(caps->Gamepad.sThumbRX) + 1)) - 1;
    caps->Gamepad.sThumbRY = (1u << (sizeof(caps->Gamepad.sThumbRY) + 1)) - 1;

    caps->Type = XINPUT_DEVTYPE_GAMEPAD;
    caps->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;

    caps->Flags |= XINPUT_CAPS_FFB_SUPPORTED;
    caps->Vibration.wLeftMotorSpeed = 255;
    caps->Vibration.wRightMotorSpeed = 255;

    return TRUE;
}

static void controller_enable(struct xinput_controller *controller)
{
    if (controller->enabled) return;
    controller->enabled = TRUE;
}

static void controller_disable(struct xinput_controller *controller)
{
    if (!controller->enabled) return;
    controller->enabled = FALSE;
}

static void controller_connect(struct xinput_controller *controller)
{
    EnterCriticalSection(&controller->crit);

    memset(&controller->state, 0, sizeof(controller->state));

    controller_check_caps(controller);
    controller->connected = TRUE;
    controller->enabled = TRUE;

    LeaveCriticalSection(&controller->crit);
}

static void controller_disconnect(struct xinput_controller *controller)
{
    EnterCriticalSection(&controller->crit);

    controller->connected = FALSE;
    controller->enabled = FALSE;

    memset(&controller->caps, 0, sizeof(controller->caps));

    LeaveCriticalSection(&controller->crit);
}

static short scale_value(unsigned short input)
{
    return -32768 + ((input * 65535) / 255);
}

static void read_controller_state(struct xinput_controller *controller, char *buffer)
{
    /*
        Received Buffer Scheme

        buffer[0]: Type of operation
        buffer[1]: Controller Connected Status
        buffer[2]: A, B, X, Y, RB, LB, RS, LS Button State
        buffer[3]: Start, Select Button State
        buffer[4]: D-Pad Status
        buffer[5]: Left X Analog Status (0-255)
        buffer[6]: Left Y Analog Status (0-255)
        buffer[7]: Right X Analog Status (0-255)
        buffer[8]: Right Y Analog Status (0-255)
        buffer[9]: LT Status (0-255)
        buffer[10]: RT Status (0-255)
    */

    XINPUT_STATE *state = &controller->state;

    state->Gamepad.wButtons = 0;

    if (buffer[2] & A_BUTTON) state->Gamepad.wButtons |= XINPUT_GAMEPAD_A;
    if (buffer[2] & B_BUTTON) state->Gamepad.wButtons |= XINPUT_GAMEPAD_B;
    if (buffer[2] & X_BUTTON) state->Gamepad.wButtons |= XINPUT_GAMEPAD_X;
    if (buffer[2] & Y_BUTTON) state->Gamepad.wButtons |= XINPUT_GAMEPAD_Y;
    if (buffer[2] & LB_BUTTON) state->Gamepad.wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
    if (buffer[2] & RB_BUTTON) state->Gamepad.wButtons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
    if (buffer[2] & LS_BUTTON) state->Gamepad.wButtons |= XINPUT_GAMEPAD_LEFT_THUMB;
    if (buffer[2] & RS_BUTTON) state->Gamepad.wButtons |= XINPUT_GAMEPAD_RIGHT_THUMB;
    if (buffer[3] & SELECT_BUTTON) state->Gamepad.wButtons |= XINPUT_GAMEPAD_BACK;
    if (buffer[3] & START_BUTTON) state->Gamepad.wButtons |= XINPUT_GAMEPAD_START;

    /* 8 1 2
     * 7 0 3
     * 6 5 4 */

    switch (buffer[4])
    {
        case 0: break;
        case 1: state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_UP; break;
        case 2: state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_RIGHT; break;
        case 3: state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT; break;
        case 4: state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT | XINPUT_GAMEPAD_DPAD_DOWN; break;
        case 5: state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_DOWN; break;
        case 6: state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT; break;
        case 7: state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_LEFT; break;
        case 8: state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_UP; break;
    }

    state->Gamepad.sThumbLX = scale_value(buffer[5]);
    state->Gamepad.sThumbLY = scale_value(buffer[6]);
    state->Gamepad.sThumbRX = scale_value(buffer[7]);
    state->Gamepad.sThumbRY = scale_value(buffer[8]);

    state->Gamepad.bLeftTrigger = buffer[9];
    state->Gamepad.bRightTrigger = buffer[10];

    EnterCriticalSection(&controller->crit);

    state->dwPacketNumber++;

    LeaveCriticalSection(&controller->crit);
}

static DWORD WINAPI gamepad_update_thread_proc(void *param)
{
    WSADATA wsaData;
    SOCKET serverSocket;
    struct sockaddr_in serverAddr;
    struct timeval timeout;
    const char *env = getenv("MICEWINE_JOYSTICK_SERVER_IP");
    const char *serverIp = env ? env : "127.0.0.1";
    char buffer[BUFFER_SIZE];
    char controller0[CONTROLLER_BUFFER_SIZE];
    char controller1[CONTROLLER_BUFFER_SIZE];
    char controller2[CONTROLLER_BUFFER_SIZE];
    char controller3[CONTROLLER_BUFFER_SIZE];
    int serverAddrSize = sizeof(serverAddr);
    int res = -1;
    int timeoutCounter = 0;

    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        printf("Fail on start WSA.\n");
        return 1;
    }

    serverSocket = socket(AF_INET, SOCK_DGRAM, 0);
    if (serverSocket == INVALID_SOCKET) {
        printf("Error on Creating Socket: %d\n", WSAGetLastError());
        WSACleanup();
        return 1;
    }

    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = inet_addr(serverIp);
    serverAddr.sin_port = htons(SERVER_PORT);

    timeout.tv_sec = 2;
    timeout.tv_usec = 0;

    if (setsockopt(serverSocket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout)) < 0) {
        printf("Failure on defining timeout on server: %d\n", WSAGetLastError());
    }

    memset(buffer, 0, BUFFER_SIZE);
    buffer[0] = REQUEST_GET_CONNECTION;

    sendto(serverSocket, buffer, BUFFER_SIZE, 0, (struct sockaddr*)&serverAddr, serverAddrSize);

    while (TRUE)
    {
        memset(buffer, 0, BUFFER_SIZE);

        /*
            Sended Buffer Rumble Scheme

            buffer[1]: Rumble Left Motor for Controller 0
            buffer[2]: Rumble Right Motor for Controller 0
            buffer[3]: Rumble Left Motor for Controller 1
            buffer[4]: Rumble Right Motor for Controller 1
            buffer[5]: Rumble Left Motor for Controller 2
            buffer[6]: Rumble Right Motor for Controller 2
            buffer[7]: Rumble Left Motor for Controller 3
            buffer[8]: Rumble Right Motor for Controller 3
        */

        uint16_t wLeftMotorSpeedC0 = controllers[0].vibration.wLeftMotorSpeed;
        uint16_t wRightMotorSpeedC0 = controllers[0].vibration.wRightMotorSpeed;

        uint16_t wLeftMotorSpeedC1 = controllers[1].vibration.wLeftMotorSpeed;
        uint16_t wRightMotorSpeedC1 = controllers[1].vibration.wRightMotorSpeed;

        uint16_t wLeftMotorSpeedC2 = controllers[2].vibration.wLeftMotorSpeed;
        uint16_t wRightMotorSpeedC2 = controllers[2].vibration.wRightMotorSpeed;

        uint16_t wLeftMotorSpeedC3 = controllers[3].vibration.wLeftMotorSpeed;
        uint16_t wRightMotorSpeedC3 = controllers[3].vibration.wRightMotorSpeed;

        buffer[0] = REQUEST_GET_CONTROLLER_STATE;
        buffer[1] = (int) (wLeftMotorSpeedC0 / 257);
        buffer[2] = (int) (wRightMotorSpeedC0 / 257);
        buffer[3] = (int) (wLeftMotorSpeedC1 / 257);
        buffer[4] = (int) (wRightMotorSpeedC1 / 257);
        buffer[5] = (int) (wLeftMotorSpeedC2 / 257);
        buffer[6] = (int) (wRightMotorSpeedC2 / 257);
        buffer[7] = (int) (wLeftMotorSpeedC3 / 257);
        buffer[8] = (int) (wRightMotorSpeedC3 / 257);

        sendto(serverSocket, buffer, BUFFER_SIZE, 0, (struct sockaddr*)&serverAddr, serverAddrSize);

        memset(buffer, 0, BUFFER_SIZE);
        serverAddrSize = sizeof(serverAddr);
        res = recvfrom(serverSocket, buffer, BUFFER_SIZE, 0, (struct sockaddr*)&serverAddr, &serverAddrSize);

        if (res == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err == WSAETIMEDOUT) {
                timeoutCounter++;
                if (timeoutCounter > 60) {
                    TRACE("Failed to retrieve connection from input server...\n");
                    for (int i = 0; i < 4; i++) {
                        if (controllers[i].connected) {
                            controller_disconnect(&controllers[i]);
                        }
                    }
                    xinput_is_ready = true; // For not locking thread of xinput_get_state or XInputGetCapabilitiesEx
                    timeoutCounter = 0;
                    Sleep(250);
                    xinput_is_ready = false;
                }
            }
            continue;
        }
        timeoutCounter = 0;

        switch (buffer[0]) {
            case REQUEST_GET_CONNECTION:
                SetEvent(start_event);
                break;

            case REQUEST_GET_CONTROLLER_STATE:
                memcpy(controller0, buffer, CONTROLLER_BUFFER_SIZE);
                memcpy(controller1, buffer + CONTROLLER_BUFFER_SIZE, CONTROLLER_BUFFER_SIZE);
                memcpy(controller2, buffer + CONTROLLER_BUFFER_SIZE * 2, CONTROLLER_BUFFER_SIZE);
                memcpy(controller3, buffer + CONTROLLER_BUFFER_SIZE * 3, CONTROLLER_BUFFER_SIZE);

                char *controllers_ptrs[4] = { controller0, controller1, controller2, controller3 };

                for (int i = 0; i < 4; i++) {
                    if (controllers_ptrs[i][1]) {
                        if (!controllers[i].connected) {
                            controller_connect(&controllers[i]);
                        }
                        read_controller_state(&controllers[i], controllers_ptrs[i]);
                    } else {
                        if (controllers[i].connected) {
                            controller_disconnect(&controllers[i]);
                        }
                    }
                }
                xinput_is_ready = true;
                break;
        }
    }
}

static BOOL WINAPI start_update_thread_once( INIT_ONCE *once, void *param, void **context )
{
    HANDLE thread;

    start_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!start_event) ERR("failed to create start event, error %lu\n", GetLastError());

    thread = CreateThread(NULL, 0, gamepad_update_thread_proc, NULL, 0, NULL);
    if (!thread) ERR("failed to create update thread, error %lu\n", GetLastError());
    CloseHandle(thread);

    WaitForSingleObject(start_event, 2000);
    return TRUE;
}

static void start_update_thread(void)
{
    static INIT_ONCE init_once = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(&init_once, start_update_thread_once, NULL, NULL);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    TRACE("inst %p, reason %lu, reserved %p.\n", inst, reason, reserved);

    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        xinput_instance = inst;
        DisableThreadLibraryCalls(inst);
        break;
    }
    return TRUE;
}

void WINAPI DECLSPEC_HOTPATCH XInputEnable(BOOL enable)
{
    int index;

    TRACE("enable %d.\n", enable);

    /* Setting to false will stop messages from XInputSetState being sent
    to the controllers. Setting to true will send the last vibration
    value (sent to XInputSetState) to the controller and allow messages to
    be sent */
    start_update_thread();

    for (index = 0; index < XUSER_MAX_COUNT; index++)
    {
        if (enable) controller_enable(&controllers[index]);
        else controller_disable(&controllers[index]);
    }
}

DWORD WINAPI DECLSPEC_HOTPATCH XInputSetState(DWORD index, XINPUT_VIBRATION *vibration)
{
    TRACE("index %lu, vibration %p.\n", index, vibration);

    start_update_thread();

    if (index >= XUSER_MAX_COUNT) return ERROR_BAD_ARGUMENTS;

    controllers[index].vibration = *vibration;

    return ERROR_SUCCESS;
}

/* Some versions of SteamOverlayRenderer hot-patch XInputGetStateEx() and call
 * XInputGetState() in the hook, so we need a wrapper. */
static DWORD xinput_get_state(DWORD index, XINPUT_STATE *state)
{
    if (!state) return ERROR_BAD_ARGUMENTS;

    start_update_thread();

    if (index >= XUSER_MAX_COUNT) return ERROR_BAD_ARGUMENTS;

    while (!xinput_is_ready) Sleep(125);
    if (!controllers[index].connected) return ERROR_DEVICE_NOT_CONNECTED;

    *state = controllers[index].state;

    return ERROR_SUCCESS;
}

DWORD WINAPI DECLSPEC_HOTPATCH XInputGetState(DWORD index, XINPUT_STATE *state)
{
    DWORD ret;

    TRACE("index %lu, state %p.\n", index, state);

    ret = xinput_get_state(index, state);
    if (ret != ERROR_SUCCESS) return ret;

    /* The main difference between this and the Ex version is the media guide button */
    state->Gamepad.wButtons &= ~XINPUT_GAMEPAD_GUIDE;

    return ERROR_SUCCESS;
}

DWORD WINAPI DECLSPEC_HOTPATCH XInputGetStateEx(DWORD index, XINPUT_STATE *state)
{
    TRACE("index %lu, state %p.\n", index, state);

    return xinput_get_state(index, state);
}

static const int JS_STATE_OFF = 0;
static const int JS_STATE_LOW = 1;
static const int JS_STATE_HIGH = 2;

static int joystick_state(const SHORT value)
{
    if (value > 20000) return JS_STATE_HIGH;
    if (value < -20000) return JS_STATE_LOW;
    return JS_STATE_OFF;
}

static WORD js_vk_offs(const int x, const int y)
{
    if (y == JS_STATE_OFF)
    {
      /*if (x == JS_STATE_OFF) shouldn't get here */
        if (x == JS_STATE_LOW) return 3; /* LEFT */
      /*if (x == JS_STATE_HIGH)*/ return 2; /* RIGHT */
    }
    if (y == JS_STATE_HIGH)
    {
        if (x == JS_STATE_OFF) return 0; /* UP */
        if (x == JS_STATE_LOW) return 4; /* UPLEFT */
      /*if (x == JS_STATE_HIGH)*/ return 5; /* UPRIGHT */
    }
  /*if (y == JS_STATE_LOW)*/
    {
        if (x == JS_STATE_OFF) return 1; /* DOWN */
        if (x == JS_STATE_LOW) return 7; /* DOWNLEFT */
      /*if (x == JS_STATE_HIGH)*/ return 6; /* DOWNRIGHT */
    }
}

static DWORD check_joystick_keystroke(const DWORD index, XINPUT_KEYSTROKE *keystroke, const SHORT *cur_x,
                                      const SHORT *cur_y, SHORT *last_x, SHORT *last_y, const WORD base_vk)
{
    int cur_vk = 0, cur_x_st, cur_y_st;
    int last_vk = 0, last_x_st, last_y_st;

    cur_x_st = joystick_state(*cur_x);
    cur_y_st = joystick_state(*cur_y);
    if (cur_x_st || cur_y_st)
        cur_vk = base_vk + js_vk_offs(cur_x_st, cur_y_st);

    last_x_st = joystick_state(*last_x);
    last_y_st = joystick_state(*last_y);
    if (last_x_st || last_y_st)
        last_vk = base_vk + js_vk_offs(last_x_st, last_y_st);

    if (cur_vk != last_vk)
    {
        if (last_vk)
        {
            /* joystick was set, and now different. send a KEYUP event, and set
             * last pos to centered, so the appropriate KEYDOWN event will be
             * sent on the next call. */
            keystroke->VirtualKey = last_vk;
            keystroke->Unicode = 0; /* unused */
            keystroke->Flags = XINPUT_KEYSTROKE_KEYUP;
            keystroke->UserIndex = index;
            keystroke->HidCode = 0;

            *last_x = 0;
            *last_y = 0;

            return ERROR_SUCCESS;
        }

        /* joystick was unset, send KEYDOWN. */
        keystroke->VirtualKey = cur_vk;
        keystroke->Unicode = 0; /* unused */
        keystroke->Flags = XINPUT_KEYSTROKE_KEYDOWN;
        keystroke->UserIndex = index;
        keystroke->HidCode = 0;

        *last_x = *cur_x;
        *last_y = *cur_y;

        return ERROR_SUCCESS;
    }

    *last_x = *cur_x;
    *last_y = *cur_y;

    return ERROR_EMPTY;
}

static BOOL trigger_is_on(const BYTE value)
{
    return value > 30;
}

static DWORD check_for_keystroke(const DWORD index, XINPUT_KEYSTROKE *keystroke)
{
    struct xinput_controller *controller = &controllers[index];
    const XINPUT_GAMEPAD *cur;
    DWORD ret = ERROR_EMPTY;
    int i;

    static const struct
    {
        int mask;
        WORD vk;
    } buttons[] = {
        { XINPUT_GAMEPAD_DPAD_UP, VK_PAD_DPAD_UP },
        { XINPUT_GAMEPAD_DPAD_DOWN, VK_PAD_DPAD_DOWN },
        { XINPUT_GAMEPAD_DPAD_LEFT, VK_PAD_DPAD_LEFT },
        { XINPUT_GAMEPAD_DPAD_RIGHT, VK_PAD_DPAD_RIGHT },
        { XINPUT_GAMEPAD_START, VK_PAD_START },
        { XINPUT_GAMEPAD_BACK, VK_PAD_BACK },
        { XINPUT_GAMEPAD_LEFT_THUMB, VK_PAD_LTHUMB_PRESS },
        { XINPUT_GAMEPAD_RIGHT_THUMB, VK_PAD_RTHUMB_PRESS },
        { XINPUT_GAMEPAD_LEFT_SHOULDER, VK_PAD_LSHOULDER },
        { XINPUT_GAMEPAD_RIGHT_SHOULDER, VK_PAD_RSHOULDER },
        { XINPUT_GAMEPAD_A, VK_PAD_A },
        { XINPUT_GAMEPAD_B, VK_PAD_B },
        { XINPUT_GAMEPAD_X, VK_PAD_X },
        { XINPUT_GAMEPAD_Y, VK_PAD_Y },
        /* note: guide button does not send an event */
    };

    cur = &controller->state.Gamepad;

    /*** buttons ***/
    for (i = 0; i < ARRAY_SIZE(buttons); ++i)
    {
        if ((cur->wButtons & buttons[i].mask) ^ (controller->last_keystroke.wButtons & buttons[i].mask))
        {
            keystroke->VirtualKey = buttons[i].vk;
            keystroke->Unicode = 0; /* unused */
            if (cur->wButtons & buttons[i].mask)
            {
                keystroke->Flags = XINPUT_KEYSTROKE_KEYDOWN;
                controller->last_keystroke.wButtons |= buttons[i].mask;
            }
            else
            {
                keystroke->Flags = XINPUT_KEYSTROKE_KEYUP;
                controller->last_keystroke.wButtons &= ~buttons[i].mask;
            }
            keystroke->UserIndex = index;
            keystroke->HidCode = 0;
            ret = ERROR_SUCCESS;
            goto done;
        }
    }

    /*** triggers ***/
    if (trigger_is_on(cur->bLeftTrigger) ^ trigger_is_on(controller->last_keystroke.bLeftTrigger))
    {
        keystroke->VirtualKey = VK_PAD_LTRIGGER;
        keystroke->Unicode = 0; /* unused */
        keystroke->Flags = trigger_is_on(cur->bLeftTrigger) ? XINPUT_KEYSTROKE_KEYDOWN : XINPUT_KEYSTROKE_KEYUP;
        keystroke->UserIndex = index;
        keystroke->HidCode = 0;
        controller->last_keystroke.bLeftTrigger = cur->bLeftTrigger;
        ret = ERROR_SUCCESS;
        goto done;
    }

    if (trigger_is_on(cur->bRightTrigger) ^ trigger_is_on(controller->last_keystroke.bRightTrigger))
    {
        keystroke->VirtualKey = VK_PAD_RTRIGGER;
        keystroke->Unicode = 0; /* unused */
        keystroke->Flags = trigger_is_on(cur->bRightTrigger) ? XINPUT_KEYSTROKE_KEYDOWN : XINPUT_KEYSTROKE_KEYUP;
        keystroke->UserIndex = index;
        keystroke->HidCode = 0;
        controller->last_keystroke.bRightTrigger = cur->bRightTrigger;
        ret = ERROR_SUCCESS;
        goto done;
    }

    /*** joysticks ***/
    ret = check_joystick_keystroke(index, keystroke, &cur->sThumbLX, &cur->sThumbLY,
            &controller->last_keystroke.sThumbLX,
            &controller->last_keystroke.sThumbLY, VK_PAD_LTHUMB_UP);
    if (ret == ERROR_SUCCESS)
        goto done;

    ret = check_joystick_keystroke(index, keystroke, &cur->sThumbRX, &cur->sThumbRY,
            &controller->last_keystroke.sThumbRX,
            &controller->last_keystroke.sThumbRY, VK_PAD_RTHUMB_UP);
    if (ret == ERROR_SUCCESS)
        goto done;

done:
    return ret;
}

DWORD WINAPI DECLSPEC_HOTPATCH XInputGetKeystroke(DWORD index, DWORD reserved, PXINPUT_KEYSTROKE keystroke)
{
    TRACE("index %lu, reserved %lu, keystroke %p.\n", index, reserved, keystroke);

    if (index >= XUSER_MAX_COUNT && index != XUSER_INDEX_ANY) return ERROR_BAD_ARGUMENTS;

    if (index == XUSER_INDEX_ANY)
    {
        int i;
        for (i = 0; i < XUSER_MAX_COUNT; ++i)
            if (check_for_keystroke(i, keystroke) == ERROR_SUCCESS)
                return ERROR_SUCCESS;
        return ERROR_EMPTY;
    }

    return check_for_keystroke(index, keystroke);
}

DWORD WINAPI DECLSPEC_HOTPATCH XInputGetCapabilities(DWORD index, DWORD flags, XINPUT_CAPABILITIES *capabilities)
{
    XINPUT_CAPABILITIES_EX caps_ex;
    DWORD ret;

    ret = XInputGetCapabilitiesEx(1, index, flags, &caps_ex);

    if (!ret) *capabilities = caps_ex.Capabilities;

    return ret;
}

DWORD WINAPI DECLSPEC_HOTPATCH XInputGetDSoundAudioDeviceGuids(DWORD index, GUID *render_guid, GUID *capture_guid)
{
    FIXME("index %lu, render_guid %s, capture_guid %s stub!\n", index, debugstr_guid(render_guid),
          debugstr_guid(capture_guid));

    if (index >= XUSER_MAX_COUNT) return ERROR_BAD_ARGUMENTS;
    if (!controllers[index].device) return ERROR_DEVICE_NOT_CONNECTED;

    return ERROR_NOT_SUPPORTED;
}

DWORD WINAPI DECLSPEC_HOTPATCH XInputGetBatteryInformation(DWORD index, BYTE type, XINPUT_BATTERY_INFORMATION* battery)
{
    static int once;

    if (!once++) FIXME("index %lu, type %u, battery %p.\n", index, type, battery);

    if (index >= XUSER_MAX_COUNT) return ERROR_BAD_ARGUMENTS;
    if (!controllers[index].device) return ERROR_DEVICE_NOT_CONNECTED;

    return ERROR_NOT_SUPPORTED;
}

DWORD WINAPI DECLSPEC_HOTPATCH XInputGetCapabilitiesEx(DWORD unk, DWORD index, DWORD flags, XINPUT_CAPABILITIES_EX *caps)
{
    DWORD ret = ERROR_SUCCESS;

    TRACE("unk %lu, index %lu, flags %#lx, capabilities %p.\n", unk, index, flags, caps);

    start_update_thread();

    while (!xinput_is_ready) Sleep(125);

    if (!controllers[index].connected) {
        return ERROR_DEVICE_NOT_CONNECTED;
    }

    EnterCriticalSection(&controllers[index].crit);

    if (flags & XINPUT_FLAG_GAMEPAD && controllers[index].caps.SubType != XINPUT_DEVSUBTYPE_GAMEPAD)
        ret = ERROR_DEVICE_NOT_CONNECTED;
    else
    {
        caps->Capabilities = controllers[index].caps;
        caps->VendorId = 0x045E;
        caps->ProductId = 0x02A1;
    }

    LeaveCriticalSection(&controllers[index].crit);

    return ret;
}
