/*
 * uwb_frames.h — UWB frame definitions
 *
 * Common IEEE 802.15.4-2011 MAC header, frame-type registry, and compile-time
 * size/offset assertions.  Shared between C firmware (Zephyr/nRF) and C++
 * anchor host (Raspberry Pi).
 *
 * This is a hand-authored header — no generator.  Rationale: the MAC header
 * is small and stable; a generator would add build-time complexity for no
 * practical benefit at this scale.  See contracts/uwb/README.md §Design choices.
 *
 * Wire-shape changes (struct layout, enum values, protocol version) MUST be
 * made in a dedicated PR that bumps UWB_PROTOCOL_VERSION.  Never change the
 * wire shape as a side-effect of another PR.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef UWB_FRAMES_H
#define UWB_FRAMES_H

/* C++ guard — consumable by both C (firmware) and C++ (anchor host) */
#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>   /* offsetof */

/* =========================================================================
 * Protocol version — single source of truth
 *
 * Encoded as individual integer constants (usable in preprocessor comparisons)
 * plus a human-readable string.
 *
 * Bump rules (any wire-shape change requires a dedicated PR):
 *   MAJOR — incompatible change: altered field layout, changed enum value,
 *            removed frame type, renamed struct member.
 *   MINOR — backwards-compatible addition: new frame type with a previously
 *            unassigned ID, new reserved field added at the end of a struct.
 *
 * Firmware and anchor repos pin a specific version of contracts/ (see
 * contracts/uwb/README.md §Version pinning).
 * ========================================================================= */
#define UWB_PROTOCOL_VERSION_MAJOR  0
#define UWB_PROTOCOL_VERSION_MINOR  6
#define UWB_PROTOCOL_VERSION        "0.6"

/* =========================================================================
 * IEEE 802.15.4-2011 MAC header
 *
 * Protocol configuration:
 *   Frame type         : Data (0b001)
 *   Security           : Disabled
 *   Frame Pending      : 0
 *   ACK Request        : 0
 *   PAN ID Compression : Enabled — PAN ID appears only in the Destination
 *                        field; Source PAN ID is omitted (same network).
 *   Destination mode   : Short (16-bit)
 *   Frame version      : 0  (IEEE 802.15.4-2003 compatible)
 *   Source mode        : Short (16-bit)
 *
 * Resulting Frame Control word: 0x8841
 *   On wire (little-endian): byte 0 = 0x41, byte 1 = 0x88
 *
 * Byte layout (all multi-byte fields little-endian):
 *
 *   Offset  Width  Field
 *   ------  -----  -----------------------------------------------
 *      0      2    Frame Control  (see UWB_FRAME_CTRL_* constants)
 *      2      1    Sequence Number (0x00–0xFF, wraps)
 *      3      2    PAN ID          (UWB_PAN_ID = 0x5EEA)
 *      5      2    Destination Address (16-bit short)
 *      7      2    Source Address      (16-bit short)
 *   ------  -----
 *      9          Total MAC header size (UWB_MAC_HDR_SIZE)
 *
 * The DW1000 hardware appends a 2-byte FCS (CRC-16/CCITT) after the payload.
 * The FCS is NOT represented in any struct here; it is handled by hardware.
 * ========================================================================= */

#define UWB_MAC_HDR_SIZE      9u   /* bytes */

/* Frame Control field constants */
#define UWB_FRAME_CTRL_LOW    0x41u   /* FC byte 0 (offset 0) */
#define UWB_FRAME_CTRL_HIGH   0x88u   /* FC byte 1 (offset 1) */
#define UWB_FRAME_CTRL        0x8841u /* FC as a 16-bit LE integer */

/*
 * Frame Control bit positions (within the 16-bit word, bit 0 = LSb of byte 0).
 * These match IEEE 802.15.4-2011 Table 2.
 */
#define UWB_FC_FRAME_TYPE_SHIFT       0u
#define UWB_FC_FRAME_TYPE_MASK        0x0007u
#define UWB_FC_FRAME_TYPE_DATA        0x0001u  /* 0b001 */
#define UWB_FC_SECURITY_SHIFT         3u
#define UWB_FC_FRAME_PENDING_SHIFT    4u
#define UWB_FC_ACK_REQUEST_SHIFT      5u
#define UWB_FC_PAN_ID_COMPRESS_SHIFT  6u
#define UWB_FC_DEST_ADDR_MODE_SHIFT   10u
#define UWB_FC_DEST_ADDR_SHORT        0x0800u  /* 0b10 << 10 */
#define UWB_FC_FRAME_VERSION_SHIFT    12u
#define UWB_FC_SRC_ADDR_MODE_SHIFT    14u
#define UWB_FC_SRC_ADDR_SHORT         0x8000u  /* 0b10 << 14 */

/* Well-known PAN ID and addresses */
#define UWB_PAN_ID            0x5EEAu   /* Fundamentals Sports network PAN */
#define UWB_ADDR_BROADCAST    0xFFFFu   /* IEEE 802.15.4 broadcast short address */
#define UWB_ADDR_UNASSIGNED   0xFFFEu   /* Tag address before slot assignment */

/* Absolute byte-offset constants (for code that accesses raw buffers) */
#define UWB_OFF_FRAME_CTRL    0u   /* 2 bytes */
#define UWB_OFF_SEQ_NUM       2u   /* 1 byte  */
#define UWB_OFF_PAN_ID        3u   /* 2 bytes */
#define UWB_OFF_DEST_ADDR     5u   /* 2 bytes */
#define UWB_OFF_SRC_ADDR      7u   /* 2 bytes */
#define UWB_OFF_FRAME_TYPE    9u   /* 1 byte  — first application-layer byte */
#define UWB_OFF_PAYLOAD      10u   /* N bytes — frame-type specific */

/*
 * uwb_mac_hdr_t
 *
 * Packed byte representation of the IEEE 802.15.4 MAC header as configured
 * above (short addressing, PAN-ID compression).
 *
 * All fields are uint8_t or uint8_t[] — the compiler cannot insert padding,
 * so no __attribute__((packed)) is required.  The compile-time assertions
 * below confirm the layout matches the byte-offset table above.
 */
typedef struct {
    uint8_t frame_ctrl[2]; /* Frame Control, LE: [0]=LSB=0x41, [1]=MSB=0x88 */
    uint8_t seq_num;       /* Sequence Number, 0x00–0xFF, wraps               */
    uint8_t pan_id[2];     /* PAN ID, little-endian                           */
    uint8_t dest_addr[2];  /* Destination Address, 16-bit short, LE           */
    uint8_t src_addr[2];   /* Source Address,      16-bit short, LE           */
} uwb_mac_hdr_t;

/*
 * uwb_app_hdr_t
 *
 * MAC header (9 bytes) followed by the one-byte application-layer frame-type
 * discriminator (uwb_frame_type_t).  This 10-byte header is present in every
 * frame.  Per-frame payloads start at offset UWB_OFF_PAYLOAD (10).
 */
typedef struct {
    uwb_mac_hdr_t mac;        /* Bytes 0–8:  IEEE 802.15.4 MAC header */
    uint8_t       frame_type; /* Byte  9:    see uwb_frame_type_t     */
} uwb_app_hdr_t;

/* =========================================================================
 * Frame-type registry
 *
 * Stable numeric IDs.  Once assigned, a value belongs to that frame type
 * permanently — even if the frame type is retired.  Never reuse an ID.
 *
 * ID ranges:
 *   0x01–0x0F  Superframe control (sync, future masters)
 *   0x10–0x1F  DS-TWR calibration exchange
 *   0x20–0x2F  Registration and slot management
 *   0x30–0xFF  Reserved for future use
 *
 * Payloads for each frame type are defined in per-frame header files
 * (dependent subissues UWB-113, UWB-114, …).
 * ========================================================================= */
typedef enum {
    /*
     * Superframe control (0x01–0x0F)
     */

    /** SYNC — Superframe cycle-start broadcast from the clock-master anchor.
     *  Drives TDoA clock synchronisation (ADR-003).  All slave anchors
     *  hardware-timestamp this frame to estimate clock offset + drift.
     *  Sent to UWB_ADDR_BROADCAST; dest_addr field = 0xFFFF. */
    UWB_FRAME_TYPE_SYNC             = 0x01u,

    /** TAG_BLINK — TDoA positioning blink from a tag in its assigned TDMA slot.
     *  All anchors timestamp arrival; the solver multilaterates the tag position.
     *  Sent to UWB_ADDR_BROADCAST; dest_addr field = 0xFFFF. */
    UWB_FRAME_TYPE_TAG_BLINK        = 0x02u,

    /*
     * DS-TWR calibration exchange (0x10–0x1F)
     * Three-message double-sided TWR between any two devices (anchor↔anchor
     * or anchor↔reference tag).  Initiator starts with POLL, responder
     * replies with RESPONSE, initiator closes with FINAL.
     */

    /** TWR_POLL — DS-TWR Poll (initiator → responder, unicast). */
    UWB_FRAME_TYPE_TWR_POLL         = 0x10u,

    /** TWR_RESPONSE — DS-TWR Response (responder → initiator, unicast). */
    UWB_FRAME_TYPE_TWR_RESPONSE     = 0x11u,

    /** TWR_FINAL — DS-TWR Final (initiator → responder, unicast). */
    UWB_FRAME_TYPE_TWR_FINAL        = 0x12u,

    /*
     * Registration and slot management (0x20–0x2F)
     */

    /** JOIN_REQUEST — Aloha contention-based join from an unregistered tag.
     *  Tag uses UWB_ADDR_UNASSIGNED (0xFFFE) as src_addr until assigned. */
    UWB_FRAME_TYPE_JOIN_REQUEST     = 0x20u,

    /** SLOT_ASSIGNMENT — Master assigns a short address and TDMA slot to a tag
     *  in response to a JOIN_REQUEST. */
    UWB_FRAME_TYPE_SLOT_ASSIGNMENT  = 0x21u,

} uwb_frame_type_t;

