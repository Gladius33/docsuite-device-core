// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include <QImage>
#include <QLabel>
#include <QRect>

#include <optional>

class QMouseEvent;
class QResizeEvent;
class QRubberBand;

namespace docsuite::desktop {

class SelectionPreview final : public QLabel {
public:
    explicit SelectionPreview(QWidget* parent = nullptr);

    void set_image(const QImage& image);
    void clear_image();
    void clear_selection();
    [[nodiscard]] std::optional<QRect> selected_image_rect() const;

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void refresh_pixmap();
    [[nodiscard]] QRect displayed_pixmap_rect() const;

    QImage image_;
    QRubberBand* rubber_band_{nullptr};
    QPoint origin_;
    QRect selection_;
};

} // namespace docsuite::desktop
