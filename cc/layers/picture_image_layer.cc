// Copyright 2010 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cc/layers/picture_image_layer.h"

#include "cc/layers/picture_image_layer_impl.h"
#include "third_party/skia/include/core/SkCanvas.h"

namespace cc {

scoped_refptr<PictureImageLayer> PictureImageLayer::Create() {
  return make_scoped_refptr(new PictureImageLayer());
}

PictureImageLayer::PictureImageLayer() : PictureLayer(this) {}

PictureImageLayer::~PictureImageLayer() {
  ClearClient();
}

scoped_ptr<LayerImpl> PictureImageLayer::CreateLayerImpl(
    LayerTreeImpl* tree_impl) {
  return PictureImageLayerImpl::Create(tree_impl, id()).PassAs<LayerImpl>();
}

bool PictureImageLayer::DrawsContent() const {
  return !bitmap_.isNull() && PictureLayer::DrawsContent();
}

void PictureImageLayer::SetBitmap(const SkBitmap& bitmap) {
  // SetBitmap() currently gets called whenever there is any
  // style change that affects the layer even if that change doesn't
  // affect the actual contents of the image (e.g. a CSS animation).
  // With this check in place we avoid unecessary texture uploads.
  if (bitmap.pixelRef() && bitmap.pixelRef() == bitmap_.pixelRef())
    return;

  bitmap_ = bitmap;
  SetNeedsDisplay();
}

void PictureImageLayer::PaintContents(SkCanvas* canvas,
                                      const gfx::Rect& clip,
                                      gfx::RectF* opaque) {
  if (!bitmap_.width() || !bitmap_.height())
    return;

  SkScalar content_to_layer_scale_x =
      SkFloatToScalar(static_cast<float>(bounds().width()) / bitmap_.width());
  SkScalar content_to_layer_scale_y =
      SkFloatToScalar(static_cast<float>(bounds().height()) / bitmap_.height());
  canvas->scale(content_to_layer_scale_x, content_to_layer_scale_y);

  // Because PictureImageLayer always FillsBoundsCompletely it will not clear
  // before painting on playback. As a result we must configure the paint to
  // copy over the uncleared destination, rather than blending with it.
  SkPaint paint;
  paint.setXfermodeMode(SkXfermode::kSrc_Mode);
  canvas->drawBitmap(bitmap_, 0, 0, &paint);
}

bool PictureImageLayer::FillsBoundsCompletely() const {
  // PictureImageLayer will always paint to the entire layer bounds.
  return true;
}

}  // namespace cc
