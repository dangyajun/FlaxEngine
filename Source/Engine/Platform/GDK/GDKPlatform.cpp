// Copyright (c) 2012-2021 Wojciech Figat. All rights reserved.

#if PLATFORM_GDK

#include "Engine/Platform/Platform.h"
#include "Engine/Platform/Window.h"
#include "Engine/Platform/CreateWindowSettings.h"
#include "Engine/Platform/WindowsManager.h"
#include "Engine/Platform/MemoryStats.h"
#include "Engine/Platform/BatteryInfo.h"
#include "Engine/Profiler/ProfilerCPU.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Collections/Array.h"
#include "Engine/Core/Collections/Dictionary.h"
#include "Engine/Platform/MessageBox.h"
#include "Engine/Engine/Engine.h"
#include "Engine/Utilities/StringConverter.h"
#include "Engine/Platform/Win32/IncludeWindowsHeaders.h"
#include "GDKInput.h"
#include <XGameRuntime.h>
#include <appnotify.h>

inline bool operator==(const APP_LOCAL_DEVICE_ID& l, const APP_LOCAL_DEVICE_ID& r)
{
    return Platform::MemoryCompare(&l, &r, sizeof(APP_LOCAL_DEVICE_ID)) == 0;
}

const Char* GDKPlatform::ApplicationWindowClass = TEXT("FlaxWindow");
void* GDKPlatform::Instance = nullptr;
Delegate<> GDKPlatform::OnSuspend;
Delegate<> GDKPlatform::OnResume;

struct User
{
    XUserHandle UserHandle;
    XUserLocalId LocalId;
    Array<APP_LOCAL_DEVICE_ID, FixedAllocation<32>> AssociatedDevices;

    void Set(XUserHandle userHandle, XUserLocalId localId)
    {
        UserHandle = userHandle;
        LocalId = localId;
        AssociatedDevices.Clear();
    }

    void Unset()
    {
        XUserCloseHandle(UserHandle);
        AssociatedDevices.Clear();
    }
};

namespace
{
    bool IsSuspended = false;
    HANDLE PlmSuspendComplete = nullptr;
    HANDLE PlmSignalResume = nullptr;
    PAPPSTATE_REGISTRATION Plm = {};
    String UserLocale, ComputerName;
    XTaskQueueHandle TaskQueue = nullptr;
    Array<User, FixedAllocation<8>> Users;
    XTaskQueueRegistrationToken UserChangeEventCallbackToken;
    XTaskQueueRegistrationToken UserDeviceAssociationChangedCallbackToken;
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_USER:
    {
        LOG(Info, "Suspending application");
        IsSuspended = true;
        GDKPlatform::OnSuspend();

        // Complete deferral
        SetEvent(PlmSuspendComplete);

        (void)WaitForSingleObject(PlmSignalResume, INFINITE);

        IsSuspended = false;
        LOG(Info, "Resuming application");
        GDKPlatform::OnResume();
        return DefWindowProc(hWnd, msg, wParam, lParam);
    }
    }

    // Find window to process that message
    if (hWnd != nullptr)
    {
        // Find window by handle
        const auto win = WindowsManager::GetByNativePtr(hWnd);
        if (win)
        {
            return static_cast<GDKWindow*>(win)->WndProc(msg, wParam, lParam);
        }
    }

    // Default
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

void CALLBACK UserChangeEventCallback(_In_opt_ void* context,_In_ XUserLocalId userLocalId, _In_ XUserChangeEvent event)
{
    LOG(Info, "User event (userLocalId: {0}, event: {1})", userLocalId.value, (int32)event);

    switch (event)
    {
    case XUserChangeEvent::SignedInAgain:
        break;
    case XUserChangeEvent::SignedOut:
        for (int32 i = 0; i < Users.Count(); i++)
        {
            if (Users[i].LocalId.value == userLocalId.value)
            {
                Users[i].Unset();
                Users.RemoveAt(i);
                break;
            }
        }
        break;
    default: ;
    }
}

