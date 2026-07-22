// Copyright 2018 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <DockWidget.h>

class LLEServiceModulesWidget : public ads::CDockWidget {
    Q_OBJECT

public:
    explicit LLEServiceModulesWidget(QWidget* parent = nullptr);
    ~LLEServiceModulesWidget();
};
