#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "winuser.h"
#include "winerror.h"
#include "winreg.h"
#include "winioctl.h"
#include <winsock2.h>

#include "ddk/hidclass.h"
#include "ddk/hidsdi.h"
#include "setupapi.h"
#include "devguid.h"
#include "dinput.h"
#include "xinput.h"
#include "setupapi.h"
#include "dbt.h"

#include "dinput_private.h"
#include "device_private.h"

#include "initguid.h"

#include "wine/debug.h"
#include "wine/hid.h"

#define CONTROLLER_BUFFER_SIZE 11
#define BUFFER_SIZE (CONTROLLER_BUFFER_SIZE * 4)
#define SERVER_PORT 7941

#define REQUEST_GET_CONNECTION 1
#define REQUEST_GET_CONTROLLER_STATE 3

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

WINE_DEFAULT_DEBUG_CHANNEL(dinput);

DEFINE_GUID( micewine_joystick_guid, 0xdeadbeef, 0x7734, 0x11d2, 0x8d, 0x4a, 0x23, 0x90, 0x3f, 0xb6, 0xbd, 0xf7 );
DEFINE_GUID (GUID_DEVINTERFACE_HID, 0x4D1E55B2L, 0xF16F, 0x11CF, 0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30);

static bool dinput_is_ready = false;

struct micewine_joystick
{
	struct dinput_device base;
    BOOL connected;
	WCHAR device_path[MAX_PATH];
    XINPUT_STATE state;
    int index;
};

static struct micewine_joystick connected_devices[XUSER_MAX_COUNT] = {0};

static inline struct micewine_joystick *impl_from_IDirectInputDevice8W( IDirectInputDevice8W *iface )
{
    return CONTAINING_RECORD( CONTAINING_RECORD( iface, struct dinput_device, IDirectInputDevice8W_iface ), struct micewine_joystick, base );
}

static inline BOOL is_exclusively_acquired( struct micewine_joystick *joystick )
{
    return joystick->base.status == STATUS_ACQUIRED && (joystick->base.dwCoopLevel & DISCL_EXCLUSIVE);
}

static unsigned short scale_value(unsigned short input)
{
    return (unsigned short)((input * 65535) / 255);
}

static short scale_value_signed(unsigned short input)
{
    return -32768 + ((input * 65535) / 255);
}

static int xinput_mask_map[] = {
    XINPUT_GAMEPAD_A,
    XINPUT_GAMEPAD_B,
    XINPUT_GAMEPAD_X,
    XINPUT_GAMEPAD_Y,
    XINPUT_GAMEPAD_LEFT_SHOULDER,
    XINPUT_GAMEPAD_RIGHT_SHOULDER,
    XINPUT_GAMEPAD_BACK,
    XINPUT_GAMEPAD_START,
    XINPUT_GAMEPAD_LEFT_THUMB,
    XINPUT_GAMEPAD_RIGHT_THUMB,
};

static void micewine_joystick_destroy( IDirectInputDevice8W *iface )
{
    struct micewine_joystick *impl = impl_from_IDirectInputDevice8W( iface );
    TRACE( "iface %p.\n", iface );
    CloseHandle( impl->base.read_event );
}

static HRESULT micewine_joystick_acquire( IDirectInputDevice8W *iface )
{
    struct micewine_joystick *impl = impl_from_IDirectInputDevice8W( iface );
    memcpy(&impl->base.device_format, &impl->base.user_format, sizeof(DIDATAFORMAT));
    IDirectInputDevice8_SendForceFeedbackCommand( iface, DISFFC_RESET );
    return DI_OK;
}

static HRESULT micewine_joystick_unacquire( IDirectInputDevice8W *iface )
{
    struct micewine_joystick *impl = impl_from_IDirectInputDevice8W( iface );
    if (!(impl->base.caps.dwFlags & DIDC_FORCEFEEDBACK)) return DI_OK;
    if (!is_exclusively_acquired( impl )) return DI_OK;
    IDirectInputDevice8_SendForceFeedbackCommand( iface, DISFFC_RESET );
    return DI_OK;
}