String ToString(const APP_LOCAL_DEVICE_ID& deviceId)
{
    return String::Format(TEXT("{}-{}-{}-{}-{}-{}-{}-{}"),
                          *reinterpret_cast<const unsigned int*>(&deviceId.value[0]),
                          *reinterpret_cast<const unsigned int*>(&deviceId.value[4]),
                          *reinterpret_cast<const unsigned int*>(&deviceId.value[8]),
                          *reinterpret_cast<const unsigned int*>(&deviceId.value[12]),
                          *reinterpret_cast<const unsigned int*>(&deviceId.value[16]),
                          *reinterpret_cast<const unsigned int*>(&deviceId.value[20]),
                          *reinterpret_cast<const unsigned int*>(&deviceId.value[24]),
                          *reinterpret_cast<const unsigned int*>(&deviceId.value[28]));
}

User* FindUser(const XUserLocalId& id)
{
    User* result = nullptr;
    for (auto& user : Users)
    {
        if (user.LocalId.value == id.value)
        {
            result = &user;
            break;
        }
    }
    return result;
}

void CALLBACK UserDeviceAssociationChangedCallback(_In_opt_ void* context,_In_ const XUserDeviceAssociationChange* change)
{
    LOG(Info, "User device association event (deviceId: {0}, oldUser: {1}, newUser: {2})", ToString(change->deviceId), change->oldUser.value, change->newUser.value);

    User* oldGameUser = FindUser(change->oldUser);
    if (oldGameUser)
    {
        oldGameUser->AssociatedDevices.Remove(change->deviceId);
    }

    User* newGameUser = FindUser(change->newUser);
    if (newGameUser)
    {
        newGameUser->AssociatedDevices.Add(change->deviceId);
    }
}

void OnMainWindowCreated(HWND hWnd)
{
    // Register for app suspending/resuming events
    PlmSuspendComplete = CreateEventEx(nullptr, nullptr, 0, EVENT_MODIFY_STATE | SYNCHRONIZE);
    PlmSignalResume = CreateEventEx(nullptr, nullptr, 0, EVENT_MODIFY_STATE | SYNCHRONIZE);
    if (!PlmSuspendComplete || !PlmSignalResume)
        return;
    if (RegisterAppStateChangeNotification([](BOOLEAN quiesced, PVOID context)
    {
        if (quiesced)
        {
            ResetEvent(PlmSuspendComplete);
            ResetEvent(PlmSignalResume);

            // To ensure we use the main UI thread to process the notification, we self-post a message
            PostMessage(reinterpret_cast<HWND>(context), WM_USER, 0, 0);

            // To defer suspend, you must wait to exit this callback
            (void)WaitForSingleObject(PlmSuspendComplete, INFINITE);
        }
        else
        {
            SetEvent(PlmSignalResume);
        }
    }, hWnd, &Plm))
        return;
}

void CALLBACK AddUserComplete(_In_ XAsyncBlock* ab)
{
    XUserHandle userHandle;
    HRESULT hr = XUserAddResult(ab, &userHandle);
    if (SUCCEEDED(hr))
    {
        XUserLocalId userLocalId;
        XUserGetLocalId(userHandle, &userLocalId);

        XUserLocalId localId;
        XUserGetLocalId(userHandle, &localId);

        if (FindUser(localId) == nullptr)
        {
            // Add user
            auto& user = Users.AddOne();
            user.Set(userHandle, userLocalId);
        }
    }

    delete ab;
}

