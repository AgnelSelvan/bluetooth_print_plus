#include "bluetooth_print_plus_plugin.h"

// Winsock2 must come before windows.h
#include <winsock2.h>

#include <windows.h>

#include <BluetoothAPIs.h>
#include <VersionHelpers.h>
#include <ws2bth.h>

#include <flutter/event_channel.h>
#include <flutter/event_stream_handler_functions.h>
#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>
#include <flutter/standard_method_codec.h>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Bthprops.lib")

namespace bluetooth_print_plus {

// Custom message to wake the platform thread and drain pending callbacks
static constexpr UINT WM_BPP_DISPATCH = WM_APP + 0x100;
static const wchar_t kDispatchWindowClass[] =
    L"BluetoothPrintPlusDispatchWindow";

// ---------- helpers ----------

static std::string BthAddrToString(BTH_ADDR addr) {
  char buf[18];
  sprintf_s(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
            static_cast<int>((addr >> 40) & 0xFF),
            static_cast<int>((addr >> 32) & 0xFF),
            static_cast<int>((addr >> 24) & 0xFF),
            static_cast<int>((addr >> 16) & 0xFF),
            static_cast<int>((addr >> 8) & 0xFF),
            static_cast<int>(addr & 0xFF));
  return std::string(buf);
}

static BTH_ADDR StringToBthAddr(const std::string &str) {
  unsigned int b[6] = {};
  sscanf_s(str.c_str(), "%02X:%02X:%02X:%02X:%02X:%02X", &b[0], &b[1], &b[2],
           &b[3], &b[4], &b[5]);
  BTH_ADDR addr = 0;
  for (int i = 0; i < 6; i++) {
    addr = (addr << 8) | static_cast<BTH_ADDR>(b[i]);
  }
  return addr;
}

static std::string WideToUtf8(const wchar_t *wide) {
  if (!wide || !*wide) return "";
  int size =
      WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
  if (size <= 0) return "";
  std::vector<char> buf(size);
  WideCharToMultiByte(CP_UTF8, 0, wide, -1, buf.data(), size, nullptr,
                      nullptr);
  return std::string(buf.data());
}

static bool IsBluetoothAvailable() {
  BLUETOOTH_FIND_RADIO_PARAMS params;
  params.dwSize = sizeof(params);
  HANDLE hRadio = nullptr;
  HBLUETOOTH_RADIO_FIND hFind = BluetoothFindFirstRadio(&params, &hRadio);
  if (hFind) {
    CloseHandle(hRadio);
    BluetoothFindRadioClose(hFind);
    return true;
  }
  return false;
}

// ----------------------------------------------------------------
// Registration
// ----------------------------------------------------------------

void BluetoothPrintPlusPlugin::RegisterWithRegistrar(
    flutter::PluginRegistrarWindows *registrar) {
  auto plugin = std::make_unique<BluetoothPrintPlusPlugin>(registrar);
  registrar->AddPlugin(std::move(plugin));
}

// ----------------------------------------------------------------
// Constructor / Destructor
// ----------------------------------------------------------------

BluetoothPrintPlusPlugin::BluetoothPrintPlusPlugin(
    flutter::PluginRegistrarWindows *registrar)
    : registrar_(registrar) {
  // Create a hidden message-only window on the platform thread.
  // PostMessage to this HWND from any thread delivers the message
  // on the platform thread's message loop – guaranteed by Windows.
  WNDCLASSEX wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = BluetoothPrintPlusPlugin::DispatchWndProc;
  wc.hInstance = GetModuleHandle(nullptr);
  wc.lpszClassName = kDispatchWindowClass;
  RegisterClassEx(&wc);

  dispatch_hwnd_ = CreateWindowEx(
      0, kDispatchWindowClass, L"", 0, 0, 0, 0, 0,
      HWND_MESSAGE,  // message-only window
      nullptr, wc.hInstance, nullptr);
  // Stash `this` so the static WndProc can reach back to the instance.
  SetWindowLongPtr(dispatch_hwnd_, GWLP_USERDATA,
                   reinterpret_cast<LONG_PTR>(this));

  // --- Main method channel ---
  method_channel_ =
      std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
          registrar->messenger(), "bluetooth_print_plus/methods",
          &flutter::StandardMethodCodec::GetInstance());

  method_channel_->SetMethodCallHandler(
      [this](const auto &call, auto result) {
        HandleMethodCall(call, std::move(result));
      });

  // --- Event channel (state) ---
  event_channel_ =
      std::make_unique<flutter::EventChannel<flutter::EncodableValue>>(
          registrar->messenger(), "bluetooth_print_plus/state",
          &flutter::StandardMethodCodec::GetInstance());