/* =========================================================================
 * Compile-time assertions — sizes and field offsets
 *
 * If any assertion fires, a struct layout or ABI change has violated the
 * wire format.  Fix the struct; do not relax the assertion.
 * ========================================================================= */

#ifdef __cplusplus
#  define UWB_STATIC_ASSERT(cond, msg)  static_assert((cond), msg)
#else
#  define UWB_STATIC_ASSERT(cond, msg)  _Static_assert((cond), msg)
#endif

/* Struct sizes */
UWB_STATIC_ASSERT(sizeof(uwb_mac_hdr_t) == 9u,
    "uwb_mac_hdr_t must be exactly 9 bytes (IEEE 802.15.4 MAC header)");
UWB_STATIC_ASSERT(sizeof(uwb_app_hdr_t) == 10u,
    "uwb_app_hdr_t must be exactly 10 bytes (MAC hdr + frame-type byte)");

/* MAC header field offsets */
UWB_STATIC_ASSERT(offsetof(uwb_mac_hdr_t, frame_ctrl) == 0u,
    "frame_ctrl must be at byte offset 0");
UWB_STATIC_ASSERT(offsetof(uwb_mac_hdr_t, seq_num)    == 2u,
    "seq_num must be at byte offset 2");
UWB_STATIC_ASSERT(offsetof(uwb_mac_hdr_t, pan_id)     == 3u,
    "pan_id must be at byte offset 3");
UWB_STATIC_ASSERT(offsetof(uwb_mac_hdr_t, dest_addr)  == 5u,
    "dest_addr must be at byte offset 5");
UWB_STATIC_ASSERT(offsetof(uwb_mac_hdr_t, src_addr)   == 7u,
    "src_addr must be at byte offset 7");

/* Application header field offsets */
UWB_STATIC_ASSERT(offsetof(uwb_app_hdr_t, mac)        == 0u,
    "app_hdr.mac must be at byte offset 0");
UWB_STATIC_ASSERT(offsetof(uwb_app_hdr_t, frame_type) == 9u,
    "app_hdr.frame_type must be at byte offset 9 (= UWB_OFF_FRAME_TYPE)");

/* UWB_OFF_* constants must agree with the struct layout */
UWB_STATIC_ASSERT(UWB_OFF_FRAME_TYPE == UWB_MAC_HDR_SIZE,
    "UWB_OFF_FRAME_TYPE must equal UWB_MAC_HDR_SIZE");

/* =========================================================================
 * TDoA tag blink frame — UWB_FRAME_TYPE_TAG_BLINK (0x02)
 *
 * Broadcast by a tag in its assigned TDMA slot.  Every anchor hardware-
 * timestamps the frame arrival; the TDoA solver (anchor edge or cloud Lambda)
 * multilaterates the tag position from the cross-anchor timestamp differences.
 *
 * The tag's 16-bit short address is already in the MAC header at
 * UWB_OFF_SRC_ADDR (offset 7) — it is NOT repeated in the payload.
 *
 * Byte layout (absolute offsets from the start of the frame buffer):
 *
 *   Offset  Width  Field            Notes
 *   ------  -----  ---------------  ----------------------------------------
 *      0      2    Frame Control    0x8841 LE — see uwb_mac_hdr_t
 *      2      1    Sequence Number  Per-device MAC counter, wraps 0x00→0xFF
 *      3      2    PAN ID           0x5EEA LE
 *      5      2    Destination Addr 0xFFFF (UWB_ADDR_BROADCAST)
 *      7      2    Source Address   Tag's 16-bit short address, LE
 *      9      1    Frame Type       0x02 (UWB_FRAME_TYPE_TAG_BLINK)
 *     10      2    Blink Count      Per-tag 16-bit counter, LE, wraps
 *                                   0x0000→0xFFFF; used for de-duplication
 *                                   and cross-anchor correlation of the same
 *                                   blink event (see note below)
 *     12      1    Flags            Bit 0 = low battery (UWB_BLINK_FLAG_LOW_BATTERY)
 *                                   Bits 7:1 = reserved, must be 0 on TX
 *   ------  -----
 *     13         Total PSDU (UWB_TAG_BLINK_FRAME_SIZE); DW1000 appends 2-byte
 *                FCS in hardware → 15 bytes total on-air.
 *
 * Blink Count note:
 *   At 75 Hz update rate a 16-bit counter wraps in ~14.5 minutes — well beyond
 *   any single positioning session.  The MAC Sequence Number (1 byte) wraps
 *   in ~3.4 s and is not sufficient for de-dup across a session; the 16-bit
 *   application-layer counter is the reliable correlation key.
 *
 * Airtime note (ADR-001 §superframe, target ~150 µs blink, ~0.5 ms slot):
 *   The 3-byte payload adds only ≈3.5 µs at 6.8 Mbps data rate.  Airtime is
 *   dominated by the preamble:
 *     64-symbol preamble  → ≈104 µs total  (comfortably within 150 µs budget)
 *    128-symbol preamble  → ≈175 µs total  (marginally exceeds budget)
 *   Choose a 64-symbol preamble at 6.8 Mbps for tracking operation.  The
 *   payload size does not push the frame over budget.
 * ========================================================================= */

/* Payload field byte offsets (absolute from frame buffer start) */
#define UWB_OFF_BLINK_COUNT   10u   /* 2 bytes, LE uint16 — blink counter */
#define UWB_OFF_BLINK_FLAGS   12u   /* 1 byte  — status flags             */

/* Total PSDU frame size (no FCS; DW1000 appends 2 bytes in hardware) */
#define UWB_TAG_BLINK_FRAME_SIZE  13u

/* Flags field bit definitions (UWB_OFF_BLINK_FLAGS) */
#define UWB_BLINK_FLAG_LOW_BATTERY  0x01u  /* Bit 0: tag battery below threshold  */
                                           /* Bits 7:1: reserved, must be 0 on TX */

/*
 * uwb_tag_blink_payload_t
 *
 * Application-layer payload of the TDoA tag blink frame: bytes [10..12],
 * 3 bytes.  Follows immediately after uwb_app_hdr_t (bytes [0..9]).
 *
 * All fields are uint8_t / uint8_t[] — no compiler padding is possible and no
 * __attribute__((packed)) is needed or used.
 */
typedef struct {
    uint8_t blink_count[2]; /* Per-tag 16-bit counter, little-endian.
                               Incremented by the tag on every blink TX.
                               Wraps 0x0000 → 0xFFFF.  The solver uses this
                               field to correlate reception reports from
                               different anchors belonging to the same blink
                               event, and to detect duplicate deliveries.     */
    uint8_t flags;          /* Status bit-field.
                               Bit 0: UWB_BLINK_FLAG_LOW_BATTERY (0x01).
                               Bits 7:1: reserved; transmit as 0, ignore on
                               receive.                                        */
} uwb_tag_blink_payload_t;

/*
 * uwb_tag_blink_frame_t
 *
 * Complete TDoA tag blink frame as stored in the radio PSDU buffer
 * (no FCS — the DW1000 appends 2 bytes automatically on TX and strips them
 * on RX before handing the buffer to the host).
 *
 * Usage (receiver side):
 *   1. Read the PSDU into a raw buffer.
 *   2. Verify received length == UWB_TAG_BLINK_FRAME_SIZE.
 *   3. Check hdr.frame_type == UWB_FRAME_TYPE_TAG_BLINK.
 *   4. Cast buffer to const uwb_tag_blink_frame_t * and access fields.
 *
 * Source address (tag short address) is in hdr.mac.src_addr, LE.
 */
typedef struct {
    uwb_app_hdr_t           hdr;     /* Bytes  0–9:  MAC header + frame-type byte */
    uwb_tag_blink_payload_t payload; /* Bytes 10–12: blink payload                */
} uwb_tag_blink_frame_t;

/* --- TDoA blink frame compile-time assertions --------------------------------
 *
 * Verify the payload struct layout and that the UWB_OFF_* constants stay
 * consistent with the actual struct field positions.  Fix the struct if any
 * assertion fires; do not relax or remove the check.
 * --------------------------------------------------------------------------- */

UWB_STATIC_ASSERT(sizeof(uwb_tag_blink_payload_t) == 3u,
    "uwb_tag_blink_payload_t must be exactly 3 bytes");
UWB_STATIC_ASSERT(sizeof(uwb_tag_blink_frame_t) == UWB_TAG_BLINK_FRAME_SIZE,
    "uwb_tag_blink_frame_t must equal UWB_TAG_BLINK_FRAME_SIZE (13 bytes)");

UWB_STATIC_ASSERT(offsetof(uwb_tag_blink_payload_t, blink_count) == 0u,
    "blink_count must be at offset 0 within uwb_tag_blink_payload_t");
UWB_STATIC_ASSERT(offsetof(uwb_tag_blink_payload_t, flags) == 2u,
    "flags must be at offset 2 within uwb_tag_blink_payload_t");
