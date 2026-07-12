#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <vector>

#include <rex/system/xobject.h>
#include <rex/types.h>

namespace rex::system {

struct XNKID {
  uint8_t ab[8];
};
static_assert_size(XNKID, 0x8);

struct XNKEY {
  uint8_t ab[16];
};
static_assert_size(XNKEY, 0x10);

struct XSESSION_XNADDR {
  rex::be<uint32_t> ina;
  rex::be<uint32_t> inaOnline;
  rex::be<uint16_t> wPortOnline;
  uint8_t abEnet[6];
  uint8_t abOnline[20];
};
static_assert_size(XSESSION_XNADDR, 0x24);

struct XSESSION_INFO {
  XNKID sessionID;
  XSESSION_XNADDR hostAddress;
  XNKEY keyExchangeKey;
};
static_assert_size(XSESSION_INFO, 0x3C);

enum class X_USER_DATA_TYPE : uint8_t {
  CONTEXT = 0,
  INT32 = 1,
  INT64 = 2,
  DOUBLE = 3,
  WSTRING = 4,
  FLOAT = 5,
  BINARY = 6,
  DATETIME = 7,
  UNSET = 0xFF,
};

struct X_USER_DATA_UNION {
  union {
    rex::be<int32_t> s32;
    rex::be<int64_t> s64;
    rex::be<uint32_t> u32;
    rex::be<double> f64;
    struct {
      rex::be<uint32_t> size;
      rex::be<uint32_t> ptr;
    } unicode;
    rex::be<float> f32;
    struct {
      rex::be<uint32_t> size;
      rex::be<uint32_t> ptr;
    } binary;
    rex::be<uint64_t> filetime;
  };
};
static_assert_size(X_USER_DATA_UNION, 0x8);

struct alignas(8) X_USER_DATA {
  X_USER_DATA_TYPE type;
  X_USER_DATA_UNION data;
};
static_assert_size(X_USER_DATA, 0x10);

struct XUSER_CONTEXT {
  rex::be<uint32_t> context_id;
  rex::be<uint32_t> value;
};
static_assert_size(XUSER_CONTEXT, 0x8);

struct XUSER_PROPERTY {
  rex::be<uint32_t> property_id;
  X_USER_DATA data;
};
static_assert_size(XUSER_PROPERTY, 0x18);

struct XSESSION_SEARCHRESULT {
  XSESSION_INFO info;
  rex::be<uint32_t> open_public_slots;
  rex::be<uint32_t> open_private_slots;
  rex::be<uint32_t> filled_public_slots;
  rex::be<uint32_t> filled_private_slots;
  rex::be<uint32_t> properties_count;
  rex::be<uint32_t> contexts_count;
  rex::be<uint32_t> properties_ptr;
  rex::be<uint32_t> contexts_ptr;
};
static_assert_size(XSESSION_SEARCHRESULT, 0x5C);

struct XSESSION_SEARCHRESULT_HEADER {
  rex::be<uint32_t> search_results_count;
  rex::be<uint32_t> search_results_ptr;
};
static_assert_size(XSESSION_SEARCHRESULT_HEADER, 0x8);

struct XSESSION_LOCAL_DETAILS {
  rex::be<uint32_t> user_index_host;
  rex::be<uint32_t> game_type;
  rex::be<uint32_t> game_mode;
  rex::be<uint32_t> flags;
  rex::be<uint32_t> max_public_slots;
  rex::be<uint32_t> max_private_slots;
  rex::be<uint32_t> available_public_slots;
  rex::be<uint32_t> available_private_slots;
  rex::be<uint32_t> actual_member_count;
  rex::be<uint32_t> returned_member_count;
  rex::be<uint32_t> state;
  rex::be<uint64_t> nonce;
  XSESSION_INFO session_info;
  XNKID arbitration_id;
  rex::be<uint32_t> session_members_ptr;
};
static_assert_size(XSESSION_LOCAL_DETAILS, 0x80);

struct X_KSESSION {
  rex::be<uint32_t> handle;
};
static_assert_size(X_KSESSION, 4);

class XSession : public XObject {
 public:
  static const XObject::Type kObjectType = XObject::Type::Session;

  explicit XSession(KernelState* kernel_state);
  ~XSession() override;

  X_STATUS Initialize();

  X_STATUS CreateHostSession(uint32_t flags, uint32_t public_slots, uint32_t private_slots,
                             XSESSION_INFO* session_info, uint64_t* nonce);
  X_STATUS DeleteSession();
  X_STATUS JoinLocal(uint32_t user_count, uint32_t user_index_array,
                     uint32_t private_slots_array);
  X_STATUS JoinRemote(uint32_t user_count, const std::vector<bool>& private_slots);
  X_STATUS Leave(uint32_t user_count);
  X_STATUS Modify(uint32_t flags, uint32_t public_slots, uint32_t private_slots);
  X_STATUS Start();
  X_STATUS End();
  X_STATUS GetDetails(XSESSION_LOCAL_DETAILS* details, uint32_t details_size) const;

  const XSESSION_INFO& session_info() const { return session_info_; }
  uint64_t session_id() const;
  uint64_t nonce() const { return nonce_; }
  uint32_t flags() const { return flags_; }
  uint32_t max_public_slots() const { return max_public_slots_; }
  uint32_t max_private_slots() const { return max_private_slots_; }
  uint32_t available_public_slots() const { return available_public_slots_; }
  uint32_t available_private_slots() const { return available_private_slots_; }

 private:
  uint32_t flags_ = 0;
  uint32_t max_public_slots_ = 0;
  uint32_t max_private_slots_ = 0;
  uint32_t available_public_slots_ = 0;
  uint32_t available_private_slots_ = 0;
  uint32_t actual_member_count_ = 0;
  uint32_t state_ = 0;
  uint64_t nonce_ = 0;
  XSESSION_INFO session_info_ = {};
};

uint64_t XnkidToUint64(const XNKID& session_id);
void Uint64ToXnkid(uint64_t value, XNKID* session_id);

}  // namespace rex::system