static HRESULT micewine_joystick_read( IDirectInputDevice8W *iface )
{
    struct micewine_joystick *impl = impl_from_IDirectInputDevice8W(iface);
    struct dinput_device *dinput = &impl->base;
    XINPUT_STATE state = impl->state;
    BYTE *user_state;

    if (!impl->connected) return DI_OK;
    if (dinput->status != STATUS_ACQUIRED) return DI_OK;

    EnterCriticalSection(&dinput->crit);

    user_state = dinput->device_state;
    memset(user_state, 0, dinput->user_format.dwDataSize);

    for (DWORD i = 0; i < dinput->user_format.dwNumObjs; i++) {
        DIOBJECTDATAFORMAT *obj = &dinput->user_format.rgodf[i];

        if (obj->dwType & DIDFT_AXIS) {
            LONG *ptr = (LONG *)(user_state + obj->dwOfs);

            switch (i) {
                case 17: // X Axis
                    *ptr = state.Gamepad.sThumbLX + 32767;
                    break;
                case 18: // Y Axis
                    *ptr = -state.Gamepad.sThumbLY + 32767;
                    break;
                case 19: // Z Axis
                    *ptr = scale_value(state.Gamepad.bRightTrigger / 2 - state.Gamepad.bLeftTrigger / 2 + 128);
                    break;
                case 20: // RX Axis
                    *ptr = state.Gamepad.sThumbRX + 32767;
                    break;
                case 21: // RY Axis
                    *ptr = -state.Gamepad.sThumbRY + 32767;
                    break;
            }
        } else if (obj->dwType & DIDFT_BUTTON) {
            BYTE *ptr = user_state + obj->dwOfs;
            DWORD index = DIDFT_GETINSTANCE(obj->dwType);

            *ptr = (state.Gamepad.wButtons & xinput_mask_map[index]) ? 0x80 : 0x0;
        } else if (obj->dwType & DIDFT_POV) {
            DWORD pov = 0xFFFF;
            DWORD *ptr = (DWORD *)(user_state + obj->dwOfs);

            if (i == 25) {
                BOOL up    = state.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_UP;
                BOOL down  = state.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_DOWN;
                BOOL left  = state.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_LEFT;
                BOOL right = state.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT;

                if (up && right) {
                    pov = 4500;
                } else if (right && down) {
                    pov = 13500;
                } else if (down && left) {
                    pov = 22500;
                } else if (left && up) {
                    pov = 31500;
                } else if (up) {
                    pov = 0;
                } else if (right) {
                    pov = 9000;
                } else if (down) {
                    pov = 18000;
                } else if (left) {
                    pov = 27000;
                }
            }

            *ptr = pov;
        }
    }

    LeaveCriticalSection(&dinput->crit);

    SetEvent(impl->base.hEvent);

    return DI_OK;
}

static HRESULT micewine_joystick_get_property( IDirectInputDevice8W *iface, DWORD property,
                                          DIPROPHEADER *header, const DIDEVICEOBJECTINSTANCEW *instance )
{
    struct micewine_joystick *impl = impl_from_IDirectInputDevice8W( iface );

    switch (property)
    {
        case (DWORD_PTR)DIPROP_PRODUCTNAME:
        {
            DIPROPSTRING *value = (DIPROPSTRING *)header;
            lstrcpynW( value->wsz, impl->base.instance.tszProductName, MAX_PATH );
            return DI_OK;
        }
        case (DWORD_PTR)DIPROP_INSTANCENAME:
        {
            DIPROPSTRING *value = (DIPROPSTRING *)header;
            lstrcpynW( value->wsz, impl->base.instance.tszInstanceName, MAX_PATH );
            return DI_OK;
        }
        case (DWORD_PTR)DIPROP_VIDPID:
        {
            DIPROPDWORD *value = (DIPROPDWORD *)header;
            value->dwData = MAKELONG( 0x045E, 0x028E );
            return DI_OK;
        }
        case (DWORD_PTR)DIPROP_JOYSTICKID:
        {
            DIPROPDWORD *value = (DIPROPDWORD *)header;
            value->dwData = impl->base.instance.guidInstance.Data3;
            return DI_OK;
        }
        case (DWORD_PTR)DIPROP_GUIDANDPATH:
        {
            DIPROPGUIDANDPATH *value = (DIPROPGUIDANDPATH *)header;
            value->guidClass = GUID_DEVCLASS_HIDCLASS;
            lstrcpynW( value->wszPath, impl->device_path, MAX_PATH );
            return DI_OK;
        }
        case (DWORD_PTR)DIPROP_FFLOAD:
        {
            DIPROPDWORD *value = (DIPROPDWORD *)header;
            if (!(impl->base.caps.dwFlags & DIDC_FORCEFEEDBACK)) return DIERR_UNSUPPORTED;
            if (!is_exclusively_acquired( impl )) return DIERR_NOTEXCLUSIVEACQUIRED;
            value->dwData = 0;
            return DI_OK;
        }
    }

    return DIERR_UNSUPPORTED;
}

