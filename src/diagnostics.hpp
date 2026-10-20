#pragma once

#include "config.hpp"

#include <string>

void printUsage();
int runProbe(const Config& config);
int runLookup(const Config& config, const std::string& title, const std::string& artist);
