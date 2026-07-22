// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <memory>
#include <QDockWidget>
#include "common/common_types.h"

class QTreeWidget;
class QTreeWidgetItem;
class QComboBox;
class EmuThread;

namespace Ui {
class ARMRegisters;
}

namespace Core {
class ARM_Interface;
class System;
} // namespace Core

class RegistersWidget : public QDockWidget {
    Q_OBJECT

public:
    explicit RegistersWidget(const Core::System& system, QWidget* parent = nullptr);
    ~RegistersWidget();

public slots:
    void OnDebugModeEntered();
    void OnDebugModeLeft();

    void OnEmulationStarting(EmuThread* emu_thread);
    void OnEmulationStopping();

private:
    void CreateCPSRChildren();
    void UpdateCPSRValues(u32 value);

    void CreateVFPSystemRegisterChildren();
    void UpdateVFPSystemRegisterValues(u32 fpscr, u32 fpexc);

    std::unique_ptr<Ui::ARMRegisters> cpu_regs_ui;
    const Core::System& system;
    QTreeWidget* tree;
    QComboBox* core_selector;

    QTreeWidgetItem* core_registers;
    QTreeWidgetItem* vfp_registers;
    QTreeWidgetItem* vfp_system_registers;
    QTreeWidgetItem* cpsr;
};
