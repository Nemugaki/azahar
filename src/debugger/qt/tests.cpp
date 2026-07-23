// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version

#include <QApplication>

#include "capture_viewer.h"

int main(int argc, char** argv) {
    QApplication application{argc, argv};
    auto sessions = std::make_shared<Debugger::RenderSessionManager>("test", "software");
    Debugger::CaptureViewerWidget viewer{sessions};
    viewer.Refresh();
    return viewer.SelectedTarget().has_value();
}