  auto handler = std::make_unique<
      flutter::StreamHandlerFunctions<flutter::EncodableValue>>(
      [this](const flutter::EncodableValue *arguments,
             std::unique_ptr<flutter::EventSink<flutter::EncodableValue>>
                 &&events) -> std::unique_ptr<flutter::StreamHandlerError<
          flutter::EncodableValue>> {
        std::lock_guard<std::mutex> lock(event_sink_mutex_);
        event_sink_ = std::move(events);
        return nullptr;
      },
      [this](const flutter::EncodableValue *arguments)
          -> std::unique_ptr<
              flutter::StreamHandlerError<flutter::EncodableValue>> {
        std::lock_guard<std::mutex> lock(event_sink_mutex_);
        event_sink_ = nullptr;
        return nullptr;
      });
  event_channel_->SetStreamHandler(std::move(handler));

  // --- TSC command channel ---
  tsc_channel_ =
      std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
          registrar->messenger(), "bluetooth_print_plus_tsc",
          &flutter::StandardMethodCodec::GetInstance());
  tsc_channel_->SetMethodCallHandler([this](const auto &call, auto result) {
    HandleTscMethodCall(call, std::move(result));
  });

  // --- CPCL command channel ---
  cpcl_channel_ =
      std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
          registrar->messenger(), "bluetooth_print_plus_cpcl",
          &flutter::StandardMethodCodec::GetInstance());
  cpcl_channel_->SetMethodCallHandler([this](const auto &call, auto result) {
    HandleCpclMethodCall(call, std::move(result));
  });

  // --- ESC command channel ---
  esc_channel_ =
      std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
          registrar->messenger(), "bluetooth_print_plus_esc",
          &flutter::StandardMethodCodec::GetInstance());
  esc_channel_->SetMethodCallHandler([this](const auto &call, auto result) {
    HandleEscMethodCall(call, std::move(result));
  });
}

BluetoothPrintPlusPlugin::~BluetoothPrintPlusPlugin() {
  StopScan();
  Disconnect();
  if (dispatch_hwnd_) {
    DestroyWindow(dispatch_hwnd_);
    dispatch_hwnd_ = nullptr;
  }
  UnregisterClass(kDispatchWindowClass, GetModuleHandle(nullptr));
  if (wsa_initialized_) {
    WSACleanup();
  }
}

// ----------------------------------------------------------------
// Helpers – buffer append
// ----------------------------------------------------------------

void BluetoothPrintPlusPlugin::AppendToBuffer(std::vector<uint8_t> &buffer,
                                              const std::string &str) {
  buffer.insert(buffer.end(), str.begin(), str.end());
}

void BluetoothPrintPlusPlugin::AppendToBuffer(std::vector<uint8_t> &buffer,
                                              const uint8_t *data,
                                              size_t len) {
  buffer.insert(buffer.end(), data, data + len);
}

// ----------------------------------------------------------------
// Platform-thread dispatch via hidden message-only window
// ----------------------------------------------------------------

LRESULT CALLBACK BluetoothPrintPlusPlugin::DispatchWndProc(
    HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_BPP_DISPATCH) {
    auto *self = reinterpret_cast<BluetoothPrintPlusPlugin *>(
        GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (self) {
      self->DrainPendingCallbacks();
    }
    return 0;
  }
  return DefWindowProc(hwnd, message, wparam, lparam);
}

void BluetoothPrintPlusPlugin::DrainPendingCallbacks() {
  std::queue<std::function<void()>> work;
  {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    std::swap(work, pending_callbacks_);
  }
  while (!work.empty()) {
    work.front()();
    work.pop();
  }
}

void BluetoothPrintPlusPlugin::RunOnPlatformThread(
    std::function<void()> fn) {
  {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    pending_callbacks_.push(std::move(fn));
  }
  // Post to the hidden message-only window; Windows guarantees delivery
  // on the thread that created the window (the platform thread).
  if (dispatch_hwnd_) {
    ::PostMessage(dispatch_hwnd_, WM_BPP_DISPATCH, 0, 0);
  }
}

// ----------------------------------------------------------------
// State / scan-result helpers (marshalled to platform thread)
// ----------------------------------------------------------------

void BluetoothPrintPlusPlugin::SendStateEvent(int state) {
  RunOnPlatformThread([this, state]() {
    std::lock_guard<std::mutex> lock(event_sink_mutex_);
    if (event_sink_) {
      event_sink_->Success(flutter::EncodableValue(state));
    }
  });
}

void BluetoothPrintPlusPlugin::SendScanResult(const std::string &name,
                                              const std::string &address,
                                              int type) {
  RunOnPlatformThread([this, name, address, type]() {
    flutter::EncodableMap map;
    map[flutter::EncodableValue("name")] = flutter::EncodableValue(name);
    map[flutter::EncodableValue("address")] =
        flutter::EncodableValue(address);
    map[flutter::EncodableValue("type")] = flutter::EncodableValue(type);
    method_channel_->InvokeMethod(
        "ScanResult",
        std::make_unique<flutter::EncodableValue>(
            flutter::EncodableValue(map)));
  });
}

void BluetoothPrintPlusPlugin::SendReceivedData(
    const std::vector<uint8_t> &data) {
  RunOnPlatformThread([this, data]() {
    method_channel_->InvokeMethod(
        "ReceivedData",
        std::make_unique<flutter::EncodableValue>(
            flutter::EncodableValue(data)));
  });
}

