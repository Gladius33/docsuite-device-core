// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include "docsuite/device/device_manager.hpp"
#include "docsuite/ocr/tesseract_ocr.hpp"

#include <QByteArray>
#include <QImage>
#include <QMainWindow>
#include <QString>

#include <memory>
#include <optional>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QScrollArea;
class QTableWidget;
class QTabWidget;
class QTextEdit;
class QResizeEvent;

namespace docsuite::desktop {

class DeviceCenterWindow final : public QMainWindow {
public:
    explicit DeviceCenterWindow(QWidget* parent = nullptr);

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void build_ui();
    void reload_devices();
    void load_printer_details(const QString& printer_name);

    void start_scan(bool preview);
    void show_scan_frame(const std::shared_ptr<ScanFrame>& frame);
    void refresh_scan_preview();
    void save_scan_image(const QByteArray& format);
    void save_scan_pdf();
    void rotate_scan(int degrees);
    void run_ocr();

    void browse_print_file();
    void submit_print(bool diagnostic);
    void refresh_jobs();
    void cancel_selected_job();
    [[nodiscard]] PrintProfile selected_print_profile() const;

    std::shared_ptr<DeviceManager> manager_;
    TesseractOcr ocr_{};

    QTabWidget* tabs_{nullptr};

    QLabel* device_summary_{nullptr};
    QPushButton* device_refresh_{nullptr};
    QListWidget* device_printers_{nullptr};
    QListWidget* device_scanners_{nullptr};
    QTextEdit* device_details_{nullptr};

    QComboBox* scan_scanner_{nullptr};
    QComboBox* scan_mode_{nullptr};
    QComboBox* scan_dpi_{nullptr};
    QPushButton* scan_preview_button_{nullptr};
    QPushButton* scan_acquire_button_{nullptr};
    QPushButton* scan_save_png_{nullptr};
    QPushButton* scan_save_jpeg_{nullptr};
    QPushButton* scan_save_pdf_{nullptr};
    QPushButton* scan_rotate_left_{nullptr};
    QPushButton* scan_rotate_right_{nullptr};
    QPushButton* scan_ocr_button_{nullptr};
    QLabel* scan_status_{nullptr};
    QScrollArea* scan_scroll_{nullptr};
    QLabel* scan_image_{nullptr};
    QPlainTextEdit* scan_ocr_text_{nullptr};
    std::shared_ptr<ScanFrame> scan_frame_backing_{};
    QImage scan_image_data_{};
    std::optional<OcrResult> scan_ocr_result_{};
    int scan_dpi_value_{300};

    QComboBox* print_printer_{nullptr};
    QLineEdit* print_file_{nullptr};
    QComboBox* print_profile_{nullptr};
    QPushButton* print_submit_{nullptr};
    QPushButton* print_diagnose_{nullptr};
    QPushButton* jobs_refresh_{nullptr};
    QPushButton* jobs_cancel_{nullptr};
    QTableWidget* jobs_table_{nullptr};
    QPlainTextEdit* print_log_{nullptr};
};

} // namespace docsuite::desktop
