#pragma once
#include <string>
bool Auth_Check(std::string& msg);
bool Auth_IsAlive();   // ← 加这行
void CheckUpdate();