static BOOL enum_objects(struct micewine_joystick *impl, const DIPROPHEADER *filter, DWORD flags,
                         enum_object_callback callback, void *data)
{
    static const struct {
        const WCHAR *name;
        DWORD type;
        DWORD offset;
        DWORD flags;
    } buttons[] = {
        {L"Button 0", DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(0), 24, 0},
        {L"Button 1", DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(1), 25, 0},
        {L"Button 2", DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(2), 26, 0},
        {L"Button 3", DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(3), 27, 0},
        {L"Button 4", DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(4), 28, 0},
        {L"Button 5", DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(5), 29, 0},
        {L"Button 6", DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(6), 30, 0},
        {L"Button 7", DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(7), 31, 0},
        {L"Button 8", DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(8), 32, 0},
        {L"Button 9", DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(9), 33, 0},
    };

    static const struct {
        const WCHAR *name;
        DWORD type;
        DWORD offset;
        DWORD flags;
    } axis[] = {
        {L"X Axis",      DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(0),  0,  DIDOI_ASPECTPOSITION},
        {L"Y Axis",      DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(1),  4,  DIDOI_ASPECTPOSITION},
        {L"Z Axis",      DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(2),  8,  DIDOI_ASPECTPOSITION},
        {L"X Rotation",  DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(3), 12,  DIDOI_ASPECTPOSITION},
        {L"Y Rotation",  DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(4), 16,  DIDOI_ASPECTPOSITION},
    };

    static const struct {
        const WCHAR *name;
        DWORD type;
        DWORD offset;
        DWORD flags;
    } povs[] = {
        {L"Hat Switch",  DIDFT_POV     | DIDFT_MAKEINSTANCE(0), 20,  0},
    };

    DIDEVICEOBJECTINSTANCEW instance = {.dwSize = sizeof(DIDEVICEOBJECTINSTANCEW)};
    instance.guidType = GUID_Joystick;
    instance.wUsagePage = 0x0001;
    instance.wUsage = 0x0005;
    instance.wReportId = 0;
    instance.wCollectionNumber = 0;
    instance.dwDimension = 0;
    instance.wExponent = 0;

    if (flags & DIDFT_BUTTON) {
        for (int i = 0; i < sizeof(buttons)/sizeof(buttons[0]); ++i)
        {
            instance.dwOfs = buttons[i].offset;
            instance.dwType = buttons[i].type;
            instance.dwFlags = buttons[i].flags;
            lstrcpynW(instance.tszName, buttons[i].name, MAX_PATH);

            if (!callback(&impl->base, i, NULL, &instance, data))
                return DIENUM_STOP;
        }
    }
    if (flags & DIDFT_AXIS) {
        for (int i = 0; i < sizeof(axis)/sizeof(axis[0]); ++i)
        {
            instance.dwOfs = axis[i].offset;
            instance.dwType = axis[i].type;
            instance.dwFlags = axis[i].flags;
            lstrcpynW(instance.tszName, axis[i].name, MAX_PATH);

            if (!callback(&impl->base, i, NULL, &instance, data))
                return DIENUM_STOP;
        }
    }
    if (flags & DIDFT_POV) {
        for (int i = 0; i < sizeof(povs)/sizeof(povs[0]); ++i)
        {
            instance.dwOfs = povs[i].offset;
            instance.dwType = povs[i].type;
            instance.dwFlags = povs[i].flags;
            lstrcpynW(instance.tszName, povs[i].name, MAX_PATH);

            if (!callback(&impl->base, i, NULL, &instance, data))
                return DIENUM_STOP;
        }
    }

    return DIENUM_STOP;
}

static HRESULT micewine_joystick_enum_objects( IDirectInputDevice8W *iface, const DIPROPHEADER *filter,
                                          DWORD flags, enum_object_callback callback, void *context )
{
    struct micewine_joystick *impl = impl_from_IDirectInputDevice8W( iface );
    return enum_objects(impl, filter, flags, callback, context );
}

