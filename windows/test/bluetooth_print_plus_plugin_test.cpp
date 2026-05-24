#include <flutter/method_call.h>
#include <flutter/method_result_functions.h>
#include <flutter/standard_method_codec.h>
#include <gtest/gtest.h>
#include <windows.h>

#include <memory>
#include <string>
#include <variant>

// The full plugin requires a registrar, so unit tests for methods that need
// Bluetooth hardware are integration tests.  Keep this file as a build-
// verification placeholder.

namespace bluetooth_print_plus {
namespace test {

TEST(BluetoothPrintPlusPlugin, Placeholder) {
  // Build verification – the plugin compiles and links.
  EXPECT_TRUE(true);
}

}  // namespace test
}  // namespace bluetooth_print_plus
