#include "settings_policy.hpp"

#include <cassert>
#include <string>

using namespace musicremote::settings_policy;

int main() {
  assert(!valid_wifi_ssid(""));
  assert(valid_wifi_ssid(std::string(32, 's')));
  assert(!valid_wifi_ssid(std::string(33, 's')));
  assert(valid_wifi_password(""));
  assert(valid_wifi_password(std::string(64, 'p')));
  assert(!valid_wifi_password(std::string(65, 'p')));
  assert(!valid_server_host(""));
  assert(valid_server_host(std::string(127, 'h')));
  assert(!valid_server_host(std::string(128, 'h')));
  assert(!valid_text(std::string_view("abc\0def", 7), 10, false));

  assert(!valid_brightness(4));
  assert(valid_brightness(5));
  assert(valid_brightness(100));
  assert(!valid_brightness(101));

  assert(next_auto_blank_seconds(0) == 30);
  assert(next_auto_blank_seconds(30) == 60);
  assert(next_auto_blank_seconds(60) == 300);
  assert(next_auto_blank_seconds(300) == 900);
  assert(next_auto_blank_seconds(900) == 0);
  assert(next_auto_blank_seconds(123) == 0);
  assert(valid_auto_blank_seconds(0));
  assert(valid_auto_blank_seconds(900));
  assert(!valid_auto_blank_seconds(123));
}
