#ifndef FLUTTER_PLUGIN_BLUETOOTH_PRINT_PLUS_PLUGIN_H_
#define FLUTTER_PLUGIN_BLUETOOTH_PRINT_PLUS_PLUGIN_H_

// winsock2.h must precede windows.h (pulled in by Flutter headers)
#include <winsock2.h>

#include <flutter/event_channel.h>
#include <flutter/event_stream_handler_functions.h>
#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>
#include <flutter/standard_method_codec.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace bluetooth_print_plus {

class BluetoothPrintPlusPlugin : public flutter::Plugin {
 public:
  static void RegisterWithRegistrar(flutter::PluginRegistrarWindows *registrar);

  explicit BluetoothPrintPlusPlugin(
      flutter::PluginRegistrarWindows *registrar);

  virtual ~BluetoothPrintPlusPlugin();

  // Disallow copy and assign.
  BluetoothPrintPlusPlugin(const BluetoothPrintPlusPlugin &) = delete;
  BluetoothPrintPlusPlugin &operator=(const BluetoothPrintPlusPlugin &) =
      delete;

 private:
  // Method call handlers
  void HandleMethodCall(
      const flutter::MethodCall<flutter::EncodableValue> &method_call,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);

  void HandleTscMethodCall(
      const flutter::MethodCall<flutter::EncodableValue> &method_call,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);

  void HandleCpclMethodCall(
      const flutter::MethodCall<flutter::EncodableValue> &method_call,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);

  void HandleEscMethodCall(
      const flutter::MethodCall<flutter::EncodableValue> &method_call,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);

  // Bluetooth operations
  void StartScan();
  void StopScan();
  void Connect(const std::string &address);
  void Disconnect();
  void Write(const std::vector<uint8_t> &data);
  int GetBluetoothState();

  // Helpers
  void SendStateEvent(int state);
  void SendScanResult(const std::string &name, const std::string &address,
                      int type);
  void SendReceivedData(const std::vector<uint8_t> &data);

  // Run a callable on the platform (main/UI) thread.
  void RunOnPlatformThread(std::function<void()> fn);
  // Process queued platform-thread callbacks.
  void DrainPendingCallbacks();
  // Window proc for the hidden message-only window.
  static LRESULT CALLBACK DispatchWndProc(HWND hwnd, UINT message,
                                          WPARAM wparam, LPARAM lparam);

  // Helper for command buffers
  void AppendToBuffer(std::vector<uint8_t> &buffer, const std::string &str);
  void AppendToBuffer(std::vector<uint8_t> &buffer, const uint8_t *data,
                      size_t len);

  // Plugin registrar
  flutter::PluginRegistrarWindows *registrar_;

  // Channels
  std::unique_ptr<flutter::MethodChannel<flutter::EncodableValue>>
      method_channel_;
  std::unique_ptr<flutter::EventChannel<flutter::EncodableValue>>
      event_channel_;
  std::unique_ptr<flutter::MethodChannel<flutter::EncodableValue>>
      tsc_channel_;
  std::unique_ptr<flutter::MethodChannel<flutter::EncodableValue>>
      cpcl_channel_;
  std::unique_ptr<flutter::MethodChannel<flutter::EncodableValue>>
      esc_channel_;

  // Event sink (guarded by event_sink_mutex_)
  std::unique_ptr<flutter::EventSink<flutter::EncodableValue>> event_sink_;
  std::mutex event_sink_mutex_;

  // Bluetooth state
  std::atomic<bool> is_scanning_{false};
  std::atomic<bool> is_connected_{false};
  SOCKET bt_socket_ = INVALID_SOCKET;
  std::thread scan_thread_;
  std::thread read_thread_;
  std::mutex socket_mutex_;
  bool wsa_initialized_ = false;

  // Command buffers
  std::vector<uint8_t> tsc_buffer_;
  std::vector<uint8_t> cpcl_buffer_;
  std::vector<uint8_t> esc_buffer_;

  // Platform-thread dispatch queue
  std::mutex callback_mutex_;
  std::queue<std::function<void()>> pending_callbacks_;
  HWND dispatch_hwnd_ = nullptr;
};

}  // namespace bluetooth_print_plus

#endif  // FLUTTER_PLUGIN_BLUETOOTH_PRINT_PLUS_PLUGIN_H_
