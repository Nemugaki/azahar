// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <QApplication>
#include <QComboBox>
#include <QLabel>
#include <QTreeWidgetItem>
#include "citra_qt/debugger/dock_workspace.h"
#include "citra_qt/debugger/registers.h"
#include "citra_qt/util/util.h"
#include "core/arm/arm_interface.h"
#include "core/core.h"
#include "ui_registers.h"

RegistersWidget::RegistersWidget(Core::System& system_, QWidget* parent)
    : ads::CDockWidget(tr("ARM Registers"), parent),
      cpu_regs_ui(std::make_unique<Ui::ARMRegisters>()), system{system_} {
    cpu_regs_ui->setupUi(this);

    tree = cpu_regs_ui->treeWidget;
    core_selector = new QComboBox(this);
    cpu_regs_ui->verticalLayout->insertWidget(0, core_selector);
    capture_status = new QLabel(tr("Pause the CPU to capture registers"), this);
    capture_status->setAccessibleName(tr("Register capture status"));
    cpu_regs_ui->verticalLayout->insertWidget(1, capture_status);
    connect(core_selector, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this] { RefreshRegisters(false); });
    tree->addTopLevelItem(core_registers = new QTreeWidgetItem(QStringList(tr("Registers"))));
    tree->addTopLevelItem(vfp_registers = new QTreeWidgetItem(QStringList(tr("VFP Registers"))));
    tree->addTopLevelItem(vfp_system_registers =
                              new QTreeWidgetItem(QStringList(tr("VFP System Registers"))));
    tree->addTopLevelItem(cpsr = new QTreeWidgetItem(QStringList(QStringLiteral("CPSR"))));

    for (int i = 0; i < 16; ++i) {
        QTreeWidgetItem* child = new QTreeWidgetItem(QStringList(QStringLiteral("R[%1]").arg(i)));
        core_registers->addChild(child);
    }

    for (int i = 0; i < 32; ++i) {
        QTreeWidgetItem* child = new QTreeWidgetItem(QStringList(QStringLiteral("S[%1]").arg(i)));
        vfp_registers->addChild(child);
    }

    QFont font = GetMonospaceFont();

    CreateCPSRChildren();
    CreateVFPSystemRegisterChildren();

    // Set Registers to display in monospace font
    for (int i = 0; i < core_registers->childCount(); ++i)
        core_registers->child(i)->setFont(1, font);

    for (int i = 0; i < vfp_registers->childCount(); ++i)
        vfp_registers->child(i)->setFont(1, font);

    for (int i = 0; i < vfp_system_registers->childCount(); ++i) {
        vfp_system_registers->child(i)->setFont(1, font);
        for (int x = 0; x < vfp_system_registers->child(i)->childCount(); ++x) {
            vfp_system_registers->child(i)->child(x)->setFont(1, font);
        }
    }
    // Set CSPR to display in monospace font
    cpsr->setFont(1, font);
    for (int i = 0; i < cpsr->childCount(); ++i) {
        cpsr->child(i)->setFont(1, font);
        for (int x = 0; x < cpsr->child(i)->childCount(); ++x) {
            cpsr->child(i)->child(x)->setFont(1, font);
        }
    }
    Debugger::SetDockAvailable(this, false);
}

RegistersWidget::~RegistersWidget() = default;

void RegistersWidget::OnDebugModeEntered() {
    if (!Debugger::IsDockActive(this)) {
        return;
    }
    RefreshRegisters(true);
}

