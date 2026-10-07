// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include "docsuite/device/device_manager.hpp"
#include "docsuite/image/image_processor.hpp"
#include "docsuite/ocr/tesseract_ocr.hpp"
#include "pdf_export.hpp"

#include <QWidget>

#include <memory>
#include <string>
#include <vector>

class QCheckBox;
class QComboBox;
class QLabel;
class QListWidget;
class QPushButton;
class QScrollArea;

namespace docsuite::desktop {

class SelectionPreview;

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
    void transform_selected(const std::string& operation);
    void crop_selection();
    void export_pdf();
    void update_list();
    void update_preview();

    std::shared_ptr<DeviceManager> manager_;
    TesseractOcr ocr_engine_{};
    ImageProcessor processor_{};
    std::vector<PdfScanPage> pages_{};

    QComboBox* scanner_{nullptr};
    QComboBox* mode_{nullptr};
    QComboBox* dpi_{nullptr};
    QComboBox* source_{nullptr};
    QCheckBox* auto_process_{nullptr};
    QCheckBox* skip_blank_{nullptr};
    QPushButton* refresh_{nullptr};
    QPushButton* add_{nullptr};
    QPushButton* delete_{nullptr};
    QPushButton* up_{nullptr};
    QPushButton* down_{nullptr};
    QPushButton* auto_crop_{nullptr};
    QPushButton* crop_selection_{nullptr};
    QPushButton* enhance_{nullptr};
    QPushButton* binarize_{nullptr};
    QPushButton* deskew_{nullptr};
    QPushButton* ocr_button_{nullptr};
    QPushButton* export_{nullptr};
    QPushButton* clear_{nullptr};
    QListWidget* pages_list_{nullptr};
    QScrollArea* preview_scroll_{nullptr};
    SelectionPreview* preview_{nullptr};
    QLabel* status_{nullptr};
};

} // namespace docsuite::desktop