UWB_STATIC_ASSERT(offsetof(uwb_tag_blink_frame_t, payload) == 10u,
    "uwb_tag_blink_frame_t.payload must start at byte offset 10");

/* UWB_OFF_BLINK_* constants must agree with the struct layout */
UWB_STATIC_ASSERT(
    UWB_OFF_BLINK_COUNT == (offsetof(uwb_tag_blink_frame_t, payload) +
                             offsetof(uwb_tag_blink_payload_t, blink_count)),
    "UWB_OFF_BLINK_COUNT must equal the absolute byte offset of blink_count");
UWB_STATIC_ASSERT(
    UWB_OFF_BLINK_FLAGS == (offsetof(uwb_tag_blink_frame_t, payload) +
                             offsetof(uwb_tag_blink_payload_t, flags)),
    "UWB_OFF_BLINK_FLAGS must equal the absolute byte offset of flags");

/* =========================================================================
 * Sync / cycle-start frame — UWB_FRAME_TYPE_SYNC (0x01)
 *
 * Broadcast by the clock-master anchor at the start of every superframe
 * cycle (ADR-001 §Phase 2, ADR-003).  All slave anchors hardware-timestamp
 * this frame's arrival; the timestamp, combined with the master's TX time
 * embedded below and the pre-calibrated master→slave propagation delay,
 * allows each slave to estimate its clock offset and drift relative to the
 * master, then convert all subsequent tag-blink timestamps into the master
 * clock domain before the TDoA solve.
 *
 * The master's 16-bit short address is already in the MAC header at
 * UWB_OFF_SRC_ADDR (offset 7) — it is NOT repeated in the payload.
 * Destination is UWB_ADDR_BROADCAST (0xFFFF).
 *
 * Byte layout (absolute offsets from the start of the frame buffer):
 *
 *   Offset  Width  Field            Notes
 *   ------  -----  ---------------  ----------------------------------------
 *      0      2    Frame Control    0x8841 LE — see uwb_mac_hdr_t
 *      2      1    Sequence Number  Per-device MAC counter, wraps 0x00→0xFF
 *      3      2    PAN ID           0x5EEA LE
 *      5      2    Destination Addr 0xFFFF (UWB_ADDR_BROADCAST)
 *      7      2    Source Address   Master anchor's 16-bit short address, LE
 *      9      1    Frame Type       0x01 (UWB_FRAME_TYPE_SYNC)
 *     10      2    Cycle Seq        LE uint16 superframe/cycle counter, wraps
 *                                   0x0000→0xFFFF (ADR-003: slaves and tags
 *                                   use this to correlate consecutive sync
 *                                   frames for drift estimation and to
 *                                   identify which cycle a blink belongs to)
 *     12      5    Master TX TS     LE uint40 DW1000 hardware TX timestamp of
 *                                   this sync frame in the master's clock
 *                                   domain (ADR-003 "cycle epoch": slave
 *                                   computes offset as
 *                                   T_rx_slave − (master_tx_ts + T_prop),
 *                                   where T_prop = distance_calibrated/c in
 *                                   DW1000 time units from TWR calibration)
 *   ------  -----
 *     17         Total PSDU (UWB_SYNC_FRAME_SIZE); DW1000 appends 2-byte
 *                FCS in hardware → 19 bytes total on-air.
 *
 * Superframe parameters decision (ADR-001 §superframe):
 *   Slot count and guard time are STATICALLY PROVISIONED (configured via the
 *   provisioning tool, not transmitted in-frame).  These values change only
 *   at installation reconfiguration, never per-cycle, so embedding them
 *   in-frame would add wire bytes with no operational benefit.  The sync
 *   frame deliberately carries only the per-cycle information (cycle_seq,
 *   master_tx_ts) that slaves cannot derive from static configuration.
 *
 * Airtime note (ADR-001 §superframe):
 *   The 7-byte payload adds only ≈8 µs at 6.8 Mbps — negligible relative
 *   to the preamble.  A 64-symbol preamble keeps the total well within the
 *   ~150 µs blink budget, same as the tag blink frame.
 * ========================================================================= */

/* Payload field byte offsets (absolute from frame buffer start) */
#define UWB_OFF_SYNC_CYCLE_SEQ     10u   /* 2 bytes, LE uint16 — cycle counter  */
#define UWB_OFF_SYNC_MASTER_TX_TS  12u   /* 5 bytes, LE uint40 — master TX time */

/* Total PSDU frame size (no FCS; DW1000 appends 2 bytes in hardware) */
#define UWB_SYNC_FRAME_SIZE  17u

/*
 * uwb_sync_payload_t
 *
 * Application-layer payload of the sync / cycle-start frame: bytes [10..16],
 * 7 bytes.  Follows immediately after uwb_app_hdr_t (bytes [0..9]).
 *
 * All fields are uint8_t[] — no compiler padding is possible and no
 * __attribute__((packed)) is needed or used.
 */
typedef struct {
    uint8_t cycle_seq[2];    /* Per-superframe 16-bit counter, little-endian.
                                Incremented by the master on every sync TX.
                                Wraps 0x0000 → 0xFFFF.
                                ADR-003: slaves correlate two consecutive sync
                                frames (same cycle_seq step = one cycle) to
                                measure clock-frequency ratio and estimate
                                drift; tags use it to identify which cycle
                                a blink belongs to.                            */
    uint8_t master_tx_ts[5]; /* Master DW1000 hardware TX timestamp of this
                                sync frame, 40-bit, little-endian.
                                ADR-003 "cycle epoch": the master writes the
                                actual scheduled-TX timestamp register value
                                here.  A slave receiving at local time
                                T_rx_slave computes:
                                  offset = T_rx_slave
                                           − (master_tx_ts + T_prop)
                                where T_prop is the master→slave propagation
                                delay in DW1000 time units (distance_calib/c),
                                derived from TWR calibration (ADR-001).
                                This converts the slave's blink timestamps
                                into the master's time domain.                 */
} uwb_sync_payload_t;

/*
 * uwb_sync_frame_t
 *
 * Complete sync / cycle-start frame as stored in the radio PSDU buffer
 * (no FCS — the DW1000 appends 2 bytes automatically on TX and strips them
 * on RX before handing the buffer to the host).
 *
 * Usage (receiver / slave side):
 *   1. Read the PSDU into a raw buffer.
 *   2. Verify received length == UWB_SYNC_FRAME_SIZE.
 *   3. Check hdr.frame_type == UWB_FRAME_TYPE_SYNC.
 *   4. Cast buffer to const uwb_sync_frame_t * and access fields.
 *
 * Master short address is in hdr.mac.src_addr, LE.
 */
typedef struct {
    uwb_app_hdr_t      hdr;     /* Bytes  0–9:  MAC header + frame-type byte */
    uwb_sync_payload_t payload; /* Bytes 10–16: sync payload                 */
} uwb_sync_frame_t;

/* --- Sync frame compile-time assertions -------------------------------------
 *
 * Verify the payload struct layout and that the UWB_OFF_* constants stay
 * consistent with the actual struct field positions.  Fix the struct if any
 * assertion fires; do not relax or remove the check.
 * --------------------------------------------------------------------------- */

UWB_STATIC_ASSERT(sizeof(uwb_sync_payload_t) == 7u,
    "uwb_sync_payload_t must be exactly 7 bytes");
UWB_STATIC_ASSERT(sizeof(uwb_sync_frame_t) == UWB_SYNC_FRAME_SIZE,
    "uwb_sync_frame_t must equal UWB_SYNC_FRAME_SIZE (17 bytes)");

UWB_STATIC_ASSERT(offsetof(uwb_sync_payload_t, cycle_seq) == 0u,
    "cycle_seq must be at offset 0 within uwb_sync_payload_t");
UWB_STATIC_ASSERT(offsetof(uwb_sync_payload_t, master_tx_ts) == 2u,
    "master_tx_ts must be at offset 2 within uwb_sync_payload_t");
UWB_STATIC_ASSERT(offsetof(uwb_sync_frame_t, payload) == 10u,
    "uwb_sync_frame_t.payload must start at byte offset 10");

/* UWB_OFF_SYNC_* constants must agree with the struct layout */
UWB_STATIC_ASSERT(
    UWB_OFF_SYNC_CYCLE_SEQ == (offsetof(uwb_sync_frame_t, payload) +
                                offsetof(uwb_sync_payload_t, cycle_seq)),
    "UWB_OFF_SYNC_CYCLE_SEQ must equal the absolute byte offset of cycle_seq");
UWB_STATIC_ASSERT(
    UWB_OFF_SYNC_MASTER_TX_TS == (offsetof(uwb_sync_frame_t, payload) +
                                   offsetof(uwb_sync_payload_t, master_tx_ts)),
    "UWB_OFF_SYNC_MASTER_TX_TS must equal the absolute byte offset of master_tx_ts");