void RegistersWidget::RefreshRegisters(bool capture) {
    if (!system.IsPoweredOn()) {
        return;
    }

    if (capture) {
        previous_capture_id = current_capture_id;
        current_capture_id = system.CreateDebugCapture().id;
        capture_status->setText(
            current_capture_id
                ? tr("Capture %1; changed values show their previous value").arg(current_capture_id)
                : tr("CPU state is not paused consistently enough to capture"));
    }

    if (!current_capture_id) {
        return;
    }

    const u32 core = static_cast<u32>(std::max(core_selector->currentIndex(), 0));
    if (core >= system.GetNumCores()) {
        return;
    }
    const auto snapshot =
        current_capture_id ? system.GetDebugCaptureCore(current_capture_id, core) : std::nullopt;
    const auto current = snapshot.value_or(system.GetCore(core).GetRegisterSnapshot());
    const auto previous =
        previous_capture_id ? system.GetDebugCaptureCore(previous_capture_id, core) : std::nullopt;
    const auto show_change = [](QTreeWidgetItem* item, u32 value, std::optional<u32> old_value) {
        const bool changed = old_value && *old_value != value;
        item->setText(2, changed ? QStringLiteral("0x%1").arg(*old_value, 8, 16, QLatin1Char('0'))
                                 : QString{});
        item->setToolTip(1, changed ? QObject::tr("Changed since the previous debugger capture")
                                    : QString{});
        item->setBackground(1, changed ? QApplication::palette().brush(QPalette::Highlight)
                                       : QBrush{});
        item->setForeground(1, changed ? QApplication::palette().brush(QPalette::HighlightedText)
                                       : QBrush{});
    };
    for (int i = 0; i < core_registers->childCount(); ++i) {
        core_registers->child(i)->setText(
            1, QStringLiteral("0x%1").arg(current.core[i], 8, 16, QLatin1Char('0')));
        show_change(core_registers->child(i), current.core[i],
                    previous ? std::optional<u32>{previous->core[i]} : std::nullopt);
    }

    for (int i = 0; i < vfp_registers->childCount(); ++i) {
        vfp_registers->child(i)->setText(
            1, QStringLiteral("0x%1").arg(current.vfp[i], 8, 16, QLatin1Char('0')));
        show_change(vfp_registers->child(i), current.vfp[i],
                    previous ? std::optional<u32>{previous->vfp[i]} : std::nullopt);
    }

    UpdateCPSRValues(current.cpsr);
    show_change(cpsr, current.cpsr, previous ? std::optional<u32>{previous->cpsr} : std::nullopt);
    UpdateVFPSystemRegisterValues(current.fpscr, current.fpexc);
    show_change(vfp_system_registers->child(0), current.fpscr,
                previous ? std::optional<u32>{previous->fpscr} : std::nullopt);
    show_change(vfp_system_registers->child(1), current.fpexc,
                previous ? std::optional<u32>{previous->fpexc} : std::nullopt);
}

void RegistersWidget::OnDebugModeLeft() {}

void RegistersWidget::OnEmulationStarting(EmuThread* emu_thread) {
    capture_status->setText(tr("Pause the CPU to capture registers"));
    core_selector->clear();
    for (u32 core = 0; core < system.GetNumCores(); ++core) {
        core_selector->addItem(tr("Core %1").arg(core));
    }
    Debugger::SetDockAvailable(this, true);
}

void RegistersWidget::OnEmulationStopping() {
    current_capture_id = 0;
    previous_capture_id = 0;
    capture_status->setText(tr("No title running"));
    core_selector->clear();
    // Reset widget text
    for (int i = 0; i < core_registers->childCount(); ++i) {
        core_registers->child(i)->setText(1, QString{});
        core_registers->child(i)->setText(2, QString{});
    }

    for (int i = 0; i < vfp_registers->childCount(); ++i) {
        vfp_registers->child(i)->setText(1, QString{});
        vfp_registers->child(i)->setText(2, QString{});
    }

    for (int i = 0; i < cpsr->childCount(); ++i) {
        cpsr->child(i)->setText(1, QString{});
    }

    cpsr->setText(1, QString{});

    // FPSCR
    for (int i = 0; i < vfp_system_registers->child(0)->childCount(); ++i) {
        vfp_system_registers->child(0)->child(i)->setText(1, QString{});
    }

    // FPEXC
    for (int i = 0; i < vfp_system_registers->child(1)->childCount(); ++i) {
        vfp_system_registers->child(1)->child(i)->setText(1, QString{});
    }

    vfp_system_registers->child(0)->setText(1, QString{});
    vfp_system_registers->child(1)->setText(1, QString{});

    Debugger::SetDockAvailable(this, false);
}

void RegistersWidget::CreateCPSRChildren() {
    cpsr->addChild(new QTreeWidgetItem(QStringList(QStringLiteral("M"))));
    cpsr->addChild(new QTreeWidgetItem(QStringList(QStringLiteral("T"))));
    cpsr->addChild(new QTreeWidgetItem(QStringList(QStringLiteral("F"))));
    cpsr->addChild(new QTreeWidgetItem(QStringList(QStringLiteral("I"))));
    cpsr->addChild(new QTreeWidgetItem(QStringList(QStringLiteral("A"))));
    cpsr->addChild(new QTreeWidgetItem(QStringList(QStringLiteral("E"))));
    cpsr->addChild(new QTreeWidgetItem(QStringList(QStringLiteral("IT"))));
    cpsr->addChild(new QTreeWidgetItem(QStringList(QStringLiteral("GE"))));
    cpsr->addChild(new QTreeWidgetItem(QStringList(QStringLiteral("DNM"))));
    cpsr->addChild(new QTreeWidgetItem(QStringList(QStringLiteral("J"))));
    cpsr->addChild(new QTreeWidgetItem(QStringList(QStringLiteral("Q"))));
    cpsr->addChild(new QTreeWidgetItem(QStringList(QStringLiteral("V"))));
    cpsr->addChild(new QTreeWidgetItem(QStringList(QStringLiteral("C"))));
    cpsr->addChild(new QTreeWidgetItem(QStringList(QStringLiteral("Z"))));
    cpsr->addChild(new QTreeWidgetItem(QStringList(QStringLiteral("N"))));
}

