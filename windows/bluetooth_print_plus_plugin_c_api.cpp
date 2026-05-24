#include "include/bluetooth_print_plus/bluetooth_print_plus_plugin_c_api.h"

#include <flutter/plugin_registrar_windows.h>

#include "bluetooth_print_plus_plugin.h"

void BluetoothPrintPlusPluginCApiRegisterWithRegistrar(
    FlutterDesktopPluginRegistrarRef registrar) {
  bluetooth_print_plus::BluetoothPrintPlusPlugin::RegisterWithRegistrar(
      flutter::PluginRegistrarManager::GetInstance()
          ->GetRegistrar<flutter::PluginRegistrarWindows>(registrar));
}