/* =========================================================================
 * DS-TWR calibration frames — UWB_FRAME_TYPE_TWR_POLL (0x10),
 *                              UWB_FRAME_TYPE_TWR_RESPONSE (0x11),
 *                              UWB_FRAME_TYPE_TWR_FINAL (0x12)
 *
 * Double-sided two-way ranging exchange for Phase 1 calibration (ADR-001).
 * Covers both uses:
 *   • Anchor↔anchor ranging  — builds the inter-anchor distance graph
 *   • Anchor↔reference-tag   — ties the constellation to the court frame
 * Both uses share the same three frame types.  The cloud Lambda distinguishes
 * them from the short addresses in the MAC header and the session's device
 * registration data — no in-frame role flag is needed.
 *
 * ─── Exchange flow ────────────────────────────────────────────────────────
 *
 *   Initiator                              Responder
 *   ─────────                              ─────────
 *   TX POLL ──── timestamp T1 ──────────► RX POLL ──── timestamp T2
 *                                          TX RESPONSE ─ timestamp T3 (sched.)
 *   RX RESPONSE ─ timestamp T4 ◄─────────── [RESPONSE payload carries T2, T3]
 *   TX FINAL ──── timestamp T5 (sched.) ─► RX FINAL ─── timestamp T6 (local)
 *   [FINAL payload carries T1, T4, T5]
 *
 * After the responder receives FINAL it holds all six timestamps:
 *   T1 = poll_tx_ts   (from FINAL payload) — initiator POLL TX
 *   T2 = poll_rx_ts   (measured locally)   — responder POLL RX
 *   T3 = resp_tx_ts   (from RESPONSE payload, also measured locally)
 *                                            — responder RESPONSE TX
 *   T4 = resp_rx_ts   (from FINAL payload) — initiator RESPONSE RX
 *   T5 = final_tx_ts  (from FINAL payload) — initiator FINAL TX
 *   T6 = final_rx_ts  (measured locally)   — responder FINAL RX
 *
 * ─── DS-TWR range formula ─────────────────────────────────────────────────
 *
 * All arithmetic is modulo 2^40 (DW1000 counter wraps in ~17.2 s; the
 * entire exchange takes microseconds, so wrap-around is not a concern).
 *
 *   T_round1 = T4 − T1   // initiator round-trip: POLL TX → RESPONSE RX
 *   T_reply1 = T3 − T2   // responder reply delay: POLL RX → RESPONSE TX
 *   T_round2 = T6 − T3   // responder round-trip: RESPONSE TX → FINAL RX
 *   T_reply2 = T5 − T4   // initiator reply delay: RESPONSE RX → FINAL TX
 *
 *   ToF = (T_round1 × T_round2 − T_reply1 × T_reply2)
 *         / (T_round1 + T_round2 + T_reply1 + T_reply2)
 *
 *   distance [m] = ToF × (c / f_dw1000)
 *               ≈ ToF × 0.004691763 m/tick
 *
 *   where  c          ≈ 299 702 547 m/s
 *          f_dw1000   = 499.2 MHz × 128 ≈ 63.8976 GHz
 *          1 DW1000 tick ≈ 15.65 ps ≈ 4.692 mm
 *
 * Antenna-delay correction (per-device bias, determined separately; out of
 * scope for these frame definitions) is subtracted from the measured ToF
 * after this formula is applied.
 *
 * ─── Scheduled TX timestamps (T3 and T5) ─────────────────────────────────
 *
 * T3 and T5 are the *scheduled* TX times, written into the frame payload
 * before transmission.  The DW1000 guarantees deterministic scheduled TX,
 * so the scheduled time equals the actual TX time.
 *
 * Typical flow:
 *   Responder:  T3 = T2 + UWB_TWR_RESP_REPLY_DELAY_DTU  (chosen constant)
 *               Writes T3 into resp_tx_ts, schedules TX at T3, then transmits.
 *   Initiator:  T5 = T4 + UWB_TWR_FINAL_REPLY_DELAY_DTU (chosen constant)
 *               Writes T5 into final_tx_ts, schedules TX at T5, then transmits.
 *
 * The actual reply delay constants are provisioning-time parameters; they are
 * not transmitted on-air.  Any values that fit within the DW1000 scheduling
 * window are valid (typical: 1–3 ms).
 *
 * ─── Cloud solve (ADR-001, ADR-009) ───────────────────────────────────────
 *
 * After a complete exchange the responder reports T1–T6 (five from the
 * embedded frame fields, T6 from its own DW1000 RX register) to the cloud
 * Lambda via MQTT.  The Lambda computes distances and solves the anchor
 * constellation geometry.  The anchor host software does not need to
 * implement the range formula itself.
 * ========================================================================= */

/* Shared offset: exchange_id appears at byte 10 in all three TWR frames */
#define UWB_OFF_TWR_EXCHANGE_ID         10u  /* 2 bytes, LE uint16              */

/* RESPONSE frame — payload field offsets (absolute from frame buffer start) */
#define UWB_OFF_TWR_RESP_POLL_RX_TS     12u  /* 5 bytes, LE uint40 — T2        */
#define UWB_OFF_TWR_RESP_RESP_TX_TS     17u  /* 5 bytes, LE uint40 — T3 (sched.) */

/* FINAL frame — payload field offsets (absolute from frame buffer start) */
#define UWB_OFF_TWR_FINAL_POLL_TX_TS    12u  /* 5 bytes, LE uint40 — T1        */
#define UWB_OFF_TWR_FINAL_RESP_RX_TS    17u  /* 5 bytes, LE uint40 — T4        */
#define UWB_OFF_TWR_FINAL_FINAL_TX_TS   22u  /* 5 bytes, LE uint40 — T5 (sched.) */

/* Total PSDU sizes (no FCS; DW1000 appends 2 bytes in hardware) */
#define UWB_TWR_POLL_FRAME_SIZE         12u
#define UWB_TWR_RESPONSE_FRAME_SIZE     22u
#define UWB_TWR_FINAL_FRAME_SIZE        27u

/* -------------------------------------------------------------------------
 * POLL frame payload — bytes [10..11], 2 bytes
 *
 * The POLL frame initiates the exchange.  It carries only an exchange_id to
 * correlate the three messages; no timestamps are embedded (T1 is read from
 * the DW1000 TX_TIMESTAMP register after the POLL is transmitted and is
 * then embedded in the subsequent FINAL frame).
 *
 * MAC addressing (unicast):
 *   dest_addr = responder's 16-bit short address
 *   src_addr  = initiator's 16-bit short address
 *
 * Byte layout (absolute offsets):
 *
 *   Offset  Width  Field            Notes
 *   ------  -----  ---------------  ----------------------------------------
 *      0      2    Frame Control    0x8841 LE
 *      2      1    Sequence Number  Per-device MAC counter, wraps 0x00→0xFF
 *      3      2    PAN ID           0x5EEA LE
 *      5      2    Destination Addr Responder 16-bit short address, LE
 *      7      2    Source Address   Initiator 16-bit short address, LE
 *      9      1    Frame Type       0x10 (UWB_FRAME_TYPE_TWR_POLL)
 *     10      2    Exchange ID      Per-initiator uint16 counter, LE; ties
 *                                   POLL / RESPONSE / FINAL into one exchange
 *   ------  -----
 *     12         Total PSDU (UWB_TWR_POLL_FRAME_SIZE)
 * ------------------------------------------------------------------------- */

/*
 * uwb_twr_poll_payload_t — DS-TWR Poll payload (2 bytes)
 *
 * All fields are uint8_t[] — no compiler padding, no __attribute__((packed)).
 */
typedef struct {
    uint8_t exchange_id[2]; /* Per-initiator uint16 exchange counter, LE.
                               Incremented for each new DS-TWR exchange attempt.
                               The cloud Lambda uses this field to correlate the
                               three MQTT messages (POLL/RESPONSE/FINAL) that
                               belong to the same ranging exchange.
                               Range 0x0000–0xFFFF; wraps freely.              */
} uwb_twr_poll_payload_t;

/*
 * uwb_twr_poll_frame_t — complete DS-TWR Poll frame (12 bytes PSDU)
 *
 * Usage (initiator side):
 *   1. Fill hdr (frame_type = UWB_FRAME_TYPE_TWR_POLL, dest/src addresses).
 *   2. Set payload.exchange_id.
 *   3. Schedule or immediate TX; read T1 from DW1000 TX_TIMESTAMP register.
 *   4. Store T1 for later embedding in the FINAL frame.
 */
typedef struct {
    uwb_app_hdr_t          hdr;     /* Bytes  0–9:  MAC header + frame-type 0x10 */
    uwb_twr_poll_payload_t payload; /* Bytes 10–11: exchange_id                  */
} uwb_twr_poll_frame_t;

/* -------------------------------------------------------------------------
 * RESPONSE frame payload — bytes [10..21], 12 bytes
 *
 * The responder embeds its own timestamps so that the initiator and the
 * cloud Lambda can recover T2 (when the POLL arrived) and T3 (when this
 * RESPONSE was sent) without a separate out-of-band report.
 *
 * T3 (resp_tx_ts) is the *scheduled* TX timestamp of this RESPONSE frame.
 * The responder computes T3 = T2 + <reply_delay_dtu> and schedules the TX
 * at T3, writing T3 into the payload before transmission.  Because DW1000
 * scheduled TX is deterministic, the scheduled and actual TX timestamps
 * are identical.
 *
 * MAC addressing (unicast):
 *   dest_addr = initiator's 16-bit short address
 *   src_addr  = responder's 16-bit short address
 *
 * Byte layout (absolute offsets):
 *
 *   Offset  Width  Field            Notes
 *   ------  -----  ---------------  ----------------------------------------
 *      0      2    Frame Control    0x8841 LE
 *      2      1    Sequence Number  Per-device MAC counter, wraps 0x00→0xFF
 *      3      2    PAN ID           0x5EEA LE
 *      5      2    Destination Addr Initiator 16-bit short address, LE
 *      7      2    Source Address   Responder 16-bit short address, LE
 *      9      1    Frame Type       0x11 (UWB_FRAME_TYPE_TWR_RESPONSE)
 *     10      2    Exchange ID      Copied from POLL's exchange_id, LE
 *     12      5    poll_rx_ts (T2)  Responder's DW1000 RX timestamp of the
 *                                   POLL frame, 40-bit LE
 *     17      5    resp_tx_ts (T3)  Responder's DW1000 scheduled TX timestamp
 *                                   of this RESPONSE, 40-bit LE
 *   ------  -----
 *     22         Total PSDU (UWB_TWR_RESPONSE_FRAME_SIZE)
 * ------------------------------------------------------------------------- */