void RegistersWidget::UpdateCPSRValues(u32 cpsr_val) {
    cpsr->setText(1, QStringLiteral("0x%1").arg(cpsr_val, 8, 16, QLatin1Char('0')));
    cpsr->child(0)->setText(
        1, QStringLiteral("b%1").arg(cpsr_val & 0x1F, 5, 2, QLatin1Char('0'))); // M - Mode
    cpsr->child(1)->setText(1, QString::number((cpsr_val >> 5) & 1));           // T - State
    cpsr->child(2)->setText(1, QString::number((cpsr_val >> 6) & 1));           // F - FIQ disable
    cpsr->child(3)->setText(1, QString::number((cpsr_val >> 7) & 1));           // I - IRQ disable
    cpsr->child(4)->setText(1, QString::number((cpsr_val >> 8) & 1)); // A - Imprecise abort
    cpsr->child(5)->setText(1, QString::number((cpsr_val >> 9) & 1)); // E - Data endianness
    cpsr->child(6)->setText(1,
                            QString::number((cpsr_val >> 10) & 0x3F)); // IT - If-Then state (DNM)
    cpsr->child(7)->setText(1,
                            QString::number((cpsr_val >> 16) & 0xF)); // GE - Greater-than-or-Equal
    cpsr->child(8)->setText(1, QString::number((cpsr_val >> 20) & 0xF)); // DNM - Do not modify
    cpsr->child(9)->setText(1, QString::number((cpsr_val >> 24) & 1));   // J - Jazelle
    cpsr->child(10)->setText(1, QString::number((cpsr_val >> 27) & 1));  // Q - Saturation
    cpsr->child(11)->setText(1, QString::number((cpsr_val >> 28) & 1));  // V - Overflow
    cpsr->child(12)->setText(1, QString::number((cpsr_val >> 29) & 1));  // C - Carry/Borrow/Extend
    cpsr->child(13)->setText(1, QString::number((cpsr_val >> 30) & 1));  // Z - Zero
    cpsr->child(14)->setText(1, QString::number((cpsr_val >> 31) & 1));  // N - Negative/Less than
}

void RegistersWidget::CreateVFPSystemRegisterChildren() {
    QTreeWidgetItem* const fpscr = new QTreeWidgetItem(QStringList(QStringLiteral("FPSCR")));
    fpscr->addChild(new QTreeWidgetItem(QStringList(QStringLiteral("IOC"))));
    fpscr->addChild(new QTreeWidgetItem(QStringList(QStringLiteral("DZC"))));
    fpscr->addChild(new QTreeWidgetItem(QStringList(QStringLiteral("OFC"))));
    fpscr->addChild(new QTreeWidgetItem(QStringList(QStringLiteral("UFC"))));
    fpscr->addChild(new QTreeWidgetItem(QStringList(QStringLiteral("IXC"))));
    fpscr->addChild(new QTreeWidgetItem(QStringList(QStringLiteral("IDC"))));
    fpscr->addChild(new QTreeWidgetItem(QStringList(QStringLiteral("IOE"))));
    fpscr->addChild(new QTreeWidgetItem(QStringList(QStringLiteral("DZE"))));
    fpscr->addChild(new QTreeWidgetItem(QStringList(QStringLiteral("OFE"))));
    fpscr->addChild(new QTreeWidgetItem(QStringList(QStringLiteral("UFE"))));
    fpscr->addChild(new QTreeWidgetItem(QStringList(QStringLiteral("IXE"))));
    fpscr->addChild(new QTreeWidgetItem(QStringList(QStringLiteral("IDE"))));
    fpscr->addChild(new QTreeWidgetItem(QStringList(tr("Vector Length"))));
    fpscr->addChild(new QTreeWidgetItem(QStringList(tr("Vector Stride"))));
    fpscr->addChild(new QTreeWidgetItem(QStringList(tr("Rounding Mode"))));
    fpscr->addChild(new QTreeWidgetItem(QStringList(QStringLiteral("FZ"))));
    fpscr->addChild(new QTreeWidgetItem(QStringList(QStringLiteral("DN"))));
    fpscr->addChild(new QTreeWidgetItem(QStringList(QStringLiteral("V"))));
    fpscr->addChild(new QTreeWidgetItem(QStringList(QStringLiteral("C"))));
    fpscr->addChild(new QTreeWidgetItem(QStringList(QStringLiteral("Z"))));
    fpscr->addChild(new QTreeWidgetItem(QStringList(QStringLiteral("N"))));

    QTreeWidgetItem* const fpexc = new QTreeWidgetItem(QStringList(QStringLiteral("FPEXC")));
    fpexc->addChild(new QTreeWidgetItem(QStringList(QStringLiteral("IOC"))));
    fpexc->addChild(new QTreeWidgetItem(QStringList(QStringLiteral("OFC"))));
    fpexc->addChild(new QTreeWidgetItem(QStringList(QStringLiteral("UFC"))));
    fpexc->addChild(new QTreeWidgetItem(QStringList(QStringLiteral("INV"))));
    fpexc->addChild(new QTreeWidgetItem(QStringList(tr("Vector Iteration Count"))));
    fpexc->addChild(new QTreeWidgetItem(QStringList(QStringLiteral("FP2V"))));
    fpexc->addChild(new QTreeWidgetItem(QStringList(QStringLiteral("EN"))));
    fpexc->addChild(new QTreeWidgetItem(QStringList(QStringLiteral("EX"))));

    vfp_system_registers->addChild(fpscr);
    vfp_system_registers->addChild(fpexc);
}

