#pragma once
#include <iostream>

void warn(const std::string& msg) // bool added to create function overload
{
    std::cerr << "\033[1;33mwarning:\033[0m " << msg << '\n';
}

void error(const std::string& msg)
{
    std::cerr << "\033[1;31merror:\033[0m " << msg << '\n';
    exit(1);
}