/*
 * uwb_twr_resp_payload_t — DS-TWR Response payload (12 bytes)
 *
 * All fields are uint8_t[] — no compiler padding, no __attribute__((packed)).
 */
typedef struct {
    uint8_t exchange_id[2]; /* Copied from the POLL frame's exchange_id, LE.
                               Lets the initiator match this RESPONSE to the
                               correct pending exchange.                        */
    uint8_t poll_rx_ts[5];  /* Responder's DW1000 RX timestamp of the POLL
                               (T2), 40-bit, little-endian.
                               Read from DW1000 RXTIMESTAMP register.          */
    uint8_t resp_tx_ts[5];  /* Responder's DW1000 scheduled TX timestamp of
                               this RESPONSE (T3), 40-bit, little-endian.
                               Written into payload before TX; equals the
                               actual TX time (DW1000 deterministic sched.).   */
} uwb_twr_resp_payload_t;

/*
 * uwb_twr_resp_frame_t — complete DS-TWR Response frame (22 bytes PSDU)
 *
 * Usage (responder side):
 *   1. Receive POLL; read T2 from DW1000 RXTIMESTAMP.
 *   2. Compute T3 = T2 + reply_delay_dtu.
 *   3. Fill hdr (frame_type = UWB_FRAME_TYPE_TWR_RESPONSE, swap dest/src).
 *   4. Set payload fields; schedule TX at T3.
 */
typedef struct {
    uwb_app_hdr_t          hdr;     /* Bytes  0–9:  MAC header + frame-type 0x11 */
    uwb_twr_resp_payload_t payload; /* Bytes 10–21: exchange_id, T2, T3          */
} uwb_twr_resp_frame_t;

/* -------------------------------------------------------------------------
 * FINAL frame payload — bytes [10..26], 17 bytes
 *
 * The initiator embeds all three of its timestamps, giving the responder
 * (and cloud Lambda) everything needed to compute the range using the
 * DS-TWR formula above.
 *
 * T5 (final_tx_ts) is the *scheduled* TX timestamp of this FINAL frame.
 * The initiator computes T5 = T4 + <reply_delay_dtu> and schedules TX at
 * T5, writing T5 into the payload before transmission.
 *
 * T1 (poll_tx_ts) is read from the DW1000 TX_TIMESTAMP register after the
 * POLL was transmitted and stored locally; it is embedded here.
 *
 * MAC addressing (unicast):
 *   dest_addr = responder's 16-bit short address
 *   src_addr  = initiator's 16-bit short address
 *
 * Byte layout (absolute offsets):
 *
 *   Offset  Width  Field            Notes
 *   ------  -----  ---------------  ----------------------------------------
 *      0      2    Frame Control    0x8841 LE
 *      2      1    Sequence Number  Per-device MAC counter, wraps 0x00→0xFF
 *      3      2    PAN ID           0x5EEA LE
 *      5      2    Destination Addr Responder 16-bit short address, LE
 *      7      2    Source Address   Initiator 16-bit short address, LE
 *      9      1    Frame Type       0x12 (UWB_FRAME_TYPE_TWR_FINAL)
 *     10      2    Exchange ID      Matches POLL's exchange_id, LE
 *     12      5    poll_tx_ts (T1)  Initiator's DW1000 TX timestamp of the
 *                                   POLL, 40-bit LE
 *     17      5    resp_rx_ts (T4)  Initiator's DW1000 RX timestamp of the
 *                                   RESPONSE, 40-bit LE
 *     22      5    final_tx_ts (T5) Initiator's DW1000 scheduled TX timestamp
 *                                   of this FINAL, 40-bit LE
 *   ------  -----
 *     27         Total PSDU (UWB_TWR_FINAL_FRAME_SIZE)
 *
 * After receiving this FINAL frame the responder has T1 (above), T2 and T3
 * (its own; T2 measured locally, T3 sent in RESPONSE), T4 (above), T5
 * (above), and T6 (measured locally from DW1000 RXTIMESTAMP).  All six
 * timestamps are then reported to the cloud Lambda via MQTT.
 * ------------------------------------------------------------------------- */

/*
 * uwb_twr_final_payload_t — DS-TWR Final payload (17 bytes)
 *
 * All fields are uint8_t[] — no compiler padding, no __attribute__((packed)).
 */
typedef struct {
    uint8_t exchange_id[2];  /* Matches POLL frame's exchange_id, LE.
                                Correlates this FINAL with the correct exchange. */
    uint8_t poll_tx_ts[5];   /* Initiator's DW1000 TX timestamp of the POLL
                                (T1), 40-bit, little-endian.
                                Read from DW1000 TX_TIMESTAMP after POLL TX.    */
    uint8_t resp_rx_ts[5];   /* Initiator's DW1000 RX timestamp of the
                                RESPONSE (T4), 40-bit, little-endian.
                                Read from DW1000 RXTIMESTAMP after RESPONSE RX. */
    uint8_t final_tx_ts[5];  /* Initiator's DW1000 scheduled TX timestamp of
                                this FINAL (T5), 40-bit, little-endian.
                                Written into payload before TX; equals the
                                actual TX time (DW1000 deterministic sched.).   */
} uwb_twr_final_payload_t;

/*
 * uwb_twr_final_frame_t — complete DS-TWR Final frame (27 bytes PSDU)
 *
 * Usage (initiator side):
 *   1. Receive RESPONSE; read T4 from DW1000 RXTIMESTAMP.
 *   2. Compute T5 = T4 + reply_delay_dtu.
 *   3. Fill hdr (frame_type = UWB_FRAME_TYPE_TWR_FINAL, original dest/src).
 *   4. Set payload fields (T1 stored from earlier POLL TX, T4 and T5 fresh).
 *   5. Schedule TX at T5.
 *
 * Usage (responder side, after RX):
 *   1. Verify length == UWB_TWR_FINAL_FRAME_SIZE and frame_type == 0x12.
 *   2. Read T6 from DW1000 RXTIMESTAMP.
 *   3. Extract T1, T4, T5 from payload.  Combine with locally held T2, T3, T6.
 *   4. Apply DS-TWR formula (see block comment above) to compute ToF.
 *   5. Report all six timestamps to the cloud Lambda via MQTT.
 */
typedef struct {
    uwb_app_hdr_t           hdr;     /* Bytes  0–9:  MAC header + frame-type 0x12 */
    uwb_twr_final_payload_t payload; /* Bytes 10–26: exchange_id, T1, T4, T5     */
} uwb_twr_final_frame_t;

/* --- DS-TWR frame compile-time assertions ------------------------------------
 *
 * Verify payload struct sizes, field offsets within payloads, payload start
 * positions within complete frame structs, and the agreement between
 * UWB_OFF_TWR_* constants and actual struct field positions.
 *
 * Fix the struct if any assertion fires; do not relax or remove the check.
 * --------------------------------------------------------------------------- */

/* --- Payload sizes --------------------------------------------------------- */
UWB_STATIC_ASSERT(sizeof(uwb_twr_poll_payload_t)  ==  2u,
    "uwb_twr_poll_payload_t must be exactly 2 bytes (exchange_id only)");
UWB_STATIC_ASSERT(sizeof(uwb_twr_resp_payload_t)  == 12u,
    "uwb_twr_resp_payload_t must be exactly 12 bytes (exchange_id + T2 + T3)");
UWB_STATIC_ASSERT(sizeof(uwb_twr_final_payload_t) == 17u,
    "uwb_twr_final_payload_t must be exactly 17 bytes (exchange_id + T1 + T4 + T5)");

/* --- Complete frame sizes -------------------------------------------------- */
UWB_STATIC_ASSERT(sizeof(uwb_twr_poll_frame_t)  == UWB_TWR_POLL_FRAME_SIZE,
    "uwb_twr_poll_frame_t must equal UWB_TWR_POLL_FRAME_SIZE (12 bytes)");
UWB_STATIC_ASSERT(sizeof(uwb_twr_resp_frame_t)  == UWB_TWR_RESPONSE_FRAME_SIZE,
    "uwb_twr_resp_frame_t must equal UWB_TWR_RESPONSE_FRAME_SIZE (22 bytes)");
UWB_STATIC_ASSERT(sizeof(uwb_twr_final_frame_t) == UWB_TWR_FINAL_FRAME_SIZE,
    "uwb_twr_final_frame_t must equal UWB_TWR_FINAL_FRAME_SIZE (27 bytes)");

/* --- Field offsets within payload structs ---------------------------------- */
UWB_STATIC_ASSERT(offsetof(uwb_twr_poll_payload_t,  exchange_id) == 0u,
    "poll payload: exchange_id must be at offset 0");
UWB_STATIC_ASSERT(offsetof(uwb_twr_resp_payload_t,  exchange_id) == 0u,
    "resp payload: exchange_id must be at offset 0");
