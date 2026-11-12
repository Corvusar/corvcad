#pragma once
#include <string>
namespace corvcad {
// Actual general/thickness metadata in millimetres; 0 means not specified.
double board_thickness(const std::string &source);
}
