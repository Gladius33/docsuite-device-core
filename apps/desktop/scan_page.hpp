// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include "docsuite/device/device_manager.hpp"
#include "docsuite/ocr/tesseract_ocr.hpp"

#include <QByteArray>
#include <QImage>
#include <QWidget>

#include <memory>
#include <optional>

class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QResizeEvent;
class QScrollArea;

namespace docsuite::desktop {

class ScanPage final : public QWidget {
public:
    explicit ScanPage(std::shared_ptr<DeviceManager> manager, QWidget* parent = nullptr);
    void refresh_scanners();

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void load_capabilities();
    void start_scan(bool preview);
    void set_scan_frame(const std::shared_ptr<ScanFrame>& frame);
    void update_preview();
    void rotate(int degrees);
    void convert_to_grayscale();
    void save_image(const QByteArray& format);
    void save_pdf();
    void run_ocr();
    void copy_ocr_text();
    void save_ocr_text();
    [[nodiscard]] ScanFrame frame_for_ocr() const;

    std::shared_ptr<DeviceManager> manager_;
    TesseractOcr ocr_{};

    QComboBox* scanner_{nullptr};
    QComboBox* source_{nullptr};
    QComboBox* mode_{nullptr};
    QComboBox* dpi_{nullptr};
    QLabel* capabilities_{nullptr};
    QPushButton* refresh_{nullptr};
    QPushButton* preview_{nullptr};
    QPushButton* scan_{nullptr};
    QPushButton* rotate_left_{nullptr};
    QPushButton* rotate_right_{nullptr};
    QPushButton* grayscale_{nullptr};
    QPushButton* save_png_{nullptr};
    QPushButton* save_jpeg_{nullptr};
    QPushButton* save_pdf_{nullptr};
    QPushButton* ocr_button_{nullptr};
    QPushButton* copy_ocr_{nullptr};
    QPushButton* save_ocr_{nullptr};
    QLabel* status_{nullptr};
    QScrollArea* scroll_{nullptr};
    QLabel* image_label_{nullptr};
    QPlainTextEdit* ocr_text_{nullptr};

    std::shared_ptr<ScanFrame> frame_backing_{};
    QImage image_{};
    std::optional<OcrResult> ocr_result_{};
    int image_dpi_{300};
};

} // namespace docsuite::desktop
