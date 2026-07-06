/*! ----------------------------------------------------------------------------
 * @file    uwb_contract_version.h
 * @brief   Compile-time version guard for the vendored uwb_frames.h (UWB-229).
 *
 * drivers/dw1000/uwb_frames.h is a PINNED COPY of
 * contracts/uwb/uwb_frames.h from the FUNDAMENTALS_SPORTS monorepo — it is
 * copied byte-for-byte and must never be hand-edited in this repo. To change
 * a frame definition, edit the canonical file in
 * FUNDAMENTALS_SPORTS:contracts/uwb/uwb_frames.h, bump UWB_PROTOCOL_VERSION
 * there per contracts/uwb/README.md §Version pinning procedure, and re-vendor
 * the copy here (see CONTRACTS_VERSION at the repo root for the currently
 * pinned version and the re-vendor steps).
 *
 * This header does not itself define any wire format. It exists solely so
 * that any translation unit including uwb_frames.h can also include this
 * file and get a hard compile-time error if the vendored header's protocol
 * version ever drifts from the version this firmware was written against
 * (0.6) — e.g. because someone updated uwb_frames.h without following the
 * re-vendor procedure, or pinned a version bump without reviewing frame-
 * handling code for breakage.
 *
 * Usage:
 *   #include "uwb_frames.h"
 *   #include "uwb_contract_version.h"
 *
 * @attention
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 Fundamentals UWB project contributors.
 */

#ifndef UWB_CONTRACT_VERSION_H_
#define UWB_CONTRACT_VERSION_H_

#include "uwb_frames.h"

#if (UWB_PROTOCOL_VERSION_MAJOR != 0) || (UWB_PROTOCOL_VERSION_MINOR != 6)
#  error "Unexpected UWB protocol version — re-pin contracts/uwb into this firmware"
#endif

#endif /* UWB_CONTRACT_VERSION_H_ */