DialogResult MessageBox::Show(Window* parent, const StringView& text, const StringView& caption, MessageBoxButtons buttons, MessageBoxIcon icon)
{
    const char *firstButtonText, *secondButtonText, *thirdButtonText;
    XGameUiMessageDialogButton defaultButton, cancelButton = XGameUiMessageDialogButton::First;
    switch (buttons)
    {
    case MessageBoxButtons::AbortRetryIgnore:
        firstButtonText = "Abort";
        secondButtonText = "Retry";
        thirdButtonText = "Ignore";
        defaultButton = XGameUiMessageDialogButton::Second;
        cancelButton = XGameUiMessageDialogButton::Third;
        break;
    case MessageBoxButtons::OK:
        firstButtonText = "OK";
        secondButtonText = nullptr;
        thirdButtonText = nullptr;
        defaultButton = XGameUiMessageDialogButton::First;
        cancelButton = XGameUiMessageDialogButton::First;
        break;
    case MessageBoxButtons::OKCancel:
        firstButtonText = "OK";
        secondButtonText = "Cancel";
        thirdButtonText = nullptr;
        defaultButton = XGameUiMessageDialogButton::First;
        cancelButton = XGameUiMessageDialogButton::Second;
        break;
    case MessageBoxButtons::RetryCancel:
        firstButtonText = "Retry";
        secondButtonText = "Cancel";
        thirdButtonText = nullptr;
        defaultButton = XGameUiMessageDialogButton::First;
        cancelButton = XGameUiMessageDialogButton::Second;
        break;
    case MessageBoxButtons::YesNo:
        firstButtonText = "Yes";
        secondButtonText = "No";
        thirdButtonText = nullptr;
        defaultButton = XGameUiMessageDialogButton::First;
        cancelButton = XGameUiMessageDialogButton::Second;
        break;
    case MessageBoxButtons::YesNoCancel:
        firstButtonText = "Yes";
        secondButtonText = "No";
        thirdButtonText = "Cancel";
        defaultButton = XGameUiMessageDialogButton::First;
        cancelButton = XGameUiMessageDialogButton::Third;
        break;
    default:
        return DialogResult::None;
    }
    const StringAsANSI<> textAnsi(text.Get(), text.Length());
    const StringAsANSI<> captionAnsi(caption.Get(), caption.Length());

    // Show dialog and wait for the result
    DialogResult result = DialogResult::None;
    XTaskQueueHandle queue;
    if (FAILED(XTaskQueueCreate(XTaskQueueDispatchMode::ThreadPool, XTaskQueueDispatchMode::Immediate, &queue)))
    {
        return DialogResult::None;
    }
    XAsyncBlock* ab = new XAsyncBlock();
    Platform::MemoryClear(ab, sizeof(XAsyncBlock));
    ab->queue = queue;
    XGameUiMessageDialogButton button;
    if (SUCCEEDED(XGameUiShowMessageDialogAsync(ab, captionAnsi.Get(), textAnsi.Get(), firstButtonText, secondButtonText, thirdButtonText, defaultButton, cancelButton)) &&
        SUCCEEDED(XAsyncGetStatus(ab, true)) &&
        SUCCEEDED(XGameUiShowMessageDialogResult(ab, &button)))
    {
        switch (buttons)
        {
        case MessageBoxButtons::AbortRetryIgnore:
            result = button == XGameUiMessageDialogButton::First ? DialogResult::Abort : button == XGameUiMessageDialogButton::Second ? DialogResult::Retry : DialogResult::Ignore;
            break;
        case MessageBoxButtons::OK:
            result = DialogResult::OK;
            break;
        case MessageBoxButtons::OKCancel:
            result = button == XGameUiMessageDialogButton::First ? DialogResult::OK : DialogResult::Cancel;
            break;
        case MessageBoxButtons::RetryCancel:
            result = button == XGameUiMessageDialogButton::First ? DialogResult::Retry : DialogResult::Cancel;
            break;
        case MessageBoxButtons::YesNo:
            result = button == XGameUiMessageDialogButton::First ? DialogResult::Yes : DialogResult::No;
            break;
        case MessageBoxButtons::YesNoCancel:
            result = button == XGameUiMessageDialogButton::First ? DialogResult::Yes : button == XGameUiMessageDialogButton::Second ? DialogResult::No : DialogResult::Cancel;
            break;
        }
    }

    // Cleanup
    XTaskQueueTerminate(queue, true, nullptr, nullptr);
    delete ab;

    return result;
}

