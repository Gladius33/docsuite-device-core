// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "selection_preview.hpp"

#include <QMouseEvent>
#include <QPixmap>
#include <QResizeEvent>
#include <QRubberBand>

#include <algorithm>
#include <cmath>

namespace docsuite::desktop {

SelectionPreview::SelectionPreview(QWidget* parent)
    : QLabel{parent}, rubber_band_{new QRubberBand(QRubberBand::Rectangle, this)} {
    setAlignment(Qt::AlignCenter);
    setMinimumSize(200, 200);
    setMouseTracking(true);
    setText(QStringLiteral("No document pages yet"));
}

void SelectionPreview::set_image(const QImage& image) {
    image_ = image;
    clear_selection();
    setText(QString{});
    refresh_pixmap();
}

void SelectionPreview::clear_image() {
    image_ = {};
    clear_selection();
    setPixmap(QPixmap{});
    setText(QStringLiteral("No document pages yet"));
}

void SelectionPreview::clear_selection() {
    selection_ = {};
    rubber_band_->hide();
}

QRect SelectionPreview::displayed_pixmap_rect() const {
    const QPixmap shown = pixmap(Qt::ReturnByValue);
    if (shown.isNull()) {
        return {};
    }
    const QRect area = contentsRect();
    const int x = area.x() + (area.width() - shown.width()) / 2;
    const int y = area.y() + (area.height() - shown.height()) / 2;
    return QRect{x, y, shown.width(), shown.height()};
}

std::optional<QRect> SelectionPreview::selected_image_rect() const {
    if (image_.isNull() || selection_.isEmpty()) {
        return std::nullopt;
    }

    const QRect shown = displayed_pixmap_rect();
    if (shown.isEmpty()) {
        return std::nullopt;
    }
    const QRect clipped = selection_.normalized().intersected(shown);
    if (clipped.width() < 3 || clipped.height() < 3) {
        return std::nullopt;
    }

    const double scale_x = static_cast<double>(image_.width()) /
        static_cast<double>(shown.width());
    const double scale_y = static_cast<double>(image_.height()) /
        static_cast<double>(shown.height());

    int left = static_cast<int>(std::floor(
        static_cast<double>(clipped.left() - shown.left()) * scale_x));
    int top = static_cast<int>(std::floor(
        static_cast<double>(clipped.top() - shown.top()) * scale_y));
    int right = static_cast<int>(std::ceil(
        static_cast<double>(clipped.right() - shown.left() + 1) * scale_x));
    int bottom = static_cast<int>(std::ceil(
        static_cast<double>(clipped.bottom() - shown.top() + 1) * scale_y));

    left = std::clamp(left, 0, image_.width() - 1);
    top = std::clamp(top, 0, image_.height() - 1);
    right = std::clamp(right, left + 1, image_.width());
    bottom = std::clamp(bottom, top + 1, image_.height());
    return QRect{left, top, right - left, bottom - top};
}

void SelectionPreview::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || image_.isNull()) {
        QLabel::mousePressEvent(event);
        return;
    }
    origin_ = event->position().toPoint();
    selection_ = QRect{origin_, QSize{}};
    rubber_band_->setGeometry(selection_);
    rubber_band_->show();
    event->accept();
}

void SelectionPreview::mouseMoveEvent(QMouseEvent* event) {
    if (!rubber_band_->isVisible()) {
        QLabel::mouseMoveEvent(event);
        return;
    }
    selection_ = QRect{origin_, event->position().toPoint()}.normalized();
    rubber_band_->setGeometry(selection_);
    event->accept();
}

void SelectionPreview::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || !rubber_band_->isVisible()) {
        QLabel::mouseReleaseEvent(event);
        return;
    }
    selection_ = QRect{origin_, event->position().toPoint()}.normalized();
    rubber_band_->setGeometry(selection_);
    event->accept();
}

void SelectionPreview::resizeEvent(QResizeEvent* event) {
    QLabel::resizeEvent(event);
    refresh_pixmap();
    clear_selection();
}

void SelectionPreview::refresh_pixmap() {
    if (image_.isNull()) {
        return;
    }
    QSize target = contentsRect().size() - QSize{12, 12};
    if (target.width() < 32 || target.height() < 32) {
        return;
    }
    setPixmap(
        QPixmap::fromImage(image_).scaled(
            target,
            Qt::KeepAspectRatio,
            Qt::SmoothTransformation));
}

} // namespace docsuite::desktop