// ----------------------------------------------------------------
// Bluetooth operations
// ----------------------------------------------------------------

int BluetoothPrintPlusPlugin::GetBluetoothState() {
  return IsBluetoothAvailable() ? 0 : 1;  // 0 = on, 1 = off
}

void BluetoothPrintPlusPlugin::StartScan() {
  if (is_scanning_.load()) return;
  is_scanning_ = true;

  // Run discovery in a worker thread
  scan_thread_ = std::thread([this]() {
    BLUETOOTH_DEVICE_SEARCH_PARAMS searchParams;
    ZeroMemory(&searchParams, sizeof(searchParams));
    searchParams.dwSize = sizeof(searchParams);
    searchParams.fReturnAuthenticated = TRUE;
    searchParams.fReturnRemembered = TRUE;
    searchParams.fReturnUnknown = TRUE;
    searchParams.fReturnConnected = TRUE;
    searchParams.fIssueInquiry = TRUE;
    searchParams.cTimeoutMultiplier = 4;  // ~5 s inquiry
    searchParams.hRadio = nullptr;

    BLUETOOTH_DEVICE_INFO deviceInfo;
    ZeroMemory(&deviceInfo, sizeof(deviceInfo));
    deviceInfo.dwSize = sizeof(deviceInfo);

    HBLUETOOTH_DEVICE_FIND hFind =
        BluetoothFindFirstDevice(&searchParams, &deviceInfo);
    if (hFind) {
      do {
        if (!is_scanning_.load()) break;

        std::string name = WideToUtf8(deviceInfo.szName);
        std::string address = BthAddrToString(deviceInfo.Address.ullLong);
        int type = static_cast<int>(deviceInfo.ulClassofDevice);

        SendScanResult(name, address, type);

      } while (BluetoothFindNextDevice(hFind, &deviceInfo));
      BluetoothFindDeviceClose(hFind);
    }

    is_scanning_ = false;
  });
  scan_thread_.detach();
}

void BluetoothPrintPlusPlugin::StopScan() {
  is_scanning_ = false;
  // The scan thread checks the flag and will stop on its own.
}