void GDKPlatform::PreInit(void* hInstance)
{
    ASSERT(hInstance);
    Instance = hInstance;

    // Initialize the Game Runtime APIs
    if (FAILED(XGameRuntimeInitialize()))
    {
        Error(TEXT("Game runtime initialization failed!"));
        exit(-1);
    }

    // Register window class
    WNDCLASSW windowsClass;
    Platform::MemoryClear(&windowsClass, sizeof(WNDCLASS));
    windowsClass.style = CS_HREDRAW | CS_VREDRAW;
    windowsClass.lpfnWndProc = WndProc;
    windowsClass.hInstance = (HINSTANCE)Instance;
    windowsClass.lpszClassName = ApplicationWindowClass;
    if (!RegisterClassW(&windowsClass))
    {
        Error(TEXT("Window class registration failed!"));
        exit(-1);
    }
}

bool GDKPlatform::IsRunningOnDevKit()
{
    const XSystemDeviceType deviceType = XSystemGetDeviceType();
    return deviceType == XSystemDeviceType::XboxOneXDevkit || deviceType == XSystemDeviceType::XboxScarlettDevkit;
}

bool GDKPlatform::Init()
{
    if (Win32Platform::Init())
        return true;

    DWORD tmp;
    Char buffer[256];

    // Get user locale string
    if (GetUserDefaultLocaleName(buffer, LOCALE_NAME_MAX_LENGTH))
    {
        UserLocale = String(buffer);
    }

    // Get computer name string
    if (GetComputerNameW(buffer, &tmp))
    {
        ComputerName = String(buffer);
    }

    // Create a task queue that will process in the background on system threads and fire callbacks on a thread we choose in a serialized order
    if (FAILED(XTaskQueueCreate(XTaskQueueDispatchMode::ThreadPool, XTaskQueueDispatchMode::Manual, &TaskQueue)))
        return true;

    // Register for any change events for user
    XUserRegisterForChangeEvent(
        TaskQueue,
        nullptr,
        &UserChangeEventCallback,
        &UserChangeEventCallbackToken
    );

    // Registers for any change to device association so that the application can keep up-to-date information about users and their associated devices
    XUserRegisterForDeviceAssociationChanged(
        TaskQueue,
        nullptr,
        &UserDeviceAssociationChangedCallback,
        &UserDeviceAssociationChangedCallbackToken
    );

    // Login the default user
    {
        auto asyncBlock = new XAsyncBlock();
        asyncBlock->queue = TaskQueue;
        asyncBlock->callback = AddUserComplete;
        HRESULT hr = XUserAddAsync(XUserAddOptions::AddDefaultUserAllowingUI, asyncBlock);
        if (FAILED(hr))
            delete asyncBlock;
    }

    GDKInput::Init();

    return false;
}

void GDKPlatform::BeforeRun()
{
    // Log system info
    const XSystemAnalyticsInfo analyticsInfo = XSystemGetAnalyticsInfo();
    LOG(Info, "{0}, {1}", StringAsUTF16<64>(analyticsInfo.family).Get(), StringAsUTF16<64>(analyticsInfo.form).Get());
    LOG(Info, "OS Version {0}.{1}.{2}.{3}", analyticsInfo.osVersion.major, analyticsInfo.osVersion.minor, analyticsInfo.osVersion.build, analyticsInfo.osVersion.revision);
}

