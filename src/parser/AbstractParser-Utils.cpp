// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2019 The MMapper Authors
// Author: Nils Schimmelmann <nschimme@gmail.com> (Jahara)

#include "AbstractParser-Utils.h"

#include <cctype>
#include <sstream>

#include "../configuration/configuration.h"

bool isOffline()
{
    return getConfig().general.mapMode == MapModeEnum::OFFLINE;
}

bool isOnline()
{
    return !isOffline();
}

const char *enabledString(const bool isEnabled)
{
    return isEnabled ? "enabled" : "disabled";
}

bool isValidPrefix(const char c)
{
    return std::ispunct(c);
}

std::string concatenate_unquoted(const Vector &input)
{
    std::ostringstream oss;
    bool first = true;
    for (const Value &val : input) {
        if (first)
            first = false;
        else
            oss << " ";
        oss << val.getString();
    }
    return oss.str();
};
