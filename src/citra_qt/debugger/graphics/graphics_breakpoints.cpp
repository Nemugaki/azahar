// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QMetaType>
#include <QPushButton>
#include <QTreeView>
#include <QVBoxLayout>
#include "citra_qt/debugger/graphics/graphics_breakpoints.h"
#include "citra_qt/debugger/graphics/graphics_breakpoints_p.h"
#include "core/core.h"

BreakPointModel::BreakPointModel(std::shared_ptr<Pica::DebugContext> debug_context, QObject* parent)
    : QAbstractListModel(parent), context_weak(debug_context),
      at_breakpoint(debug_context->at_breakpoint),
      active_breakpoint(debug_context->active_breakpoint) {}

int BreakPointModel::columnCount([[maybe_unused]] const QModelIndex& parent) const {
    return 2;
}

int BreakPointModel::rowCount([[maybe_unused]] const QModelIndex& parent) const {
    return static_cast<int>(Pica::DebugContext::Event::NumEvents);
}

QVariant BreakPointModel::data(const QModelIndex& index, int role) const {
    const auto event = static_cast<Pica::DebugContext::Event>(index.row());

    switch (role) {
    case Qt::DisplayRole: {
        if (index.column() == 0) {
            return DebugContextEventToString(event);
        }
        if (index.column() == 1) {
            auto context = context_weak.lock();
            if (!context) {
                return {};
            }
            const auto condition = context->GetBreakpointCondition(event);
            if (condition.field == Pica::DebugContext::ConditionField::None) {
                return tr("always");
            }
            const auto field = [condition] {
                switch (condition.field) {
                case Pica::DebugContext::ConditionField::EventData:
                    return QStringLiteral("command");
                case Pica::DebugContext::ConditionField::ColorBuffer:
                    return QStringLiteral("color");
                case Pica::DebugContext::ConditionField::DepthBuffer:
                    return QStringLiteral("depth");
                case Pica::DebugContext::ConditionField::DrawIndex:
                    return QStringLiteral("draw");
                case Pica::DebugContext::ConditionField::FrameIndex:
                    return QStringLiteral("frame");
                case Pica::DebugContext::ConditionField::None:
                    break;
                }
                return QString{};
            }();
            return QStringLiteral("%1=0x%2/0x%3")
                .arg(field)
                .arg(condition.value, 0, 16)
                .arg(condition.mask, 0, 16);
        }
        break;
    }

    case Qt::CheckStateRole: {
        if (index.column() == 0)
            return data(index, Role_IsEnabled).toBool() ? Qt::Checked : Qt::Unchecked;
        break;
    }

    case Qt::BackgroundRole: {
        if (at_breakpoint && index.row() == static_cast<int>(active_breakpoint)) {
            return QBrush(QColor(0xE0, 0xE0, 0x10));
        }
        break;
    }

    case Role_IsEnabled: {
        auto context = context_weak.lock();
        return context && context->breakpoints[(int)event].enabled;
    }

    default:
        break;
    }
    return QVariant();
}

QVariant BreakPointModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return {};
    }
    return section == 0 ? tr("Breakpoint") : tr("Condition");
}

Qt::ItemFlags BreakPointModel::flags(const QModelIndex& index) const {
    if (!index.isValid()) {
        return {};
    }

    Qt::ItemFlags flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    if (index.column() == 0) {
        flags |= Qt::ItemIsUserCheckable;
    }

    return flags;
}

bool BreakPointModel::setData(const QModelIndex& index, const QVariant& value, int role) {
    const auto event = static_cast<Pica::DebugContext::Event>(index.row());

    switch (role) {
    case Qt::CheckStateRole: {
        if (index.column() != 0)
            return false;

        auto context = context_weak.lock();
        if (!context)
            return false;

        context->SetBreakpoint(event, value == Qt::Checked);
        QModelIndex changed_index = createIndex(index.row(), 0);
        emit dataChanged(changed_index, changed_index);
        return true;
    }
    }

    return false;
}