void BluetoothPrintPlusPlugin::Connect(const std::string &address) {
  // Run connection in a worker thread
  std::thread([this, address]() {
    // Initialise Winsock if needed
    if (!wsa_initialized_) {
      WSADATA wsaData;
      if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        OutputDebugStringA("BPP: WSAStartup failed\n");
        SendStateEvent(3);  // disconnected
        return;
      }
      wsa_initialized_ = true;
    }

    BTH_ADDR btAddr = StringToBthAddr(address);

    // --- Step 1: Ensure the device is paired ---
    // Look up the device info and pair if needed.
    BLUETOOTH_DEVICE_INFO deviceInfo;
    ZeroMemory(&deviceInfo, sizeof(deviceInfo));
    deviceInfo.dwSize = sizeof(deviceInfo);
    deviceInfo.Address.ullLong = btAddr;

    // Find radio handle for pairing
    BLUETOOTH_FIND_RADIO_PARAMS radioParams;
    radioParams.dwSize = sizeof(radioParams);
    HANDLE hRadio = nullptr;
    HBLUETOOTH_RADIO_FIND hRadioFind =
        BluetoothFindFirstRadio(&radioParams, &hRadio);

    if (hRadioFind) {
      // Get device info to check authentication status
      DWORD result = BluetoothGetDeviceInfo(hRadio, &deviceInfo);
      if (result == ERROR_SUCCESS && !deviceInfo.fAuthenticated) {
        OutputDebugStringA("BPP: Device not paired, attempting pairing...\n");
        // Attempt pairing – NULL hwndParent uses system default UI
        DWORD authResult = BluetoothAuthenticateDeviceEx(
            nullptr, hRadio, &deviceInfo, nullptr, MITMProtectionNotRequired);
        if (authResult != ERROR_SUCCESS) {
          char msg[128];
          sprintf_s(msg, "BPP: Pairing failed with error %lu\n", authResult);
          OutputDebugStringA(msg);
          // Continue anyway – some devices work without explicit pairing
        } else {
          OutputDebugStringA("BPP: Pairing succeeded\n");
        }
      }
      BluetoothFindRadioClose(hRadioFind);
      CloseHandle(hRadio);
    }

    // --- Step 2: Try connecting via RFCOMM ---
    SOCKET sock = INVALID_SOCKET;
    bool connected = false;

    // Attempt A: SDP service lookup with SerialPortServiceClass_UUID
    sock = socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM);
    if (sock != INVALID_SOCKET) {
      SOCKADDR_BTH sockAddr;
      ZeroMemory(&sockAddr, sizeof(sockAddr));
      sockAddr.addressFamily = AF_BTH;
      sockAddr.btAddr = btAddr;
      sockAddr.serviceClassId = SerialPortServiceClass_UUID;
      sockAddr.port = BT_PORT_ANY;

      OutputDebugStringA("BPP: Trying connect via SPP service class...\n");
      if (::connect(sock, reinterpret_cast<SOCKADDR *>(&sockAddr),
                    sizeof(sockAddr)) == 0) {
        OutputDebugStringA("BPP: Connected via SPP service class\n");
        connected = true;
      } else {
        char msg[128];
        sprintf_s(msg, "BPP: SPP connect failed, WSA error %d\n",
                  WSAGetLastError());
        OutputDebugStringA(msg);
        closesocket(sock);
        sock = INVALID_SOCKET;
      }
    }

    // Attempt B: Try direct RFCOMM channel 1 (most common for printers)
    if (!connected) {
      sock = socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM);
      if (sock != INVALID_SOCKET) {
        SOCKADDR_BTH sockAddr;
        ZeroMemory(&sockAddr, sizeof(sockAddr));
        sockAddr.addressFamily = AF_BTH;
        sockAddr.btAddr = btAddr;
        sockAddr.serviceClassId = GUID_NULL;
        sockAddr.port = 1;

        OutputDebugStringA("BPP: Trying direct RFCOMM channel 1...\n");
        if (::connect(sock, reinterpret_cast<SOCKADDR *>(&sockAddr),
                      sizeof(sockAddr)) == 0) {
          OutputDebugStringA("BPP: Connected on RFCOMM channel 1\n");
          connected = true;
        } else {
          char msg[128];
          sprintf_s(msg, "BPP: Channel 1 connect failed, WSA error %d\n",
                    WSAGetLastError());
          OutputDebugStringA(msg);
          closesocket(sock);
          sock = INVALID_SOCKET;
        }
      }
    }

    // Attempt C: Try channels 2–5
    if (!connected) {
      for (int ch = 2; ch <= 5 && !connected; ++ch) {
        sock = socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM);
        if (sock == INVALID_SOCKET) continue;

        SOCKADDR_BTH sockAddr;
        ZeroMemory(&sockAddr, sizeof(sockAddr));
        sockAddr.addressFamily = AF_BTH;
        sockAddr.btAddr = btAddr;
        sockAddr.serviceClassId = GUID_NULL;
        sockAddr.port = ch;

        char msg[128];
        sprintf_s(msg, "BPP: Trying RFCOMM channel %d...\n", ch);
        OutputDebugStringA(msg);

        if (::connect(sock, reinterpret_cast<SOCKADDR *>(&sockAddr),
                      sizeof(sockAddr)) == 0) {
          sprintf_s(msg, "BPP: Connected on RFCOMM channel %d\n", ch);
          OutputDebugStringA(msg);
          connected = true;
        } else {
          closesocket(sock);
          sock = INVALID_SOCKET;
        }
      }
    }

    if (!connected) {
      OutputDebugStringA("BPP: All connection attempts failed\n");
      SendStateEvent(3);
      return;
    }

    {
      std::lock_guard<std::mutex> lock(socket_mutex_);
      bt_socket_ = sock;
      is_connected_ = true;
    }

    SendStateEvent(2);  // connected

    // Start a reader thread for incoming data
    read_thread_ = std::thread([this]() {
      char buffer[1024];
      while (is_connected_.load()) {
        SOCKET s;
        {
          std::lock_guard<std::mutex> lock(socket_mutex_);
          s = bt_socket_;
        }
        if (s == INVALID_SOCKET) break;

        int bytes = recv(s, buffer, sizeof(buffer), 0);
        if (bytes > 0) {
          std::vector<uint8_t> data(buffer, buffer + bytes);
          SendReceivedData(data);
        } else {
          // Connection lost
          break;
        }
      }
      // If we exit this loop, the connection is gone
      if (is_connected_.load()) {
        Disconnect();
      }
    });
    read_thread_.detach();
  }).detach();
}

void BluetoothPrintPlusPlugin::Disconnect() {
  {
    std::lock_guard<std::mutex> lock(socket_mutex_);
    is_connected_ = false;
    if (bt_socket_ != INVALID_SOCKET) {
      closesocket(bt_socket_);
      bt_socket_ = INVALID_SOCKET;
    }
  }
  SendStateEvent(3);  // disconnected
}

void BluetoothPrintPlusPlugin::Write(const std::vector<uint8_t> &data) {
  std::lock_guard<std::mutex> lock(socket_mutex_);
  if (bt_socket_ != INVALID_SOCKET && is_connected_.load()) {
    const char *ptr = reinterpret_cast<const char *>(data.data());
    int remaining = static_cast<int>(data.size());
    while (remaining > 0) {
      int sent = send(bt_socket_, ptr, remaining, 0);
      if (sent <= 0) break;
      ptr += sent;
      remaining -= sent;
    }
  }
}

// ----------------------------------------------------------------
// Main method channel handler
// ----------------------------------------------------------------