void RegistersWidget::UpdateVFPSystemRegisterValues(u32 fpscr_val, u32 fpexc_val) {
    QTreeWidgetItem* const fpscr = vfp_system_registers->child(0);
    fpscr->setText(1, QStringLiteral("0x%1").arg(fpscr_val, 8, 16, QLatin1Char('0')));
    fpscr->child(0)->setText(1, QString::number(fpscr_val & 1));
    fpscr->child(1)->setText(1, QString::number((fpscr_val >> 1) & 1));
    fpscr->child(2)->setText(1, QString::number((fpscr_val >> 2) & 1));
    fpscr->child(3)->setText(1, QString::number((fpscr_val >> 3) & 1));
    fpscr->child(4)->setText(1, QString::number((fpscr_val >> 4) & 1));
    fpscr->child(5)->setText(1, QString::number((fpscr_val >> 7) & 1));
    fpscr->child(6)->setText(1, QString::number((fpscr_val >> 8) & 1));
    fpscr->child(7)->setText(1, QString::number((fpscr_val >> 9) & 1));
    fpscr->child(8)->setText(1, QString::number((fpscr_val >> 10) & 1));
    fpscr->child(9)->setText(1, QString::number((fpscr_val >> 11) & 1));
    fpscr->child(10)->setText(1, QString::number((fpscr_val >> 12) & 1));
    fpscr->child(11)->setText(1, QString::number((fpscr_val >> 15) & 1));
    fpscr->child(12)->setText(
        1, QStringLiteral("b%1").arg((fpscr_val >> 16) & 7, 3, 2, QLatin1Char('0')));
    fpscr->child(13)->setText(
        1, QStringLiteral("b%1").arg((fpscr_val >> 20) & 3, 2, 2, QLatin1Char('0')));
    fpscr->child(14)->setText(
        1, QStringLiteral("b%1").arg((fpscr_val >> 22) & 3, 2, 2, QLatin1Char('0')));
    fpscr->child(15)->setText(1, QString::number((fpscr_val >> 24) & 1));
    fpscr->child(16)->setText(1, QString::number((fpscr_val >> 25) & 1));
    fpscr->child(17)->setText(1, QString::number((fpscr_val >> 28) & 1));
    fpscr->child(18)->setText(1, QString::number((fpscr_val >> 29) & 1));
    fpscr->child(19)->setText(1, QString::number((fpscr_val >> 30) & 1));
    fpscr->child(20)->setText(1, QString::number((fpscr_val >> 31) & 1));

    QTreeWidgetItem* const fpexc = vfp_system_registers->child(1);
    fpexc->setText(1, QStringLiteral("0x%1").arg(fpexc_val, 8, 16, QLatin1Char('0')));
    fpexc->child(0)->setText(1, QString::number(fpexc_val & 1));
    fpexc->child(1)->setText(1, QString::number((fpexc_val >> 2) & 1));
    fpexc->child(2)->setText(1, QString::number((fpexc_val >> 3) & 1));
    fpexc->child(3)->setText(1, QString::number((fpexc_val >> 7) & 1));
    fpexc->child(4)->setText(
        1, QStringLiteral("b%1").arg((fpexc_val >> 8) & 7, 3, 2, QLatin1Char('0')));
    fpexc->child(5)->setText(1, QString::number((fpexc_val >> 28) & 1));
    fpexc->child(6)->setText(1, QString::number((fpexc_val >> 30) & 1));
    fpexc->child(7)->setText(1, QString::number((fpexc_val >> 31) & 1));
}