void BreakPointModel::OnBreakPointHit(Pica::DebugContext::Event event) {
    auto context = context_weak.lock();
    if (!context)
        return;

    active_breakpoint = context->active_breakpoint;
    at_breakpoint = context->at_breakpoint;
    emit dataChanged(createIndex(static_cast<int>(event), 0),
                     createIndex(static_cast<int>(event), 0));
}

void BreakPointModel::OnResumed() {
    auto context = context_weak.lock();
    if (!context)
        return;

    at_breakpoint = context->at_breakpoint;
    emit dataChanged(createIndex(static_cast<int>(active_breakpoint), 0),
                     createIndex(static_cast<int>(active_breakpoint), 0));
    active_breakpoint = context->active_breakpoint;
}

QString BreakPointModel::DebugContextEventToString(Pica::DebugContext::Event event) {
    switch (event) {
    case Pica::DebugContext::Event::PicaCommandLoaded:
        return tr("Pica command loaded");
    case Pica::DebugContext::Event::PicaCommandProcessed:
        return tr("Pica command processed");
    case Pica::DebugContext::Event::IncomingPrimitiveBatch:
        return tr("Incoming primitive batch");
    case Pica::DebugContext::Event::FinishedPrimitiveBatch:
        return tr("Finished primitive batch");
    case Pica::DebugContext::Event::VertexShaderInvocation:
        return tr("Vertex shader invocation");
    case Pica::DebugContext::Event::IncomingDisplayTransfer:
        return tr("Incoming display transfer");
    case Pica::DebugContext::Event::GSPCommandProcessed:
        return tr("GSP command processed");
    case Pica::DebugContext::Event::BufferSwapped:
        return tr("Buffers swapped");
    case Pica::DebugContext::Event::NumEvents:
        break;
    }
    return tr("Unknown debug context event");
}