UWB_STATIC_ASSERT(offsetof(uwb_twr_resp_payload_t,  poll_rx_ts)  == 2u,
    "resp payload: poll_rx_ts must be at offset 2");
UWB_STATIC_ASSERT(offsetof(uwb_twr_resp_payload_t,  resp_tx_ts)  == 7u,
    "resp payload: resp_tx_ts must be at offset 7");
UWB_STATIC_ASSERT(offsetof(uwb_twr_final_payload_t, exchange_id) == 0u,
    "final payload: exchange_id must be at offset 0");
UWB_STATIC_ASSERT(offsetof(uwb_twr_final_payload_t, poll_tx_ts)  == 2u,
    "final payload: poll_tx_ts must be at offset 2");
UWB_STATIC_ASSERT(offsetof(uwb_twr_final_payload_t, resp_rx_ts)  == 7u,
    "final payload: resp_rx_ts must be at offset 7");
UWB_STATIC_ASSERT(offsetof(uwb_twr_final_payload_t, final_tx_ts) == 12u,
    "final payload: final_tx_ts must be at offset 12");

/* --- Payload start offsets within complete frame structs ------------------- */
UWB_STATIC_ASSERT(offsetof(uwb_twr_poll_frame_t,  payload) == 10u,
    "uwb_twr_poll_frame_t.payload must start at byte offset 10");
UWB_STATIC_ASSERT(offsetof(uwb_twr_resp_frame_t,  payload) == 10u,
    "uwb_twr_resp_frame_t.payload must start at byte offset 10");
UWB_STATIC_ASSERT(offsetof(uwb_twr_final_frame_t, payload) == 10u,
    "uwb_twr_final_frame_t.payload must start at byte offset 10");

/* --- UWB_OFF_TWR_* constants vs actual struct positions ------------------- */

/* exchange_id (verified against the POLL frame; same offset in RESP and FINAL) */
UWB_STATIC_ASSERT(
    UWB_OFF_TWR_EXCHANGE_ID == (offsetof(uwb_twr_poll_frame_t, payload) +
                                 offsetof(uwb_twr_poll_payload_t, exchange_id)),
    "UWB_OFF_TWR_EXCHANGE_ID must equal the absolute byte offset of poll exchange_id");

/* RESPONSE payload fields */
UWB_STATIC_ASSERT(
    UWB_OFF_TWR_RESP_POLL_RX_TS == (offsetof(uwb_twr_resp_frame_t, payload) +
                                     offsetof(uwb_twr_resp_payload_t, poll_rx_ts)),
    "UWB_OFF_TWR_RESP_POLL_RX_TS must equal the absolute byte offset of poll_rx_ts");
UWB_STATIC_ASSERT(
    UWB_OFF_TWR_RESP_RESP_TX_TS == (offsetof(uwb_twr_resp_frame_t, payload) +
                                     offsetof(uwb_twr_resp_payload_t, resp_tx_ts)),
    "UWB_OFF_TWR_RESP_RESP_TX_TS must equal the absolute byte offset of resp_tx_ts");

/* FINAL payload fields */
UWB_STATIC_ASSERT(
    UWB_OFF_TWR_FINAL_POLL_TX_TS == (offsetof(uwb_twr_final_frame_t, payload) +
                                      offsetof(uwb_twr_final_payload_t, poll_tx_ts)),
    "UWB_OFF_TWR_FINAL_POLL_TX_TS must equal the absolute byte offset of poll_tx_ts");
UWB_STATIC_ASSERT(
    UWB_OFF_TWR_FINAL_RESP_RX_TS == (offsetof(uwb_twr_final_frame_t, payload) +
                                      offsetof(uwb_twr_final_payload_t, resp_rx_ts)),
    "UWB_OFF_TWR_FINAL_RESP_RX_TS must equal the absolute byte offset of resp_rx_ts");
UWB_STATIC_ASSERT(
    UWB_OFF_TWR_FINAL_FINAL_TX_TS == (offsetof(uwb_twr_final_frame_t, payload) +
                                       offsetof(uwb_twr_final_payload_t, final_tx_ts)),
    "UWB_OFF_TWR_FINAL_FINAL_TX_TS must equal the absolute byte offset of final_tx_ts");

/* =========================================================================
 * Aloha join exchange — protocol description
 *
 * These two frame types implement the contention-based registration flow that
 * assigns every tag a 16-bit short address and a TDMA slot before it can
 * participate in Phase-2 tracking (ADR-001 §Registration/join → Aloha).
 *
 * ─── Tag addressing during join ───────────────────────────────────────────
 *
 * A tag that has not yet been assigned a short address uses:
 *   src_addr  = UWB_ADDR_UNASSIGNED (0xFFFE) in the MAC header
 *   dest_addr = UWB_ADDR_BROADCAST  (0xFFFF) in the MAC header
 *
 * The 64-bit DW1000 extended address (EUI-64) identifying the tag is carried
 * in the JOIN_REQUEST payload because the 16-bit MAC src_addr field cannot
 * carry the full identity at join time.  In the SLOT_ASSIGNMENT response the
 * master echoes this EUI-64 in the payload so the target tag recognises the
 * frame as directed at it; the MAC src_addr carries the master's short address.
 *
 * ─── Exchange flow ────────────────────────────────────────────────────────
 *
 *   Unregistered tag                    Clock-master anchor
 *   ────────────────                    ───────────────────
 *   (optionally listen for SYNC to
 *    confirm network is alive)
 *
 *   TX JOIN_REQUEST ─── (broadcast) ──► RX JOIN_REQUEST
 *     src_addr = 0xFFFE (unassigned)      master selects next free
 *     dest_addr = 0xFFFF (broadcast)      short_addr + slot_idx
 *     payload: EUI-64, capabilities
 *                                        TX SLOT_ASSIGNMENT (broadcast)
 *   RX SLOT_ASSIGNMENT ◄──────────────── src_addr = master short addr
 *     match payload.target_eui64          dest_addr = 0xFFFF (broadcast)
 *     against own EUI-64                  payload: EUI-64, short_addr,
 *                                                  slot_idx, slot_count,
 *                                                  slot_duration_us
 *   store short_addr + slot_idx
 *   begin TDoA blinks in assigned slot
 *
 * ─── Collision handling and retries ──────────────────────────────────────
 *
 * Aloha is contention-based: two tags may transmit JOIN_REQUESTs at the same
 * time, causing a collision that the master receives as an FCS error — it
 * simply discards the frame.  A tag waiting for a SLOT_ASSIGNMENT that matches
 * its own EUI-64 must implement a retry with random backoff:
 *
 *   1. If no matching SLOT_ASSIGNMENT is received within an
 *      implementation-defined timeout (suggested: 3–5 superframe cycles),
 *      the tag retransmits JOIN_REQUEST after a random delay.
 *   2. Exponential backoff (or uniform random) is recommended to resolve
 *      persistent collisions.
 *
 * The exact retry policy (timeout duration, backoff algorithm) is an
 * implementation concern, not part of the wire format.
 *
 * ─── Superframe parameters in SLOT_ASSIGNMENT ────────────────────────────
 *
 * The SYNC frame (0x01) does not carry slot_count or slot_duration (those are
 * statically provisioned for anchors via the provisioning tool — ADR-011, and
 * do not change per-cycle).  However, a dynamically joining tag has no prior
 * knowledge of these parameters.  The SLOT_ASSIGNMENT therefore carries both:
 *   slot_count      — total TDMA slots in the superframe (~24 typical)
 *   slot_duration_us — microseconds per slot (~500 µs typical)
 * These are session constants; the tag uses them to compute its TX delay
 * relative to the SYNC frame:
 *   TX_delay = slot_idx × slot_duration_us  (from the SYNC RX edge)
 *
 * ─── DW1000 identity bridge (ADR-011, ADR-024) ───────────────────────────
 *
 * The EUI-64 in the JOIN_REQUEST payload is the same 64-bit DW1000 extended
 * address that is encoded in the tag's QR code (ADR-011).  When a player
 * scans the tag QR to create a session_participant record (ADR-024), the
 * DW1000 id bridges the radio registration (short_addr ↔ EUI-64) to the
 * player identity in the control plane.
 *
 * ─── Known gap — slot reclamation ────────────────────────────────────────
 *
 * Reclaiming slots from inactive tags (heartbeat / inactivity timeout policy)
 * is deferred per ADR-001 §Remaining implementation details.  No reclamation
 * frame type is defined here.  A future MINOR version bump will add one when
 * the policy is resolved.
 * ========================================================================= */