void BluetoothPrintPlusPlugin::HandleMethodCall(
    const flutter::MethodCall<flutter::EncodableValue> &method_call,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  const auto &method = method_call.method_name();

  if (method == "getPlatformVersion") {
    std::ostringstream version_stream;
    version_stream << "Windows ";
    if (IsWindows10OrGreater()) {
      version_stream << "10+";
    } else if (IsWindows8OrGreater()) {
      version_stream << "8";
    } else if (IsWindows7OrGreater()) {
      version_stream << "7";
    }
    result->Success(flutter::EncodableValue(version_stream.str()));

  } else if (method == "state") {
    int state = GetBluetoothState();
    result->Success(flutter::EncodableValue(state));

  } else if (method == "startScan") {
    StartScan();
    result->Success();

  } else if (method == "stopScan") {
    StopScan();
    result->Success();

  } else if (method == "connect") {
    const auto *args =
        std::get_if<flutter::EncodableMap>(method_call.arguments());
    if (args) {
      auto it = args->find(flutter::EncodableValue("address"));
      if (it != args->end()) {
        const auto *address = std::get_if<std::string>(&it->second);
        if (address) {
          Connect(*address);
          result->Success();
          return;
        }
      }
    }
    result->Error("INVALID_ARGUMENT", "Missing address");

  } else if (method == "disconnect") {
    Disconnect();
    result->Success();

  } else if (method == "write") {
    const auto *args =
        std::get_if<flutter::EncodableMap>(method_call.arguments());
    if (args) {
      auto it = args->find(flutter::EncodableValue("data"));
      if (it != args->end()) {
        const auto *data = std::get_if<std::vector<uint8_t>>(&it->second);
        if (data) {
          Write(*data);
          result->Success();
          return;
        }
      }
    }
    result->Error("INVALID_ARGUMENT", "Missing data");

  } else {
    result->NotImplemented();
  }
}

// ================================================================
// TSC command channel handler
// ================================================================

void BluetoothPrintPlusPlugin::HandleTscMethodCall(
    const flutter::MethodCall<flutter::EncodableValue> &method_call,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  const auto &method = method_call.method_name();
  const auto *args =
      std::get_if<flutter::EncodableMap>(method_call.arguments());

  auto getInt = [&](const char *key, int def = 0) -> int {
    if (!args) return def;
    auto it = args->find(flutter::EncodableValue(std::string(key)));
    if (it == args->end()) return def;
    if (auto *v = std::get_if<int32_t>(&it->second)) return *v;
    if (auto *v = std::get_if<int64_t>(&it->second))
      return static_cast<int>(*v);
    return def;
  };
  auto getStr = [&](const char *key,
                     const std::string &def = "") -> std::string {
    if (!args) return def;
    auto it = args->find(flutter::EncodableValue(std::string(key)));
    if (it == args->end()) return def;
    if (auto *v = std::get_if<std::string>(&it->second)) return *v;
    return def;
  };
  auto getBool = [&](const char *key, bool def = false) -> bool {
    if (!args) return def;
    auto it = args->find(flutter::EncodableValue(std::string(key)));
    if (it == args->end()) return def;
    if (auto *v = std::get_if<bool>(&it->second)) return *v;
    return def;
  };

  if (method == "cleanCommand") {
    tsc_buffer_.clear();
    result->Success();

  } else if (method == "getCommand") {
    result->Success(flutter::EncodableValue(tsc_buffer_));

  } else if (method == "selfTest") {
    AppendToBuffer(tsc_buffer_, "SELFTEST\r\n");
    result->Success();

  } else if (method == "cls") {
    AppendToBuffer(tsc_buffer_, "CLS\r\n");
    result->Success();

  } else if (method == "size") {
    int w = getInt("width");
    int h = getInt("height");
    std::ostringstream ss;
    ss << "SIZE " << w << " mm," << h << " mm\r\n";
    AppendToBuffer(tsc_buffer_, ss.str());
    result->Success();

  } else if (method == "gap") {
    int gap = getInt("gap");
    std::ostringstream ss;
    ss << "GAP " << gap << " mm,0\r\n";
    AppendToBuffer(tsc_buffer_, ss.str());
    result->Success();

  } else if (method == "speed") {
    int speed = getInt("speed");
    std::ostringstream ss;
    ss << "SPEED " << speed << "\r\n";
    AppendToBuffer(tsc_buffer_, ss.str());
    result->Success();

  } else if (method == "density") {
    int density = getInt("density");
    std::ostringstream ss;
    ss << "DENSITY " << density << "\r\n";
    AppendToBuffer(tsc_buffer_, ss.str());
    result->Success();

  } else if (method == "text") {
    int x = getInt("x");
    int y = getInt("y");
    int xm = getInt("xMulti", 1);
    int ym = getInt("yMulti", 1);
    int rot = getInt("rotation");
    std::string content = getStr("content");
    std::ostringstream ss;
    ss << "TEXT " << x << "," << y << ",\"0\",";
    ss << rot << "," << xm << "," << ym << ",";
    ss << "\"" << content << "\"\r\n";
    AppendToBuffer(tsc_buffer_, ss.str());
    result->Success();

  } else if (method == "qrCode") {
    int x = getInt("x");
    int y = getInt("y");
    int cw = getInt("cellWidth", 6);
    int rot = getInt("rotation");
    std::string content = getStr("content");
    std::ostringstream ss;
    ss << "QRCODE " << x << "," << y << ",L," << cw << ",A," << rot
       << ",\"" << content << "\"\r\n";
    AppendToBuffer(tsc_buffer_, ss.str());
    result->Success();

  } else if (method == "barCode") {
    int x = getInt("x");
    int y = getInt("y");
    std::string codeType = getStr("codeType", "128");
    int height = getInt("height", 100);
    bool readable = getBool("readable", true);
    int rot = getInt("rotation");
    int narrow = getInt("narrow", 2);
    int wide = getInt("wide", 4);
    std::string content = getStr("content");
    std::ostringstream ss;
    ss << "BARCODE " << x << "," << y << ",\"" << codeType << "\"," << height
       << "," << (readable ? 1 : 0) << "," << rot << "," << narrow << ","
       << wide << ",\"" << content << "\"\r\n";
    AppendToBuffer(tsc_buffer_, ss.str());
    result->Success();

  } else if (method == "bar") {
    int x = getInt("x");
    int y = getInt("y");
    int w = getInt("width");
    int h = getInt("height", 2);
    std::ostringstream ss;
    ss << "BAR " << x << "," << y << "," << w << "," << h << "\r\n";
    AppendToBuffer(tsc_buffer_, ss.str());
    result->Success();

  } else if (method == "box") {
    int x = getInt("x");
    int y = getInt("y");
    int ex = getInt("endX");
    int ey = getInt("endY");
    int lt = getInt("linThickness", 2);
    std::ostringstream ss;
    ss << "BOX " << x << "," << y << "," << ex << "," << ey << "," << lt
       << "\r\n";
    AppendToBuffer(tsc_buffer_, ss.str());
    result->Success();

  } else if (method == "image") {
    // Image command placeholder – raw image needs bitmap conversion
    // For now, store the image position; full bitmap→TSPL conversion is
    // printer-specific and normally done by the gprinter SDK.
    result->Success();

  } else if (method == "print") {
    int copies = getInt("copies", 1);
    std::ostringstream ss;
    ss << "PRINT " << copies << "\r\n";
    AppendToBuffer(tsc_buffer_, ss.str());
    result->Success();

  } else {
    result->NotImplemented();
  }
}