GraphicsBreakPointsWidget::GraphicsBreakPointsWidget(
    Core::System& system_, std::shared_ptr<Pica::DebugContext> debug_context, QWidget* parent)
    : QDockWidget(tr("Pica Breakpoints"), parent),
      Pica::DebugContext::BreakPointObserver(debug_context), system{system_} {
    setObjectName(QStringLiteral("PicaBreakPointsWidget"));

    status_text = new QLabel(tr("Emulation running"));
    resume_button = new QPushButton(tr("Resume"));
    resume_button->setEnabled(false);
    frame_advance_button = new QPushButton(tr("Advance Frame"));
    frame_advance_button->setEnabled(false);
    capture_button = new QPushButton(tr("Capture State"));
    capture_button->setEnabled(false);

    breakpoint_model = new BreakPointModel(debug_context, this);
    breakpoint_list = new QTreeView;
    breakpoint_list->setRootIsDecorated(false);
    breakpoint_list->setHeaderHidden(false);
    breakpoint_list->setModel(breakpoint_model);

    condition_field = new QComboBox;
    condition_field->setAccessibleName(tr("Breakpoint condition field"));
    condition_field->addItem(
        tr("Always"), static_cast<u32>(Pica::DebugContext::ConditionField::None));
    condition_field->addItem(
        tr("Pica command register"),
        static_cast<u32>(Pica::DebugContext::ConditionField::EventData));
    condition_field->addItem(
        tr("Color target"),
        static_cast<u32>(Pica::DebugContext::ConditionField::ColorBuffer));
    condition_field->addItem(
        tr("Depth target"),
        static_cast<u32>(Pica::DebugContext::ConditionField::DepthBuffer));
    condition_field->addItem(
        tr("Draw index"), static_cast<u32>(Pica::DebugContext::ConditionField::DrawIndex));
    condition_field->addItem(
        tr("Frame index"), static_cast<u32>(Pica::DebugContext::ConditionField::FrameIndex));
    condition_value = new QLineEdit;
    condition_value->setAccessibleName(tr("Breakpoint condition value"));
    condition_mask = new QLineEdit(QStringLiteral("0xffffffff"));
    condition_mask->setAccessibleName(tr("Breakpoint condition mask"));
    condition_current = new QPushButton(tr("Use Current"));
    condition_error = new QLabel;
    condition_error->setAccessibleName(tr("Breakpoint condition status"));

    qRegisterMetaType<Pica::DebugContext::Event>("Pica::DebugContext::Event");

    connect(breakpoint_list, &QTreeView::doubleClicked, this,
            &GraphicsBreakPointsWidget::OnItemDoubleClicked);
    connect(breakpoint_list->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex& current) { LoadCondition(current); });
    const auto update_condition_controls = [this] {
        const auto field = static_cast<Pica::DebugContext::ConditionField>(
            condition_field->currentData().toUInt());
        const bool conditional = field != Pica::DebugContext::ConditionField::None;
        condition_value->setEnabled(conditional);
        condition_mask->setEnabled(conditional);
        condition_current->setEnabled(
            field == Pica::DebugContext::ConditionField::ColorBuffer ||
            field == Pica::DebugContext::ConditionField::DepthBuffer ||
            field == Pica::DebugContext::ConditionField::DrawIndex ||
            field == Pica::DebugContext::ConditionField::FrameIndex);
    };
    connect(condition_field, qOverload<int>(&QComboBox::currentIndexChanged), this,
            update_condition_controls);
    update_condition_controls();
    connect(condition_current, &QPushButton::clicked, this,
            &GraphicsBreakPointsWidget::UseCurrentConditionValue);

    connect(resume_button, &QPushButton::clicked, this,
            &GraphicsBreakPointsWidget::OnResumeRequested);
    connect(frame_advance_button, &QPushButton::clicked, this,
            &GraphicsBreakPointsWidget::FrameAdvanceRequested);
    connect(capture_button, &QPushButton::clicked, this,
            &GraphicsBreakPointsWidget::CaptureState);

    connect(this, &GraphicsBreakPointsWidget::BreakPointHit, this,
            &GraphicsBreakPointsWidget::OnBreakPointHit, Qt::BlockingQueuedConnection);
    connect(this, &GraphicsBreakPointsWidget::Resumed, this, &GraphicsBreakPointsWidget::OnResumed);

    connect(this, &GraphicsBreakPointsWidget::BreakPointHit, breakpoint_model,
            &BreakPointModel::OnBreakPointHit, Qt::BlockingQueuedConnection);
    connect(this, &GraphicsBreakPointsWidget::Resumed, breakpoint_model,
            &BreakPointModel::OnResumed);

    connect(this, &GraphicsBreakPointsWidget::BreakPointsChanged,
            [this](const QModelIndex& top_left, const QModelIndex& bottom_right) {
                breakpoint_model->dataChanged(top_left, bottom_right);
            });

    QWidget* main_widget = new QWidget;
    auto main_layout = new QVBoxLayout;
    {
        auto sub_layout = new QHBoxLayout;
        sub_layout->addWidget(status_text);
        sub_layout->addWidget(capture_button);
        sub_layout->addWidget(frame_advance_button);
        sub_layout->addWidget(resume_button);
        main_layout->addLayout(sub_layout);
    }
    main_layout->addWidget(breakpoint_list);
    auto* condition_group = new QGroupBox(tr("Selected breakpoint condition"));
    auto* condition_layout = new QFormLayout;
    condition_layout->addRow(tr("Field:"), condition_field);
    condition_layout->addRow(tr("Value:"), condition_value);
    condition_layout->addRow(tr("Mask:"), condition_mask);
    auto* condition_buttons = new QHBoxLayout;
    condition_buttons->addWidget(condition_current);
    auto* apply_condition = new QPushButton(tr("Apply"));
    condition_buttons->addWidget(apply_condition);
    condition_layout->addRow(condition_buttons);
    condition_layout->addRow(condition_error);
    condition_group->setLayout(condition_layout);
    main_layout->addWidget(condition_group);
    connect(apply_condition, &QPushButton::clicked, this,
            &GraphicsBreakPointsWidget::ApplyCondition);
    main_widget->setLayout(main_layout);

    setWidget(main_widget);
    breakpoint_list->setCurrentIndex(breakpoint_model->index(0, 0));
}