void GDKPlatform::Tick()
{
    PROFILE_CPU_NAMED("Application.Tick");

    GDKInput::Update();

    // Handle callbacks in the main thread to ensure thread safety
    while (XTaskQueueDispatch(TaskQueue, XTaskQueuePort::Completion, 0))
    {
    }

    // Check to see if any messages are waiting in the queue
    MSG msg;
    while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
    {
        // Translate the message and dispatch it to WindowProc()
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
}

void GDKPlatform::BeforeExit()
{
}

void GDKPlatform::Exit()
{
    GDKInput::Exit();

    XUserUnregisterForDeviceAssociationChanged(UserDeviceAssociationChangedCallbackToken, false);
    XUserUnregisterForChangeEvent(UserChangeEventCallbackToken, false);
    if (TaskQueue)
    {
        XTaskQueueCloseHandle(TaskQueue);
    }

    UnregisterAppStateChangeNotification(Plm);

    CloseHandle(PlmSuspendComplete);
    CloseHandle(PlmSignalResume);

    UnregisterClassW(ApplicationWindowClass, nullptr);

    XGameRuntimeUninitialize();
}

#if !BUILD_RELEASE

void GDKPlatform::Log(const StringView& msg)
{
    OutputDebugStringW(msg.Get());
    OutputDebugStringW(TEXT(PLATFORM_LINE_TERMINATOR));
}

bool GDKPlatform::IsDebuggerPresent()
{
    return !!::IsDebuggerPresent();
}

#endif

BatteryInfo GDKPlatform::GetBatteryInfo()
{
    BatteryInfo info;
    info.State = BatteryInfo::States::Connected;
    return info;
}

int32 GDKPlatform::GetDpi()
{
    return 96;
}

String GDKPlatform::GetUserLocaleName()
{
    return UserLocale;
}

String GDKPlatform::GetComputerName()
{
    return ComputerName;
}

String GDKPlatform::GetUserName()
{
    return String::Empty;
}

bool GDKPlatform::GetHasFocus()
{
    return !IsSuspended;
}

bool GDKPlatform::CanOpenUrl(const StringView& url)
{
    return Users.HasItems();
}

void GDKPlatform::OpenUrl(const StringView& url)
{
    const StringAsANSI<> urlANSI(url.Get(), url.Length());
    XLaunchUri(Users[0].UserHandle, urlANSI.Get());
}

struct GetMonitorBoundsData
{
    Vector2 Pos;
    Rectangle Result;

    GetMonitorBoundsData(const Vector2& pos)
        : Pos(pos)
        , Result(Vector2::Zero, GDKPlatform::GetDesktopSize())
    {
    }
};

Rectangle GDKPlatform::GetMonitorBounds(const Vector2& screenPos)
{
    return Rectangle(Vector2::Zero, GetDesktopSize());
}

Vector2 GDKPlatform::GetDesktopSize()
{
    return Vector2(1920, 1080);
}

Rectangle GDKPlatform::GetVirtualDesktopBounds()
{
    return Rectangle(Vector2::Zero, GetDesktopSize());
}

void GDKPlatform::GetEnvironmentVariables(Dictionary<String, String>& result)
{
    const LPWCH environmentStr = GetEnvironmentStringsW();
    if (environmentStr)
    {
        LPWCH env = environmentStr;
        while (*env != '\0')
        {
            if (*env != '=')
            {
                WCHAR* str = wcschr(env, '=');
                ASSERT(str);
                result[String(env, (int32)(str - env))] = str + 1;
            }
            while (*env != '\0')
                env++;
            env++;
        }
        FreeEnvironmentStringsW(environmentStr);
    }
}

bool GDKPlatform::GetEnvironmentVariable(const String& name, String& value)
{
    const int32 bufferSize = 512;
    Char buffer[bufferSize];
    DWORD result = GetEnvironmentVariableW(*name, buffer, bufferSize);
    if (result == 0)
    {
        LOG_WIN32_LAST_ERROR;
        return true;
    }
    if (bufferSize < result)
    {
        value.ReserveSpace(result);
        result = GetEnvironmentVariableW(*name, *value, result);
        if (!result)
        {
            LOG_WIN32_LAST_ERROR;
            return FALSE;
        }
    }
    else
    {
        value.Set(buffer, result);
    }
    return false;
}

bool GDKPlatform::SetEnvironmentVariable(const String& name, const String& value)
{
    if (!SetEnvironmentVariableW(*name, *value))
    {
        LOG_WIN32_LAST_ERROR;
        return true;
    }
    return false;
}

Window* GDKPlatform::CreateWindow(const CreateWindowSettings& settings)
{
    return New<GDKWindow>(settings);
}

void* GDKPlatform::LoadLibrary(const Char* filename)
{
    ASSERT(filename);
    void* handle = ::LoadLibraryW(filename);
    if (!handle)
    {
        LOG(Warning, "Failed to load '{0}' (GetLastError={1})", filename, GetLastError());
    }
    return handle;
}

void GDKPlatform::FreeLibrary(void* handle)
{
    ::FreeLibrary((HMODULE)handle);
}

void* GDKPlatform::GetProcAddress(void* handle, const char* symbol)
{
    return (void*)::GetProcAddress((HMODULE)handle, symbol);
}

#endif