// ================================================================
// CPCL command channel handler
// ================================================================

void BluetoothPrintPlusPlugin::HandleCpclMethodCall(
    const flutter::MethodCall<flutter::EncodableValue> &method_call,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  const auto &method = method_call.method_name();
  const auto *args =
      std::get_if<flutter::EncodableMap>(method_call.arguments());

  auto getInt = [&](const char *key, int def = 0) -> int {
    if (!args) return def;
    auto it = args->find(flutter::EncodableValue(std::string(key)));
    if (it == args->end()) return def;
    if (auto *v = std::get_if<int32_t>(&it->second)) return *v;
    if (auto *v = std::get_if<int64_t>(&it->second))
      return static_cast<int>(*v);
    return def;
  };
  auto getStr = [&](const char *key,
                     const std::string &def = "") -> std::string {
    if (!args) return def;
    auto it = args->find(flutter::EncodableValue(std::string(key)));
    if (it == args->end()) return def;
    if (auto *v = std::get_if<std::string>(&it->second)) return *v;
    return def;
  };
  auto getBool = [&](const char *key, bool def = false) -> bool {
    if (!args) return def;
    auto it = args->find(flutter::EncodableValue(std::string(key)));
    if (it == args->end()) return def;
    if (auto *v = std::get_if<bool>(&it->second)) return *v;
    return def;
  };

  if (method == "cleanCommand") {
    cpcl_buffer_.clear();
    result->Success();

  } else if (method == "getCommand") {
    result->Success(flutter::EncodableValue(cpcl_buffer_));

  } else if (method == "size") {
    int w = getInt("width");
    int h = getInt("height");
    int copies = getInt("copies", 1);
    std::ostringstream ss;
    ss << "! 0 200 200 " << h << " " << copies << "\r\n";
    ss << "PAGE-WIDTH " << w << "\r\n";
    AppendToBuffer(cpcl_buffer_, ss.str());
    result->Success();

  } else if (method == "text") {
    int x = getInt("x");
    int y = getInt("y");
    int size = getInt("size");
    int xm = getInt("xMulti", 1);
    int ym = getInt("yMulti", 1);
    int rot = getInt("rotation");
    bool bold = getBool("bold");
    std::string content = getStr("content");
    std::string rotCmd = "T";
    if (rot == 90) rotCmd = "T90";
    else if (rot == 180) rotCmd = "T180";
    else if (rot == 270) rotCmd = "T270";
    if (bold) {
      std::ostringstream ss;
      ss << "SETBOLD 1\r\n";
      AppendToBuffer(cpcl_buffer_, ss.str());
    }
    if (xm > 1 || ym > 1) {
      std::ostringstream ss;
      ss << "SETMAG " << xm << " " << ym << "\r\n";
      AppendToBuffer(cpcl_buffer_, ss.str());
    }
    {
      std::ostringstream ss;
      ss << rotCmd << " " << size << " 0 " << x << " " << y << " "
         << content << "\r\n";
      AppendToBuffer(cpcl_buffer_, ss.str());
    }
    if (xm > 1 || ym > 1) {
      AppendToBuffer(cpcl_buffer_, "SETMAG 0 0\r\n");
    }
    if (bold) {
      AppendToBuffer(cpcl_buffer_, "SETBOLD 0\r\n");
    }
    result->Success();

  } else if (method == "qrCode") {
    int x = getInt("x");
    int y = getInt("y");
    int w = getInt("width", 6);
    std::string content = getStr("content");
    std::ostringstream ss;
    ss << "BARCODE QR " << x << " " << y << " M 2 U " << w << "\r\n";
    ss << "MA," << content << "\r\n";
    ss << "ENDQR\r\n";
    AppendToBuffer(cpcl_buffer_, ss.str());
    result->Success();

  } else if (method == "barCode") {
    int x = getInt("x");
    int y = getInt("y");
    int w = getInt("width", 4);
    int h = getInt("height", 100);
    bool vertical = getBool("vertical");
    std::string codeType = getStr("codeType", "128");
    std::string content = getStr("content");
    std::string cmd = vertical ? "VB" : "B";
    std::ostringstream ss;
    ss << cmd << " " << codeType << " " << w << " 1 " << h << " " << x
       << " " << y << " " << content << "\r\n";
    AppendToBuffer(cpcl_buffer_, ss.str());
    result->Success();

  } else if (method == "line") {
    int x = getInt("x");
    int y = getInt("y");
    int ex = getInt("endX");
    int ey = getInt("endY");
    int w = getInt("width", 2);
    std::ostringstream ss;
    ss << "LINE " << x << " " << y << " " << ex << " " << ey << " " << w
       << "\r\n";
    AppendToBuffer(cpcl_buffer_, ss.str());
    result->Success();

  } else if (method == "image") {
    // Image placeholder – full bitmap conversion is printer-specific
    result->Success();

  } else if (method == "print") {
    AppendToBuffer(cpcl_buffer_, "PRINT\r\n");
    result->Success();

  } else if (method == "form") {
    AppendToBuffer(cpcl_buffer_, "FORM\r\n");
    result->Success();

  } else {
    result->NotImplemented();
  }
}