/* =========================================================================
 * Join request frame — UWB_FRAME_TYPE_JOIN_REQUEST (0x20)
 *
 * Contention-based join sent by an unregistered tag before it has a short
 * address (ADR-001 §Registration/join → Aloha).  The tag uses
 * UWB_ADDR_UNASSIGNED (0xFFFE) in src_addr and broadcasts to
 * UWB_ADDR_BROADCAST (0xFFFF) because it does not yet know the master's
 * address (or any anchor's address).  The 64-bit DW1000 extended address
 * (EUI-64) is carried in the payload — it is the tag's permanent hardware
 * identity and the key the master uses for the short-address binding.
 *
 * Byte layout (absolute offsets from the start of the frame buffer):
 *
 *   Offset  Width  Field            Notes
 *   ------  -----  ---------------  ----------------------------------------
 *      0      2    Frame Control    0x8841 LE — see uwb_mac_hdr_t
 *      2      1    Sequence Number  Per-device MAC counter, wraps 0x00→0xFF
 *      3      2    PAN ID           0x5EEA LE
 *      5      2    Destination Addr 0xFFFF (UWB_ADDR_BROADCAST) — no target
 *                                   short address known at join time
 *      7      2    Source Address   0xFFFE (UWB_ADDR_UNASSIGNED) — tag has
 *                                   no assigned short address yet
 *      9      1    Frame Type       0x20 (UWB_FRAME_TYPE_JOIN_REQUEST)
 *     10      8    EUI-64           Tag's 64-bit DW1000 extended address, LE.
 *                                   Permanent hardware identity; bridges to the
 *                                   QR-scan player binding (ADR-011, ADR-024)
 *     18      1    Capabilities     Role/capability flags:
 *                                     Bit 0: UWB_JOIN_CAP_ROLE_REFERENCE —
 *                                       tag is acting as a reference tag for
 *                                       Phase-1 calibration (placed at a known
 *                                       court landmark, ADR-001 §reference-tags)
 *                                     Bits 7:1: reserved, must be 0 on TX
 *   ------  -----
 *     19         Total PSDU (UWB_JOIN_REQUEST_FRAME_SIZE); DW1000 appends
 *                2-byte FCS in hardware → 21 bytes total on-air.
 * ========================================================================= */

/* Payload field byte offsets (absolute from frame buffer start) */
#define UWB_OFF_JOIN_EUI64          10u   /* 8 bytes, LE uint64 — DW1000 EUI-64   */
#define UWB_OFF_JOIN_CAPABILITIES   18u   /* 1 byte  — role/capability flags       */

/* Total PSDU frame size (no FCS; DW1000 appends 2 bytes in hardware) */
#define UWB_JOIN_REQUEST_FRAME_SIZE  19u

/* Capabilities field bit definitions (UWB_OFF_JOIN_CAPABILITIES) */
#define UWB_JOIN_CAP_ROLE_REFERENCE  0x01u /* Bit 0: tag is a calibration reference
                                              tag placed at a known court landmark
                                              (ADR-001 §reference-tags sub-decision).
                                              0 = mobile tracking tag (default).
                                              Bits 7:1: reserved, must be 0 on TX.  */

/*
 * uwb_join_request_payload_t
 *
 * Application-layer payload of the join request frame: bytes [10..18],
 * 9 bytes.  Follows immediately after uwb_app_hdr_t (bytes [0..9]).
 *
 * All fields are uint8_t / uint8_t[] — no compiler padding is possible and no
 * __attribute__((packed)) is needed or used.
 */
typedef struct {
    uint8_t eui64[8];     /* Tag's 64-bit DW1000 extended address (EUI-64),
                             little-endian (LSB at eui64[0]).
                             This is the tag's permanent hardware identity,
                             identical to the value encoded in the tag's QR
                             code (ADR-011).  The master stores the mapping
                             EUI-64 → assigned short_addr + slot_idx so that
                             the cloud control plane can later resolve a
                             session_participant QR scan to a tracking stream
                             (ADR-024).
                             Used as the correlation key in the subsequent
                             SLOT_ASSIGNMENT (target_eui64 field).           */
    uint8_t capabilities; /* Role/capability bit-field.
                             Bit 0: UWB_JOIN_CAP_ROLE_REFERENCE (0x01) —
                               tag is acting as a reference tag placed at a
                               known court landmark for Phase-1 calibration
                               (ADR-001 §reference-tags sub-decision).
                               0 = mobile tracking tag (normal case).
                             Bits 7:1: reserved; transmit as 0, ignore on
                             receive.                                         */
} uwb_join_request_payload_t;

/*
 * uwb_join_request_frame_t
 *
 * Complete join request frame as stored in the radio PSDU buffer
 * (no FCS — the DW1000 appends 2 bytes automatically on TX and strips them
 * on RX before handing the buffer to the host).
 *
 * Usage (tag / transmitter side):
 *   1. Fill hdr: frame_type = UWB_FRAME_TYPE_JOIN_REQUEST,
 *                dest_addr = UWB_ADDR_BROADCAST (0xFFFF),
 *                src_addr  = UWB_ADDR_UNASSIGNED (0xFFFE).
 *   2. Set payload.eui64 from the DW1000 EUI-64 register.
 *   3. Set payload.capabilities (UWB_JOIN_CAP_ROLE_REFERENCE if applicable).
 *   4. Transmit; wait for a SLOT_ASSIGNMENT whose target_eui64 matches.
 *   5. If no matching assignment within timeout → retry with random backoff.
 *
 * Usage (master / receiver side):
 *   1. Verify received length == UWB_JOIN_REQUEST_FRAME_SIZE.
 *   2. Check hdr.frame_type == UWB_FRAME_TYPE_JOIN_REQUEST.
 *   3. Cast buffer to const uwb_join_request_frame_t * and read payload.
 *   4. Allocate a free short_addr and slot_idx; send SLOT_ASSIGNMENT.
 */
typedef struct {
    uwb_app_hdr_t              hdr;     /* Bytes  0–9:  MAC header + frame-type byte */
    uwb_join_request_payload_t payload; /* Bytes 10–18: EUI-64 + capabilities        */
} uwb_join_request_frame_t;

/* --- Join request frame compile-time assertions -----------------------------
 *
 * Verify the payload struct layout and that the UWB_OFF_* constants stay
 * consistent with the actual struct field positions.  Fix the struct if any
 * assertion fires; do not relax or remove the check.
 * --------------------------------------------------------------------------- */

UWB_STATIC_ASSERT(sizeof(uwb_join_request_payload_t) == 9u,
    "uwb_join_request_payload_t must be exactly 9 bytes (EUI-64 + capabilities)");
UWB_STATIC_ASSERT(sizeof(uwb_join_request_frame_t) == UWB_JOIN_REQUEST_FRAME_SIZE,
    "uwb_join_request_frame_t must equal UWB_JOIN_REQUEST_FRAME_SIZE (19 bytes)");

UWB_STATIC_ASSERT(offsetof(uwb_join_request_payload_t, eui64) == 0u,
    "eui64 must be at offset 0 within uwb_join_request_payload_t");
UWB_STATIC_ASSERT(offsetof(uwb_join_request_payload_t, capabilities) == 8u,
    "capabilities must be at offset 8 within uwb_join_request_payload_t");
UWB_STATIC_ASSERT(offsetof(uwb_join_request_frame_t, payload) == 10u,
    "uwb_join_request_frame_t.payload must start at byte offset 10");

/* UWB_OFF_JOIN_* constants must agree with the struct layout */
UWB_STATIC_ASSERT(
    UWB_OFF_JOIN_EUI64 == (offsetof(uwb_join_request_frame_t, payload) +
                            offsetof(uwb_join_request_payload_t, eui64)),
    "UWB_OFF_JOIN_EUI64 must equal the absolute byte offset of eui64");
UWB_STATIC_ASSERT(
    UWB_OFF_JOIN_CAPABILITIES == (offsetof(uwb_join_request_frame_t, payload) +
                                   offsetof(uwb_join_request_payload_t, capabilities)),
    "UWB_OFF_JOIN_CAPABILITIES must equal the absolute byte offset of capabilities");

/* =========================================================================
 * Slot / address assignment frame — UWB_FRAME_TYPE_SLOT_ASSIGNMENT (0x21)
 *
 * Sent by the clock-master anchor in response to a JOIN_REQUEST.  The frame
 * binds the requesting tag's 64-bit DW1000 EUI-64 to an assigned 16-bit short
 * address and TDMA slot index.  It also conveys the superframe parameters the
 * tag needs to schedule its blinks (slot_count and slot_duration_us), because
 * a dynamically joining tag has no prior knowledge of these values (the SYNC
 * frame does not carry them — they are statically provisioned for anchors).
 *
 * Addressing:
 *   The target tag has no short address yet, so the master cannot unicast.
 *   The frame is broadcast (dest_addr = UWB_ADDR_BROADCAST); every listening
 *   tag (registered or not) checks the target_eui64 payload field against its
 *   own EUI-64 and ignores the frame if it does not match.
 *
 * Byte layout (absolute offsets from the start of the frame buffer):
 *
 *   Offset  Width  Field            Notes
 *   ------  -----  ---------------  ----------------------------------------
 *      0      2    Frame Control    0x8841 LE — see uwb_mac_hdr_t
 *      2      1    Sequence Number  Per-device MAC counter, wraps 0x00→0xFF
 *      3      2    PAN ID           0x5EEA LE
 *      5      2    Destination Addr 0xFFFF (UWB_ADDR_BROADCAST) — tag has no
 *                                   short address yet; target identified by
 *                                   target_eui64 in the payload
 *      7      2    Source Address   Master anchor's 16-bit short address, LE
 *      9      1    Frame Type       0x21 (UWB_FRAME_TYPE_SLOT_ASSIGNMENT)
 *     10      8    Target EUI-64    64-bit DW1000 EUI-64 of the tag being
 *                                   assigned, LE. Tags match this against
 *                                   their own EUI-64 to claim the assignment.
 *     18      2    Short Address    Assigned 16-bit short address, LE.
 *                                   The tag uses this in MAC src_addr of all
 *                                   subsequent frames (blinks, TWR, etc.)
 *     20      1    Slot Index       Assigned TDMA slot index, 0-based.
 *                                   Tag TX delay from SYNC RX edge:
 *                                     delay = slot_idx × slot_duration_us
 *                                   Range: 0 to (slot_count − 1)
 *     21      1    Slot Count       Total number of TDMA slots in the
 *                                   superframe (provisioned session constant,
 *                                   ~24 typical; see ADR-001 §superframe)
 *     22      2    Slot Duration    Slot duration in microseconds, LE uint16.
 *                                   Includes tag blink airtime + guard time.
 *                                   (~500 µs typical, covering ~150 µs
 *                                   blink + guard; see ADR-001 §superframe)
 *   ------  -----
 *     24         Total PSDU (UWB_SLOT_ASSIGNMENT_FRAME_SIZE); DW1000 appends
 *                2-byte FCS in hardware → 26 bytes total on-air.
 * ========================================================================= */