void GraphicsBreakPointsWidget::LoadCondition(const QModelIndex& index) {
    const auto context = context_weak.lock();
    if (!index.isValid() || !context) {
        return;
    }
    const auto condition = context->GetBreakpointCondition(static_cast<Event>(index.row()));
    condition_field->setCurrentIndex(
        std::max(condition_field->findData(static_cast<u32>(condition.field)), 0));
    condition_value->setText(
        QStringLiteral("0x%1").arg(condition.value, 8, 16, QLatin1Char('0')));
    condition_mask->setText(
        QStringLiteral("0x%1").arg(condition.mask, 8, 16, QLatin1Char('0')));
    condition_error->clear();
}

void GraphicsBreakPointsWidget::ApplyCondition() {
    const QModelIndex index = breakpoint_list->currentIndex();
    const auto context = context_weak.lock();
    if (!index.isValid() || !context) {
        return;
    }
    Pica::DebugContext::BreakPointCondition condition{};
    condition.field = static_cast<Pica::DebugContext::ConditionField>(
        condition_field->currentData().toUInt());
    if (condition.field != Pica::DebugContext::ConditionField::None) {
        bool value_ok{};
        bool mask_ok{};
        condition.value = condition_value->text().trimmed().toUInt(&value_ok, 0);
        condition.mask = condition_mask->text().trimmed().toUInt(&mask_ok, 0);
        if (!value_ok || !mask_ok) {
            condition_error->setText(
                tr("Value and mask must be 32-bit decimal or 0x-prefixed hexadecimal numbers."));
            return;
        }
    }
    context->SetBreakpointCondition(static_cast<Event>(index.row()), condition);
    condition_error->setText(tr("Condition applied"));
    emit BreakPointsChanged(breakpoint_model->index(index.row(), 1),
                            breakpoint_model->index(index.row(), 1));
}

void GraphicsBreakPointsWidget::UseCurrentConditionValue() {
    const auto context = context_weak.lock();
    if (!context) {
        return;
    }
    const auto field = static_cast<Pica::DebugContext::ConditionField>(
        condition_field->currentData().toUInt());
    const auto target = context->GetRenderTargetInfo();
    const auto position = context->GetTimelinePosition();
    const u32 value = field == Pica::DebugContext::ConditionField::ColorBuffer
                          ? target.color_address
                      : field == Pica::DebugContext::ConditionField::DepthBuffer
                          ? target.depth_address
                      : field == Pica::DebugContext::ConditionField::DrawIndex ? position.draw
                                                                              : position.frame;
    condition_value->setText(QStringLiteral("0x%1").arg(value, 8, 16, QLatin1Char('0')));
}

void GraphicsBreakPointsWidget::OnPicaBreakPointHit(Event event, const void* data) {
    // Process in GUI thread
    emit BreakPointHit(event, data);
}

void GraphicsBreakPointsWidget::OnBreakPointHit(Pica::DebugContext::Event event, const void* data) {
    status_text->setText(tr("Emulation halted at breakpoint"));
    capture_button->setEnabled(true);
    resume_button->setEnabled(true);
    frame_advance_button->setEnabled(true);
}

void GraphicsBreakPointsWidget::OnPicaResume() {
    // Process in GUI thread
    emit Resumed();
}

void GraphicsBreakPointsWidget::OnResumed() {
    status_text->setText(tr("Emulation running"));
    resume_button->setEnabled(false);
    frame_advance_button->setEnabled(false);
    capture_button->setEnabled(false);
}

void GraphicsBreakPointsWidget::CaptureState() {
    const auto capture = system.CreateDebugCapture();
    status_text->setText(capture.id ? tr("Stored capture %1 (%2 bytes)").arg(capture.id).arg(capture.size)
                                    : tr("State changed before it could be captured"));
}

void GraphicsBreakPointsWidget::OnResumeRequested() {
    if (auto context = context_weak.lock())
        context->Resume();
}

void GraphicsBreakPointsWidget::OnItemDoubleClicked(const QModelIndex& index) {
    if (!index.isValid())
        return;

    QModelIndex check_index = breakpoint_list->model()->index(index.row(), 0);
    QVariant enabled = breakpoint_list->model()->data(check_index, Qt::CheckStateRole);
    QVariant new_state = Qt::Unchecked;
    if (enabled == Qt::Unchecked)
        new_state = Qt::Checked;
    breakpoint_list->model()->setData(check_index, new_state, Qt::CheckStateRole);
}