static const struct dinput_device_vtbl micewine_joystick_vtbl =
{
    micewine_joystick_destroy,
    NULL,
    micewine_joystick_read,
    micewine_joystick_acquire,
    micewine_joystick_unacquire,
    micewine_joystick_enum_objects,
    micewine_joystick_get_property,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
};

static void read_controller_state(struct micewine_joystick *joystick, char *buffer)
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

    XINPUT_STATE *state = &joystick->state;

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

    state->Gamepad.sThumbLX = scale_value_signed(buffer[5]);
    state->Gamepad.sThumbLY = scale_value_signed(buffer[6]);
    state->Gamepad.sThumbRX = scale_value_signed(buffer[7]);
    state->Gamepad.sThumbRY = scale_value_signed(buffer[8]);

    state->Gamepad.bLeftTrigger = buffer[9];
    state->Gamepad.bRightTrigger = buffer[10];
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
    char *controllers_ptrs[4] = { controller0, controller1, controller2, controller3 };
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

    while (TRUE) {
        memset(buffer, 0, BUFFER_SIZE);

        buffer[0] = REQUEST_GET_CONTROLLER_STATE;

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
                        if (connected_devices[i].connected) {
                            connected_devices[i].connected = false;
                        }
                    }
                    dinput_is_ready = true; // For not locking thread of enum_devices or create_device
                    timeoutCounter = 0;
                    Sleep(250);
                    dinput_is_ready = false;
                }
            }
            continue;
        }
        timeoutCounter = 0;

        switch (buffer[0]) {
            case REQUEST_GET_CONTROLLER_STATE:
                memcpy(controller0, buffer, CONTROLLER_BUFFER_SIZE);
                memcpy(controller1, buffer + CONTROLLER_BUFFER_SIZE, CONTROLLER_BUFFER_SIZE);
                memcpy(controller2, buffer + CONTROLLER_BUFFER_SIZE * 2, CONTROLLER_BUFFER_SIZE);
                memcpy(controller3, buffer + CONTROLLER_BUFFER_SIZE * 3, CONTROLLER_BUFFER_SIZE);

                for (int i = 0; i < 4; i++) {
                    if (controllers_ptrs[i][1]) {
                        if (!connected_devices[i].connected) {
                            connected_devices[i].connected = true;
                        }
                        read_controller_state(&connected_devices[i], controllers_ptrs[i]);
                        SetEvent(connected_devices[i].base.read_event);
                    } else {
                        if (connected_devices[i].connected) {
                            connected_devices[i].connected = false;
                        }
                    }
                }
                dinput_is_ready = true;
                break;
        }
    }

    return 0;
}

static BOOL WINAPI start_update_thread_once(INIT_ONCE *once, void *param, void **context)
{
    HANDLE thread;
    thread = CreateThread(NULL, 0, gamepad_update_thread_proc, 0, 0, NULL);
    if (!thread) ERR("failed to create update thread, error %lu\n", GetLastError());

    CloseHandle(thread);
    return TRUE;
}

static void start_update_thread(void)
{
    static INIT_ONCE init_once = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(&init_once, start_update_thread_once, NULL, NULL);
}

HRESULT micewine_joystick_enum_device( DWORD type, DWORD flags, DIDEVICEINSTANCEW *instance, DWORD version, int index )
{
    start_update_thread();

    if (index > XUSER_MAX_COUNT) return DI_NOTATTACHED;
    while (!dinput_is_ready) Sleep(250);
	if (!connected_devices[index].connected) return DI_NOTATTACHED;

	memset(instance, 0, sizeof(DIDEVICEINSTANCEW));

    instance->dwSize = sizeof(DIDEVICEINSTANCEW);
    instance->guidInstance = GUID_Joystick;
    instance->guidProduct = dinput_pidvid_guid;
    instance->guidProduct.Data1 = MAKELONG( 0x045E, 0x028E );
    instance->guidFFDriver = GUID_NULL;
    instance->dwDevType = DI8DEVTYPE_GAMEPAD;
    instance->wUsagePage = 0x01;
    instance->wUsage = 0x05;

    swprintf(instance->tszInstanceName, MAX_PATH, L"MiceWine Virtual Controller %i", index);
    wcscpy(instance->tszProductName,  L"MiceWine Product");

	return DI_OK;
}

static int get_controllers_count(void)
{
    int count = 0;

    for (int i = 0; i < XUSER_MAX_COUNT; i++) 
    {
        if (connected_devices[i].connected) count++;
    }

    return count;
}

