// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include "docsuite/device/device_manager.hpp"
#include "docsuite/ocr/tesseract_ocr.hpp"
#include "pdf_export.hpp"

#include <QWidget>

#include <memory>
#include <vector>

class QComboBox;
class QLabel;
class QListWidget;
class QPushButton;
class QScrollArea;

namespace docsuite::desktop {

class DocumentPage final : public QWidget {
public:
    explicit DocumentPage(std::shared_ptr<DeviceManager> manager, QWidget* parent = nullptr);

private:
    void refresh_scanners();
    void refresh_capabilities();
    void add_page();
    void delete_selected();
    void move_selected(int delta);
    void clear_pages();
    void run_ocr_selected();
    void export_pdf();
    void update_list();
    void update_preview();

    std::shared_ptr<DeviceManager> manager_;
    TesseractOcr ocr_{};
    std::vector<PdfScanPage> pages_{};

    QComboBox* scanner_{nullptr};
    QComboBox* mode_{nullptr};
    QComboBox* dpi_{nullptr};
    QComboBox* source_{nullptr};
    QPushButton* refresh_{nullptr};
    QPushButton* add_{nullptr};
    QPushButton* delete_{nullptr};
    QPushButton* up_{nullptr};
    QPushButton* down_{nullptr};
    QPushButton* ocr_{nullptr};
    QPushButton* export_{nullptr};
    QPushButton* clear_{nullptr};
    QListWidget* pages_list_{nullptr};
    QScrollArea* preview_scroll_{nullptr};
    QLabel* preview_{nullptr};
    QLabel* status_{nullptr};
};

} // namespace docsuite::desktop
