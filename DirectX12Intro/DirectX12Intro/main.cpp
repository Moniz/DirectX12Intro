#define WIN32_LEAN_AND_MEAN // Reduces the number of headers included with Windows.h
#include <Windows.h> // We're gonna be doing some windows stuff (Like creating windows!)
#include <shellapi.h> // For parsing command line arguments

// If min/max C library functions are defined, undefine them
// We want to use std::min/max and having both will cause conflicts
#if defined(min)
#undef min
#endif

#if defined(max)
#undef max
#endif

//We'll define our own CreateWindow function
#if defined(CreateWindow)
#undef CreateWindow
#endif

// Windows Runtime Library. Needed for ComPtr<>, since all DirectX 12 Objects are COM objects (interfaced)
// ComPtr gives shared pointer functionality for tracking lifetime of a COM object specifically
#include <wrl.h>
using namespace Microsoft::WRL;

// DirectX 12 specific headers.
#include <d3d12.h> //DX12 : Contains Direct3D 12 Objects (Device, CommandQueue, CommandList, etc.)
#include <dxgi1_6.h> // Direct X Graphics Infrastructure. Manage low-level tasks like adapters, devices, transitions, etc.
#include <d3dcompiler.h> // Provides functions to compile HLSL shaders at run-time 
// When using runtime compiled HLSL shaders using any of the D3DCompiler functions, do not forget to link against the d3dcompiler.lib library 
// and copy the D3dcompiler_47.dll to the same folder as the binary executable when distributing your project.
// A redistributable version of the D3dcompiler_47.dll file can be found in the Windows 10 SDK installation folder at C : \Program Files(x86)\Windows Kits\10\Redist\D3D\.
#include <DirectXMath.h> // Provides SIMD friendly C++ types and functions

// D3D12 extension library.
// D3D_FEATURE_LEVEL_12_2 define is missing?? Had to install DirectX 12 Agility SDK 
#include "d3dx12.h" // DirectX 12 extensions library that provides some helpful classes 
//(downloaded separately from (https://github.com/Microsoft/DirectX-Graphics-Samples/tree/master/Libraries/D3DX12))

// STL Headers
#include <algorithm>
#include <cassert>
#include <chrono>

// Helper functions
#include "Helpers.h"

// The number of swap chain back buffers
const uint8_t g_NumFrames = 3; 

// Use WARP (Software rasterizer) instead of the GPU
bool g_UseWarp = false;

// Initial Window Size
uint32_t g_ClientWidth = 1280;
uint32_t g_ClientHeight = 720;

// Set to true once DX12 objects (device, swap chain, etc.) have been initialized
bool g_IsInitialized = false;

// Windows and DirectX specific variables
// Window Handle
HWND g_hWnd; // handle to the OS Window for this application

// Window dimensions
RECT g_WindowRect;

// DirectX 12 Objects
ComPtr<ID3D12Device2> g_Device;
ComPtr<ID3D12CommandQueue> g_CommandQueue;
ComPtr<IDXGISwapChain4> g_SwapChain;
ComPtr<ID3D12Resource> g_BackBuffers[g_NumFrames]; // Buffer and Texture resources are referenced using ID3D12Resource
ComPtr<ID3D12GraphicsCommandList> g_CommandList; // One for each thread. GPU Commands go in here!
ComPtr<ID3D12CommandAllocator> g_CommandAllocators[g_NumFrames]; // Memory for GPU commands in the Cmd List. 
// Cannot re-use until all its commands are finished executing on the GPU. So we need 1 per render frame/back buffer
ComPtr<ID3D12DescriptorHeap> g_RTVDescriptorHeap; // Will holds all the descriptors/views, one for each back buffer
UINT g_RTVDescriptorSize; // Size of a discriptor in heap is vendor specific, so we need to find it at initialization and store it here
UINT g_CurrentBackBufferIndex; // Stores index of current back buffer on the swap chain

// Synchronization objects
ComPtr<ID3D12Fence> g_Fence; // The Fence Object!
uint64_t g_FenceValue = 0; //  64 bit unsigned is gigantic, and even if we signal 100 times a frame at 300 FPS, it'll take 20million years before we overflow
uint64_t g_FrameFenceValues[g_NumFrames] = {}; // a tracking fence value for each rendered frame that's "in-flight" on the Cmd Queue
HANDLE g_FenceEvent; // Will be called when a fence has reached a specific value

// Swap chain control variables
bool g_Vsync = true; // Wait for next vertical refresh? (caps frame rate to refresh rate of screen)
bool g_TearingSupported = false;

bool g_FullScreen;

// Forward Decleration
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM); // Windows message callback procedure

// Functions