// ================================================================
// ESC/POS command channel handler
// ================================================================

void BluetoothPrintPlusPlugin::HandleEscMethodCall(
    const flutter::MethodCall<flutter::EncodableValue> &method_call,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  const auto &method = method_call.method_name();
  const auto *args =
      std::get_if<flutter::EncodableMap>(method_call.arguments());

  auto getInt = [&](const char *key, int def = 0) -> int {
    if (!args) return def;
    auto it = args->find(flutter::EncodableValue(std::string(key)));
    if (it == args->end()) return def;
    if (auto *v = std::get_if<int32_t>(&it->second)) return *v;
    if (auto *v = std::get_if<int64_t>(&it->second))
      return static_cast<int>(*v);
    return def;
  };
  auto getStr = [&](const char *key,
                     const std::string &def = "") -> std::string {
    if (!args) return def;
    auto it = args->find(flutter::EncodableValue(std::string(key)));
    if (it == args->end()) return def;
    if (auto *v = std::get_if<std::string>(&it->second)) return *v;
    return def;
  };

  if (method == "cleanCommand") {
    esc_buffer_.clear();
    // ESC @ – Initialize printer
    uint8_t init[] = {0x1B, 0x40};
    AppendToBuffer(esc_buffer_, init, 2);
    result->Success();

  } else if (method == "getCommand") {
    result->Success(flutter::EncodableValue(esc_buffer_));

  } else if (method == "text") {
    std::string content = getStr("content");
    int alignment = getInt("alignment");
    int printMode = getInt("printMode");
    int size = getInt("size");

    // ESC a n – alignment (0=left,1=center,2=right)
    uint8_t align[] = {0x1B, 0x61, static_cast<uint8_t>(alignment)};
    AppendToBuffer(esc_buffer_, align, 3);

    // ESC ! n – print mode
    uint8_t mode[] = {0x1B, 0x21, static_cast<uint8_t>(printMode)};
    AppendToBuffer(esc_buffer_, mode, 3);

    // GS ! n – character size
    uint8_t charSize = static_cast<uint8_t>((size << 4) | size);
    uint8_t sizeCmd[] = {0x1D, 0x21, charSize};
    AppendToBuffer(esc_buffer_, sizeCmd, 3);

    // Text content
    AppendToBuffer(esc_buffer_, content);

    // Reset alignment & mode
    uint8_t resetAlign[] = {0x1B, 0x61, 0x00};
    AppendToBuffer(esc_buffer_, resetAlign, 3);
    uint8_t resetMode[] = {0x1B, 0x21, 0x00};
    AppendToBuffer(esc_buffer_, resetMode, 3);
    uint8_t resetSize[] = {0x1D, 0x21, 0x00};
    AppendToBuffer(esc_buffer_, resetSize, 3);

    result->Success();

  } else if (method == "newline") {
    uint8_t nl[] = {0x0A};
    AppendToBuffer(esc_buffer_, nl, 1);
    result->Success();

  } else if (method == "print") {
    int feedLines = getInt("feedLines", 4);
    // ESC d n – print and feed n lines
    uint8_t feed[] = {0x1B, 0x64, static_cast<uint8_t>(feedLines)};
    AppendToBuffer(esc_buffer_, feed, 3);
    result->Success();

  } else if (method == "code128") {
    std::string content = getStr("content");
    int width = getInt("width", 2);
    int height = getInt("height", 60);
    int alignment = getInt("alignment");
    int hri = getInt("hri", 2);

    // alignment
    uint8_t align[] = {0x1B, 0x61, static_cast<uint8_t>(alignment)};
    AppendToBuffer(esc_buffer_, align, 3);

    // GS w n – barcode width
    uint8_t bw[] = {0x1D, 0x77, static_cast<uint8_t>(width)};
    AppendToBuffer(esc_buffer_, bw, 3);

    // GS h n – barcode height
    uint8_t bh[] = {0x1D, 0x68, static_cast<uint8_t>(height)};
    AppendToBuffer(esc_buffer_, bh, 3);

    // GS H n – HRI position
    uint8_t hr[] = {0x1D, 0x48, static_cast<uint8_t>(hri)};
    AppendToBuffer(esc_buffer_, hr, 3);

    // GS k m n d1...dn – print barcode (Code128 = type 73)
    uint8_t header[] = {0x1D, 0x6B, 73,
                        static_cast<uint8_t>(content.size())};
    AppendToBuffer(esc_buffer_, header, 4);
    AppendToBuffer(esc_buffer_, content);

    // reset alignment
    uint8_t resetAlign[] = {0x1B, 0x61, 0x00};
    AppendToBuffer(esc_buffer_, resetAlign, 3);

    result->Success();

  } else if (method == "qrCode") {
    std::string content = getStr("content");
    int size = getInt("size", 3);
    int alignment = getInt("alignment");

    // alignment
    uint8_t align[] = {0x1B, 0x61, static_cast<uint8_t>(alignment)};
    AppendToBuffer(esc_buffer_, align, 3);

    // QR Code model: GS ( k  pL pH cn fn n
    // Select model 2
    uint8_t model[] = {0x1D, 0x28, 0x6B, 0x04, 0x00, 0x31, 0x41, 0x32,
                       0x00};
    AppendToBuffer(esc_buffer_, model, 9);

    // Set module size
    uint8_t modSize[] = {0x1D, 0x28, 0x6B, 0x03, 0x00, 0x31, 0x43,
                         static_cast<uint8_t>(size)};
    AppendToBuffer(esc_buffer_, modSize, 8);

    // Set error correction level (L)
    uint8_t ecl[] = {0x1D, 0x28, 0x6B, 0x03, 0x00, 0x31, 0x45, 0x30};
    AppendToBuffer(esc_buffer_, ecl, 8);

    // Store data
    int dataLen = static_cast<int>(content.size()) + 3;
    uint8_t storeHeader[] = {0x1D,
                             0x28,
                             0x6B,
                             static_cast<uint8_t>(dataLen & 0xFF),
                             static_cast<uint8_t>((dataLen >> 8) & 0xFF),
                             0x31,
                             0x50,
                             0x30};
    AppendToBuffer(esc_buffer_, storeHeader, 8);
    AppendToBuffer(esc_buffer_, content);

    // Print QR code
    uint8_t printQR[] = {0x1D, 0x28, 0x6B, 0x03, 0x00, 0x31, 0x51, 0x30};
    AppendToBuffer(esc_buffer_, printQR, 8);

    // reset alignment
    uint8_t resetAlign[] = {0x1B, 0x61, 0x00};
    AppendToBuffer(esc_buffer_, resetAlign, 3);

    result->Success();

  } else if (method == "image") {
    // Image placeholder – proper image→ESC/POS raster bitmap conversion is
    // complex and normally handled by the gprinter SDK. The image bytes would
    // need to be decoded, dithered, and converted to ESC * or GS v 0
    // commands. For now we accept the call without error.
    result->Success();

  } else if (method == "cutPaper") {
    // GS V m – partial cut
    uint8_t cut[] = {0x1D, 0x56, 0x01};
    AppendToBuffer(esc_buffer_, cut, 3);
    result->Success();

  } else if (method == "sound") {
    int number = getInt("number", 1);
    int time = getInt("time", 3);
    // ESC B n t – beep
    uint8_t beep[] = {0x1B, 0x42, static_cast<uint8_t>(number),
                      static_cast<uint8_t>(time)};
    AppendToBuffer(esc_buffer_, beep, 4);
    result->Success();

  } else {
    result->NotImplemented();
  }
}

}  // namespace bluetooth_print_plus
