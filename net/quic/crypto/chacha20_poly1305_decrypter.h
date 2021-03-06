// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NET_QUIC_CRYPTO_CHACHA20_POLY1305_DECRYPTER_H_
#define NET_QUIC_CRYPTO_CHACHA20_POLY1305_DECRYPTER_H_

#include "net/quic/crypto/aead_base_decrypter.h"

namespace net {

// A ChaCha20Poly1305Decrypter is a QuicDecrypter that implements the
// AEAD_CHACHA20_POLY1305 algorithm specified in
// draft-agl-tls-chacha20poly1305-04. Create an instance by calling
// QuicDecrypter::Create(kCC12).
//
// It uses an authentication tag of 16 bytes (128 bits). There is no
// fixed nonce prefix.
class NET_EXPORT_PRIVATE ChaCha20Poly1305Decrypter : public AeadBaseDecrypter {
 public:
  enum {
    kAuthTagSize = 16,
  };

  ChaCha20Poly1305Decrypter();
  virtual ~ChaCha20Poly1305Decrypter();

  // Returns true if the underlying crypto library supports ChaCha20+Poly1305.
  static bool IsSupported();

#if !defined(USE_OPENSSL)
 protected:
  // AeadBaseDecrypter methods:
  virtual void FillAeadParams(base::StringPiece nonce,
                              base::StringPiece associated_data,
                              size_t auth_tag_size,
                              AeadParams* aead_params) const OVERRIDE;
#endif
};

}  // namespace net

#endif  // NET_QUIC_CRYPTO_CHACHA20_POLY1305_DECRYPTER_H_