void ParseCommandLineArguments()
{
	int argc;
	wchar_t** argv = ::CommandLineToArgvW(::GetCommandLine(), &argc);
    // :: is for system functions that are defined in global scope

	for (size_t i = 0; i < argc; ++i)
	{
        if (::wcscmp(argv[i], L"-w") == 0 || ::wcscmp(argv[i], L"--width") == 0)
        {
            g_ClientWidth = ::wcstol(argv[++i], nullptr, 10);
        }
        if (::wcscmp(argv[i], L"-h") == 0 || ::wcscmp(argv[i], L"--height") == 0)
        {
            g_ClientHeight = ::wcstol(argv[++i], nullptr, 10);
        }
        if (::wcscmp(argv[i], L"-warp") == 0 || ::wcscmp(argv[i], L"--warp") == 0)
        {
            g_UseWarp = true;
        }

        // Free memory allocated by CommandLineToArgvW
        ::LocalFree(argv);
    }
}

void EnableDebugLayer()
{
#if defined(_DEBUG)
    // Always enable the debug layer before doing anything DX12 related
    // so all possible errors generated while creating DX12 objects
    // are caught by the debug layer.
    // (!)Enabling after creating the ID3D12Device will cause runtime to remove the device(!)
    ComPtr<ID3D12Debug> debugInterface;
    ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&debugInterface)));
    // IID_PPV_ARGS retrieves an interface pointer based on the type of interface pointer used
    debugInterface->EnableDebugLayer();
#endif
}

// Register and Creating the Window

void RegisterWindowClass(HINSTANCE hInst, const wchar_t* windowClassName)
{
    // Register a window class for creating our render window with
    WNDCLASSEXW windowClass = {};

    windowClass.cbSize = sizeof(WNDCLASSEX);
    windowClass.style = CS_HREDRAW | CS_VREDRAW; // Redraw if width or height is adjusted
    windowClass.lpfnWndProc = &WndProc; // Window change callback
    windowClass.cbClsExtra = 0; // Extra bytes to allocate following the window-class structure
    windowClass.cbWndExtra = 0; // Extra bytes to allocate following the window instance
    windowClass.hInstance = hInst; // Handle to instance that contains window procedure for this class
    windowClass.hIcon = ::LoadIcon(hInst, NULL); // A handle to the class icon (seen in taskbar), null for default application icon
    windowClass.hCursor = ::LoadCursor(hInst, IDC_ARROW); // A handle to the class cursor. IDC_ARROW is default
    windowClass.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1); // A handle to background brush. Can be a system color + 1 cast to an HBRUSH
    windowClass.lpszMenuName = NULL; // char string to resource name of class menu. NULL = no default menu.
    windowClass.lpszClassName = windowClassName; // name of this window
    windowClass.hIconSm = ::LoadIcon(hInst, NULL); // a Small icon. If NULL will search using hIcon's resource for a small vers.

    static ATOM atom = ::RegisterClassExW(&windowClass);
    assert(atom > 0);
}

//Create an instance of an OS Window
HWND CreateWindow(const wchar_t* windowClassName, HINSTANCE hInst,
    const wchar_t* windowTitle, uint32_t width, uint32_t height)
{
    int screenWidth = ::GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = ::GetSystemMetrics(SM_CYSCREEN);

    RECT windowRect = { 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };

    ::AdjustWindowRect(&windowRect, WS_OVERLAPPEDWINDOW, false); // adjust window rect to be windowed-mode and no menu

    // Compute width/height after adjustment
    int windowWidth = windowRect.right - windowRect.left;
    int windowHeight = windowRect.bottom - windowRect.top;

    // Center window
    int windowX = std::max<int>(0, (screenWidth - windowWidth) / 2);
    int windowY = std::max<int>(0, (screenHeight - windowHeight) / 2);

    HWND hWnd = ::CreateWindowExW(
        NULL, // extended window style
        windowClassName, // window class name registered by the same module
        windowTitle, // the window name
        WS_OVERLAPPEDWINDOW, // window style, windowed in this case
        windowX,
        windowY,
        windowWidth,
        windowHeight,
        NULL, // handle to the parent or owner window
        NULL, // handle to the menu
        hInst, // handle to the instance of the module to be associated with the window
        nullptr // pointer to a value to be passed to the window through a bunch of other shit
    );

    // Window is created, but it's not being shown yet
    // Still needs to the device and cmd q to be created and initialized
}

// Query DirectX 12 Adapter

ComPtr<IDXGIAdapter4> GetAdapter(bool useWarp)
{
    ComPtr<IDXGIFactory4> dxgiFactory;
    UINT createFactoryFlags = 0;
#if defined(_DEBUG)
    createFactoryFlags = DXGI_CREATE_FACTORY_DEBUG; // enables errors during device creation
#endif
    ThrowIfFailed(CreateDXGIFactory2(createFactoryFlags, IID_PPV_ARGS(&dxgiFactory)));

    ComPtr<IDXGIAdapter1> dxgiAdapter1; // used to create warp adapter
    ComPtr<IDXGIAdapter4> dxgiAdapter4; // GetAdapter returns a 4

    if (useWarp)
    {
        ThrowIfFailed(dxgiFactory->EnumWarpAdapter(IID_PPV_ARGS(&dxgiAdapter1)));
        ThrowIfFailed(dxgiAdapter1.As(&dxgiAdapter4)); // cast COM object to correct type
    }
    else
    {

    }

}