HRESULT micewine_joystick_create_device(struct dinput *dinput, const GUID *guid, IDirectInputDevice8W **out)
{
    static int index = 0;
    struct micewine_joystick *impl;

    if (index >= get_controllers_count()) index = 0;

    impl = &connected_devices[index];

    start_update_thread();

    while (!dinput_is_ready) Sleep(250);

    TRACE( "dinput %p, guid %s, out %p\n", dinput, debugstr_guid( guid ), out );

    *out = NULL;

    memset(impl, 0, sizeof(*impl));

    dinput_device_init(&impl->base, &micewine_joystick_vtbl, guid, dinput);
    impl->base.crit.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": micewine_joystick.base.crit");
    impl->base.dwCoopLevel = DISCL_NONEXCLUSIVE | DISCL_BACKGROUND;
    impl->base.read_event = CreateEventW(NULL, FALSE, FALSE, NULL);

    swprintf(impl->base.instance.tszInstanceName, MAX_PATH, L"MiceWine Virtual Controller %i", index);
    wcscpy(impl->base.instance.tszProductName,  L"MiceWine Product");
    swprintf(impl->device_path, MAX_PATH, L"\\\\?\\hid#vid_2563&pid_0575&ig_0%i#273&03006316632500007505000011010000.0&0&0&1#{4d1e55b2-f16f-11cf-88cb-001111000030}", index);

    impl->index = index;
    index++;
 
    impl->base.caps.dwDevType = DI8DEVTYPE_GAMEPAD | (DI8DEVTYPEGAMEPAD_STANDARD << 8);
    impl->base.caps.dwFlags = DIDC_FORCEFEEDBACK | DIDC_ATTACHED | DIDC_EMULATED;
    impl->base.caps.dwButtons = 10;
    impl->base.caps.dwAxes = 5;
    impl->base.caps.dwPOVs = 2;
    impl->base.caps.dwFFSamplePeriod = 1000000;
    impl->base.caps.dwFFMinTimeResolution = 1000000;

    impl->base.device_format.dwSize = sizeof(DIDATAFORMAT);
    impl->base.device_format.dwObjSize = sizeof(DIOBJECTDATAFORMAT);
    impl->base.device_format.dwFlags = DIDFT_ABSAXIS | DIDFT_BUTTON | DIDFT_POV;
    impl->base.device_format.dwNumObjs = impl->base.caps.dwAxes + impl->base.caps.dwButtons + impl->base.caps.dwPOVs;
    impl->base.device_format.dwDataSize = sizeof(DWORD) * impl->base.device_format.dwNumObjs;

    impl->base.device_format.rgodf = calloc(impl->base.device_format.dwNumObjs, sizeof(DIOBJECTDATAFORMAT));
    if (!impl->base.device_format.rgodf)
        goto failed;

    for (DWORD i = 0; i < impl->base.caps.dwAxes; i++)
    {
        impl->base.device_format.rgodf[i].dwType = DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(i);
        impl->base.device_format.rgodf[i].dwOfs = sizeof(DWORD) * i;
        impl->base.device_format.rgodf[i].pguid = NULL;
    }

    for (DWORD i = 0; i < impl->base.caps.dwButtons; i++)
    {
        DWORD idx = impl->base.caps.dwAxes + i;
        impl->base.device_format.rgodf[idx].dwType = DIDFT_BUTTON | DIDFT_MAKEINSTANCE(i);
        impl->base.device_format.rgodf[idx].dwOfs = sizeof(DWORD) * idx;
        impl->base.device_format.rgodf[idx].pguid = NULL;
    }

    for (DWORD i = 0; i < impl->base.caps.dwPOVs; i++)
    {
        DWORD idx = impl->base.caps.dwAxes + impl->base.caps.dwButtons + i;
        impl->base.device_format.rgodf[idx].dwType = DIDFT_POV | DIDFT_MAKEINSTANCE(i);
        impl->base.device_format.rgodf[idx].dwOfs = sizeof(DWORD) * idx;
        impl->base.device_format.rgodf[idx].pguid = NULL;
    }

    if (FAILED(dinput_device_init_device_format(&impl->base.IDirectInputDevice8W_iface))) goto failed;

    *out = &impl->base.IDirectInputDevice8W_iface;
    return DI_OK;

failed:
    IDirectInputDevice_Release(&impl->base.IDirectInputDevice8W_iface);
    return E_OUTOFMEMORY;
}