/* Payload field byte offsets (absolute from frame buffer start) */
#define UWB_OFF_SLOT_TARGET_EUI64    10u   /* 8 bytes, LE uint64 — target tag EUI-64  */
#define UWB_OFF_SLOT_SHORT_ADDR      18u   /* 2 bytes, LE uint16 — assigned short addr */
#define UWB_OFF_SLOT_IDX             20u   /* 1 byte  — assigned TDMA slot index       */
#define UWB_OFF_SLOT_COUNT           21u   /* 1 byte  — total slots in superframe      */
#define UWB_OFF_SLOT_DURATION_US     22u   /* 2 bytes, LE uint16 — slot duration (µs)  */

/* Total PSDU frame size (no FCS; DW1000 appends 2 bytes in hardware) */
#define UWB_SLOT_ASSIGNMENT_FRAME_SIZE  24u

/*
 * uwb_slot_assignment_payload_t
 *
 * Application-layer payload of the slot / address assignment frame:
 * bytes [10..23], 14 bytes.  Follows immediately after uwb_app_hdr_t
 * (bytes [0..9]).
 *
 * All fields are uint8_t / uint8_t[] — no compiler padding is possible and no
 * __attribute__((packed)) is needed or used.
 */
typedef struct {
    uint8_t target_eui64[8];  /* 64-bit DW1000 EUI-64 of the tag being assigned,
                                  little-endian (LSB at target_eui64[0]).
                                  Every listening tag compares this field against
                                  its own EUI-64; only the matching tag adopts
                                  the short_addr and slot_idx below.
                                  Copied from the JOIN_REQUEST payload.eui64.    */
    uint8_t short_addr[2];    /* Assigned 16-bit short address, little-endian.
                                  The tag uses this as MAC src_addr in all
                                  subsequent frames (TAG_BLINK, TWR, etc.).
                                  The master records the EUI-64 → short_addr
                                  binding for the duration of the session.       */
    uint8_t slot_idx;         /* Assigned TDMA slot index, 0-based.
                                  The tag transmits its blink after:
                                    delay = slot_idx × slot_duration_us
                                  measured from the leading edge of the SYNC
                                  frame reception (ADR-001 §superframe).
                                  Range: 0 to (slot_count − 1).                 */
    uint8_t slot_count;       /* Total number of TDMA slots in the superframe.
                                  Session constant (~24 typical, per ADR-001
                                  §superframe capacity sub-decision).
                                  Conveyed here because a joining tag has no
                                  prior knowledge of this value (it is not
                                  carried in the SYNC frame; see ADR-011 static
                                  provisioning rationale in uwb_sync_payload_t). */
    uint8_t slot_duration_us[2]; /* Slot duration in microseconds, little-endian
                                    uint16.  Includes tag blink airtime plus guard
                                    time (~500 µs typical at ADR-001 target
                                    ~75 Hz / 24 slots).  The tag uses this with
                                    slot_idx to compute its scheduled TX delay
                                    after each SYNC reception.
                                    Range: 1–65535 µs; 0 is invalid.            */
} uwb_slot_assignment_payload_t;

/*
 * uwb_slot_assignment_frame_t
 *
 * Complete slot / address assignment frame as stored in the radio PSDU buffer
 * (no FCS — the DW1000 appends 2 bytes automatically on TX and strips them
 * on RX before handing the buffer to the host).
 *
 * Usage (master / transmitter side):
 *   1. Fill hdr: frame_type = UWB_FRAME_TYPE_SLOT_ASSIGNMENT,
 *                dest_addr = UWB_ADDR_BROADCAST (0xFFFF),
 *                src_addr  = master's own short address.
 *   2. Set payload.target_eui64 from the JOIN_REQUEST payload.
 *   3. Allocate and set payload.short_addr (next free address).
 *   4. Allocate and set payload.slot_idx (next free slot).
 *   5. Set payload.slot_count and payload.slot_duration_us from provisioned
 *      session configuration.
 *   6. Transmit; record the EUI-64 → short_addr + slot_idx binding.
 *
 * Usage (tag / receiver side):
 *   1. Verify received length == UWB_SLOT_ASSIGNMENT_FRAME_SIZE.
 *   2. Check hdr.frame_type == UWB_FRAME_TYPE_SLOT_ASSIGNMENT.
 *   3. Cast buffer to const uwb_slot_assignment_frame_t *.
 *   4. Compare payload.target_eui64 with own EUI-64; discard if no match.
 *   5. Adopt payload.short_addr as own MAC address for all future frames.
 *   6. Store payload.slot_idx, payload.slot_count, payload.slot_duration_us
 *      for superframe scheduling.
 *   7. Begin TDoA blinks in the assigned slot on the next SYNC reception.
 */
typedef struct {
    uwb_app_hdr_t                 hdr;     /* Bytes  0–9:  MAC header + frame-type byte */
    uwb_slot_assignment_payload_t payload; /* Bytes 10–23: target EUI-64, binding, params */
} uwb_slot_assignment_frame_t;

/* --- Slot assignment frame compile-time assertions --------------------------
 *
 * Verify the payload struct layout and that the UWB_OFF_* constants stay
 * consistent with the actual struct field positions.  Fix the struct if any
 * assertion fires; do not relax or remove the check.
 * --------------------------------------------------------------------------- */

UWB_STATIC_ASSERT(sizeof(uwb_slot_assignment_payload_t) == 14u,
    "uwb_slot_assignment_payload_t must be exactly 14 bytes");
UWB_STATIC_ASSERT(sizeof(uwb_slot_assignment_frame_t) == UWB_SLOT_ASSIGNMENT_FRAME_SIZE,
    "uwb_slot_assignment_frame_t must equal UWB_SLOT_ASSIGNMENT_FRAME_SIZE (24 bytes)");

UWB_STATIC_ASSERT(offsetof(uwb_slot_assignment_payload_t, target_eui64)    ==  0u,
    "target_eui64 must be at offset 0 within uwb_slot_assignment_payload_t");
UWB_STATIC_ASSERT(offsetof(uwb_slot_assignment_payload_t, short_addr)      ==  8u,
    "short_addr must be at offset 8 within uwb_slot_assignment_payload_t");
UWB_STATIC_ASSERT(offsetof(uwb_slot_assignment_payload_t, slot_idx)        == 10u,
    "slot_idx must be at offset 10 within uwb_slot_assignment_payload_t");
UWB_STATIC_ASSERT(offsetof(uwb_slot_assignment_payload_t, slot_count)      == 11u,
    "slot_count must be at offset 11 within uwb_slot_assignment_payload_t");
UWB_STATIC_ASSERT(offsetof(uwb_slot_assignment_payload_t, slot_duration_us) == 12u,
    "slot_duration_us must be at offset 12 within uwb_slot_assignment_payload_t");
UWB_STATIC_ASSERT(offsetof(uwb_slot_assignment_frame_t,  payload)          == 10u,
    "uwb_slot_assignment_frame_t.payload must start at byte offset 10");

/* UWB_OFF_SLOT_* constants must agree with the struct layout */
UWB_STATIC_ASSERT(
    UWB_OFF_SLOT_TARGET_EUI64 == (offsetof(uwb_slot_assignment_frame_t, payload) +
                                   offsetof(uwb_slot_assignment_payload_t, target_eui64)),
    "UWB_OFF_SLOT_TARGET_EUI64 must equal the absolute byte offset of target_eui64");
UWB_STATIC_ASSERT(
    UWB_OFF_SLOT_SHORT_ADDR == (offsetof(uwb_slot_assignment_frame_t, payload) +
                                 offsetof(uwb_slot_assignment_payload_t, short_addr)),
    "UWB_OFF_SLOT_SHORT_ADDR must equal the absolute byte offset of short_addr");
UWB_STATIC_ASSERT(
    UWB_OFF_SLOT_IDX == (offsetof(uwb_slot_assignment_frame_t, payload) +
                          offsetof(uwb_slot_assignment_payload_t, slot_idx)),
    "UWB_OFF_SLOT_IDX must equal the absolute byte offset of slot_idx");
UWB_STATIC_ASSERT(
    UWB_OFF_SLOT_COUNT == (offsetof(uwb_slot_assignment_frame_t, payload) +
                            offsetof(uwb_slot_assignment_payload_t, slot_count)),
    "UWB_OFF_SLOT_COUNT must equal the absolute byte offset of slot_count");
UWB_STATIC_ASSERT(
    UWB_OFF_SLOT_DURATION_US == (offsetof(uwb_slot_assignment_frame_t, payload) +
                                  offsetof(uwb_slot_assignment_payload_t, slot_duration_us)),
    "UWB_OFF_SLOT_DURATION_US must equal the absolute byte offset of slot_duration_us");

#ifdef __cplusplus
}
#endif

#endif /* UWB_FRAMES_H */
