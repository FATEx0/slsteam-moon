
#include "steammessages_contentserverdirectory.pb.h"

#include <algorithm>

#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/extension_set.h>
#include <google/protobuf/wire_format_lite.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
#include <google/protobuf/port_def.inc>

PROTOBUF_PRAGMA_INIT_SEG
constexpr CContentServerDirectory_GetManifestRequestCode_Request::CContentServerDirectory_GetManifestRequestCode_Request(
  ::PROTOBUF_NAMESPACE_ID::internal::ConstantInitialized)
  : app_branch_(&::PROTOBUF_NAMESPACE_ID::internal::fixed_address_empty_string)
  , branch_password_hash_(&::PROTOBUF_NAMESPACE_ID::internal::fixed_address_empty_string)
  , app_id_(0u)
  , depot_id_(0u)
  , manifest_id_(PROTOBUF_ULONGLONG(0)){}
struct CContentServerDirectory_GetManifestRequestCode_RequestDefaultTypeInternal {
  constexpr CContentServerDirectory_GetManifestRequestCode_RequestDefaultTypeInternal()
    : _instance(::PROTOBUF_NAMESPACE_ID::internal::ConstantInitialized{}) {}
  ~CContentServerDirectory_GetManifestRequestCode_RequestDefaultTypeInternal() {}
  union {
    CContentServerDirectory_GetManifestRequestCode_Request _instance;
  };
};
PROTOBUF_ATTRIBUTE_NO_DESTROY PROTOBUF_CONSTINIT CContentServerDirectory_GetManifestRequestCode_RequestDefaultTypeInternal _CContentServerDirectory_GetManifestRequestCode_Request_default_instance_;
constexpr CContentServerDirectory_GetManifestRequestCode_Response::CContentServerDirectory_GetManifestRequestCode_Response(
  ::PROTOBUF_NAMESPACE_ID::internal::ConstantInitialized)
  : manifest_request_code_(PROTOBUF_ULONGLONG(0)){}
struct CContentServerDirectory_GetManifestRequestCode_ResponseDefaultTypeInternal {
  constexpr CContentServerDirectory_GetManifestRequestCode_ResponseDefaultTypeInternal()
    : _instance(::PROTOBUF_NAMESPACE_ID::internal::ConstantInitialized{}) {}
  ~CContentServerDirectory_GetManifestRequestCode_ResponseDefaultTypeInternal() {}
  union {
    CContentServerDirectory_GetManifestRequestCode_Response _instance;
  };
};
PROTOBUF_ATTRIBUTE_NO_DESTROY PROTOBUF_CONSTINIT CContentServerDirectory_GetManifestRequestCode_ResponseDefaultTypeInternal _CContentServerDirectory_GetManifestRequestCode_Response_default_instance_;


class CContentServerDirectory_GetManifestRequestCode_Request::_Internal {
 public:
  using HasBits = decltype(std::declval<CContentServerDirectory_GetManifestRequestCode_Request>()._has_bits_);
  static void set_has_app_id(HasBits* has_bits) {
    (*has_bits)[0] |= 4u;
  }
  static void set_has_depot_id(HasBits* has_bits) {
    (*has_bits)[0] |= 8u;
  }
  static void set_has_manifest_id(HasBits* has_bits) {
    (*has_bits)[0] |= 16u;
  }
  static void set_has_app_branch(HasBits* has_bits) {
    (*has_bits)[0] |= 1u;
  }
  static void set_has_branch_password_hash(HasBits* has_bits) {
    (*has_bits)[0] |= 2u;
  }
};

CContentServerDirectory_GetManifestRequestCode_Request::CContentServerDirectory_GetManifestRequestCode_Request(::PROTOBUF_NAMESPACE_ID::Arena* arena)
  : ::PROTOBUF_NAMESPACE_ID::MessageLite(arena) {
  SharedCtor();
  RegisterArenaDtor(arena);
}
CContentServerDirectory_GetManifestRequestCode_Request::CContentServerDirectory_GetManifestRequestCode_Request(const CContentServerDirectory_GetManifestRequestCode_Request& from)
  : ::PROTOBUF_NAMESPACE_ID::MessageLite(),
      _has_bits_(from._has_bits_) {
  _internal_metadata_.MergeFrom<std::string>(from._internal_metadata_);
  app_branch_.UnsafeSetDefault(&::PROTOBUF_NAMESPACE_ID::internal::GetEmptyStringAlreadyInited());
  if (from._internal_has_app_branch()) {
    app_branch_.Set(::PROTOBUF_NAMESPACE_ID::internal::ArenaStringPtr::EmptyDefault{}, from._internal_app_branch(), 
      GetArena());
  }
  branch_password_hash_.UnsafeSetDefault(&::PROTOBUF_NAMESPACE_ID::internal::GetEmptyStringAlreadyInited());
  if (from._internal_has_branch_password_hash()) {
    branch_password_hash_.Set(::PROTOBUF_NAMESPACE_ID::internal::ArenaStringPtr::EmptyDefault{}, from._internal_branch_password_hash(), 
      GetArena());
  }
  ::memcpy(&app_id_, &from.app_id_,
    static_cast<size_t>(reinterpret_cast<char*>(&manifest_id_) -
    reinterpret_cast<char*>(&app_id_)) + sizeof(manifest_id_));
}

void CContentServerDirectory_GetManifestRequestCode_Request::SharedCtor() {
app_branch_.UnsafeSetDefault(&::PROTOBUF_NAMESPACE_ID::internal::GetEmptyStringAlreadyInited());
branch_password_hash_.UnsafeSetDefault(&::PROTOBUF_NAMESPACE_ID::internal::GetEmptyStringAlreadyInited());
::memset(reinterpret_cast<char*>(this) + static_cast<size_t>(
    reinterpret_cast<char*>(&app_id_) - reinterpret_cast<char*>(this)),
    0, static_cast<size_t>(reinterpret_cast<char*>(&manifest_id_) -
    reinterpret_cast<char*>(&app_id_)) + sizeof(manifest_id_));
}

CContentServerDirectory_GetManifestRequestCode_Request::~CContentServerDirectory_GetManifestRequestCode_Request() {
  SharedDtor();
  _internal_metadata_.Delete<std::string>();
}

void CContentServerDirectory_GetManifestRequestCode_Request::SharedDtor() {
  GOOGLE_DCHECK(GetArena() == nullptr);
  app_branch_.DestroyNoArena(&::PROTOBUF_NAMESPACE_ID::internal::GetEmptyStringAlreadyInited());
  branch_password_hash_.DestroyNoArena(&::PROTOBUF_NAMESPACE_ID::internal::GetEmptyStringAlreadyInited());
}

void CContentServerDirectory_GetManifestRequestCode_Request::ArenaDtor(void* object) {
  CContentServerDirectory_GetManifestRequestCode_Request* _this = reinterpret_cast< CContentServerDirectory_GetManifestRequestCode_Request* >(object);
  (void)_this;
}
void CContentServerDirectory_GetManifestRequestCode_Request::RegisterArenaDtor(::PROTOBUF_NAMESPACE_ID::Arena*) {
}
void CContentServerDirectory_GetManifestRequestCode_Request::SetCachedSize(int size) const {
  _cached_size_.Set(size);
}

void CContentServerDirectory_GetManifestRequestCode_Request::Clear() {
  ::PROTOBUF_NAMESPACE_ID::uint32 cached_has_bits = 0;
  (void) cached_has_bits;

  cached_has_bits = _has_bits_[0];
  if (cached_has_bits & 0x00000003u) {
    if (cached_has_bits & 0x00000001u) {
      app_branch_.ClearNonDefaultToEmpty();
    }
    if (cached_has_bits & 0x00000002u) {
      branch_password_hash_.ClearNonDefaultToEmpty();
    }
  }
  if (cached_has_bits & 0x0000001cu) {
    ::memset(&app_id_, 0, static_cast<size_t>(
        reinterpret_cast<char*>(&manifest_id_) -
        reinterpret_cast<char*>(&app_id_)) + sizeof(manifest_id_));
  }
  _has_bits_.Clear();
  _internal_metadata_.Clear<std::string>();
}

const char* CContentServerDirectory_GetManifestRequestCode_Request::_InternalParse(const char* ptr, ::PROTOBUF_NAMESPACE_ID::internal::ParseContext* ctx) {
#define CHK_(x) if (PROTOBUF_PREDICT_FALSE(!(x))) goto failure
  _Internal::HasBits has_bits{};
  while (!ctx->Done(&ptr)) {
    ::PROTOBUF_NAMESPACE_ID::uint32 tag;
    ptr = ::PROTOBUF_NAMESPACE_ID::internal::ReadTag(ptr, &tag);
    CHK_(ptr);
    switch (tag >> 3) {
      case 1:
        if (PROTOBUF_PREDICT_TRUE(static_cast<::PROTOBUF_NAMESPACE_ID::uint8>(tag) == 8)) {
          _Internal::set_has_app_id(&has_bits);
          app_id_ = ::PROTOBUF_NAMESPACE_ID::internal::ReadVarint32(&ptr);
          CHK_(ptr);
        } else goto handle_unusual;
        continue;
      case 2:
        if (PROTOBUF_PREDICT_TRUE(static_cast<::PROTOBUF_NAMESPACE_ID::uint8>(tag) == 16)) {
          _Internal::set_has_depot_id(&has_bits);
          depot_id_ = ::PROTOBUF_NAMESPACE_ID::internal::ReadVarint32(&ptr);
          CHK_(ptr);
        } else goto handle_unusual;
        continue;
      case 3:
        if (PROTOBUF_PREDICT_TRUE(static_cast<::PROTOBUF_NAMESPACE_ID::uint8>(tag) == 24)) {
          _Internal::set_has_manifest_id(&has_bits);
          manifest_id_ = ::PROTOBUF_NAMESPACE_ID::internal::ReadVarint64(&ptr);
          CHK_(ptr);
        } else goto handle_unusual;
        continue;
      case 4:
        if (PROTOBUF_PREDICT_TRUE(static_cast<::PROTOBUF_NAMESPACE_ID::uint8>(tag) == 34)) {
          auto str = _internal_mutable_app_branch();
          ptr = ::PROTOBUF_NAMESPACE_ID::internal::InlineGreedyStringParser(str, ptr, ctx);
          CHK_(ptr);
        } else goto handle_unusual;
        continue;
      case 5:
        if (PROTOBUF_PREDICT_TRUE(static_cast<::PROTOBUF_NAMESPACE_ID::uint8>(tag) == 42)) {
          auto str = _internal_mutable_branch_password_hash();
          ptr = ::PROTOBUF_NAMESPACE_ID::internal::InlineGreedyStringParser(str, ptr, ctx);
          CHK_(ptr);
        } else goto handle_unusual;
        continue;
      default: {
      handle_unusual:
        if ((tag & 7) == 4 || tag == 0) {
          ctx->SetLastTag(tag);
          goto success;
        }
        ptr = UnknownFieldParse(tag,
            _internal_metadata_.mutable_unknown_fields<std::string>(),
            ptr, ctx);
        CHK_(ptr != nullptr);
        continue;
      }
    }  // switch
  }  // while
success:
  _has_bits_.Or(has_bits);
  return ptr;
failure:
  ptr = nullptr;
  goto success;
#undef CHK_
}

::PROTOBUF_NAMESPACE_ID::uint8* CContentServerDirectory_GetManifestRequestCode_Request::_InternalSerialize(
    ::PROTOBUF_NAMESPACE_ID::uint8* target, ::PROTOBUF_NAMESPACE_ID::io::EpsCopyOutputStream* stream) const {
  ::PROTOBUF_NAMESPACE_ID::uint32 cached_has_bits = 0;
  (void) cached_has_bits;

  cached_has_bits = _has_bits_[0];
  if (cached_has_bits & 0x00000004u) {
    target = stream->EnsureSpace(target);
    target = ::PROTOBUF_NAMESPACE_ID::internal::WireFormatLite::WriteUInt32ToArray(1, this->_internal_app_id(), target);
  }

  if (cached_has_bits & 0x00000008u) {
    target = stream->EnsureSpace(target);
    target = ::PROTOBUF_NAMESPACE_ID::internal::WireFormatLite::WriteUInt32ToArray(2, this->_internal_depot_id(), target);
  }

  if (cached_has_bits & 0x00000010u) {
    target = stream->EnsureSpace(target);
    target = ::PROTOBUF_NAMESPACE_ID::internal::WireFormatLite::WriteUInt64ToArray(3, this->_internal_manifest_id(), target);
  }

  if (cached_has_bits & 0x00000001u) {
    target = stream->WriteStringMaybeAliased(
        4, this->_internal_app_branch(), target);
  }

  if (cached_has_bits & 0x00000002u) {
    target = stream->WriteStringMaybeAliased(
        5, this->_internal_branch_password_hash(), target);
  }

  if (PROTOBUF_PREDICT_FALSE(_internal_metadata_.have_unknown_fields())) {
    target = stream->WriteRaw(_internal_metadata_.unknown_fields<std::string>(::PROTOBUF_NAMESPACE_ID::internal::GetEmptyString).data(),
        static_cast<int>(_internal_metadata_.unknown_fields<std::string>(::PROTOBUF_NAMESPACE_ID::internal::GetEmptyString).size()), target);
  }
  return target;
}

size_t CContentServerDirectory_GetManifestRequestCode_Request::ByteSizeLong() const {
  size_t total_size = 0;

  ::PROTOBUF_NAMESPACE_ID::uint32 cached_has_bits = 0;
  (void) cached_has_bits;

  cached_has_bits = _has_bits_[0];
  if (cached_has_bits & 0x0000001fu) {
    if (cached_has_bits & 0x00000001u) {
      total_size += 1 +
        ::PROTOBUF_NAMESPACE_ID::internal::WireFormatLite::StringSize(
          this->_internal_app_branch());
    }

    if (cached_has_bits & 0x00000002u) {
      total_size += 1 +
        ::PROTOBUF_NAMESPACE_ID::internal::WireFormatLite::StringSize(
          this->_internal_branch_password_hash());
    }

    if (cached_has_bits & 0x00000004u) {
      total_size += 1 +
        ::PROTOBUF_NAMESPACE_ID::internal::WireFormatLite::UInt32Size(
          this->_internal_app_id());
    }

    if (cached_has_bits & 0x00000008u) {
      total_size += 1 +
        ::PROTOBUF_NAMESPACE_ID::internal::WireFormatLite::UInt32Size(
          this->_internal_depot_id());
    }

    if (cached_has_bits & 0x00000010u) {
      total_size += 1 +
        ::PROTOBUF_NAMESPACE_ID::internal::WireFormatLite::UInt64Size(
          this->_internal_manifest_id());
    }

  }
  if (PROTOBUF_PREDICT_FALSE(_internal_metadata_.have_unknown_fields())) {
    total_size += _internal_metadata_.unknown_fields<std::string>(::PROTOBUF_NAMESPACE_ID::internal::GetEmptyString).size();
  }
  int cached_size = ::PROTOBUF_NAMESPACE_ID::internal::ToCachedSize(total_size);
  SetCachedSize(cached_size);
  return total_size;
}

void CContentServerDirectory_GetManifestRequestCode_Request::CheckTypeAndMergeFrom(
    const ::PROTOBUF_NAMESPACE_ID::MessageLite& from) {
  MergeFrom(*::PROTOBUF_NAMESPACE_ID::internal::DownCast<const CContentServerDirectory_GetManifestRequestCode_Request*>(
      &from));
}

void CContentServerDirectory_GetManifestRequestCode_Request::MergeFrom(const CContentServerDirectory_GetManifestRequestCode_Request& from) {
  GOOGLE_DCHECK_NE(&from, this);
  _internal_metadata_.MergeFrom<std::string>(from._internal_metadata_);
  ::PROTOBUF_NAMESPACE_ID::uint32 cached_has_bits = 0;
  (void) cached_has_bits;

  cached_has_bits = from._has_bits_[0];
  if (cached_has_bits & 0x0000001fu) {
    if (cached_has_bits & 0x00000001u) {
      _internal_set_app_branch(from._internal_app_branch());
    }
    if (cached_has_bits & 0x00000002u) {
      _internal_set_branch_password_hash(from._internal_branch_password_hash());
    }
    if (cached_has_bits & 0x00000004u) {
      app_id_ = from.app_id_;
    }
    if (cached_has_bits & 0x00000008u) {
      depot_id_ = from.depot_id_;
    }
    if (cached_has_bits & 0x00000010u) {
      manifest_id_ = from.manifest_id_;
    }
    _has_bits_[0] |= cached_has_bits;
  }
}

void CContentServerDirectory_GetManifestRequestCode_Request::CopyFrom(const CContentServerDirectory_GetManifestRequestCode_Request& from) {
  if (&from == this) return;
  Clear();
  MergeFrom(from);
}

bool CContentServerDirectory_GetManifestRequestCode_Request::IsInitialized() const {
  return true;
}

void CContentServerDirectory_GetManifestRequestCode_Request::InternalSwap(CContentServerDirectory_GetManifestRequestCode_Request* other) {
  using std::swap;
  _internal_metadata_.Swap<std::string>(&other->_internal_metadata_);
  swap(_has_bits_[0], other->_has_bits_[0]);
  app_branch_.Swap(&other->app_branch_, &::PROTOBUF_NAMESPACE_ID::internal::GetEmptyStringAlreadyInited(), GetArena());
  branch_password_hash_.Swap(&other->branch_password_hash_, &::PROTOBUF_NAMESPACE_ID::internal::GetEmptyStringAlreadyInited(), GetArena());
  ::PROTOBUF_NAMESPACE_ID::internal::memswap<
      PROTOBUF_FIELD_OFFSET(CContentServerDirectory_GetManifestRequestCode_Request, manifest_id_)
      + sizeof(CContentServerDirectory_GetManifestRequestCode_Request::manifest_id_)
      - PROTOBUF_FIELD_OFFSET(CContentServerDirectory_GetManifestRequestCode_Request, app_id_)>(
          reinterpret_cast<char*>(&app_id_),
          reinterpret_cast<char*>(&other->app_id_));
}

std::string CContentServerDirectory_GetManifestRequestCode_Request::GetTypeName() const {
  return "CContentServerDirectory_GetManifestRequestCode_Request";
}



class CContentServerDirectory_GetManifestRequestCode_Response::_Internal {
 public:
  using HasBits = decltype(std::declval<CContentServerDirectory_GetManifestRequestCode_Response>()._has_bits_);
  static void set_has_manifest_request_code(HasBits* has_bits) {
    (*has_bits)[0] |= 1u;
  }
};

CContentServerDirectory_GetManifestRequestCode_Response::CContentServerDirectory_GetManifestRequestCode_Response(::PROTOBUF_NAMESPACE_ID::Arena* arena)
  : ::PROTOBUF_NAMESPACE_ID::MessageLite(arena) {
  SharedCtor();
  RegisterArenaDtor(arena);
}
CContentServerDirectory_GetManifestRequestCode_Response::CContentServerDirectory_GetManifestRequestCode_Response(const CContentServerDirectory_GetManifestRequestCode_Response& from)
  : ::PROTOBUF_NAMESPACE_ID::MessageLite(),
      _has_bits_(from._has_bits_) {
  _internal_metadata_.MergeFrom<std::string>(from._internal_metadata_);
  manifest_request_code_ = from.manifest_request_code_;
}

void CContentServerDirectory_GetManifestRequestCode_Response::SharedCtor() {
manifest_request_code_ = PROTOBUF_ULONGLONG(0);
}

CContentServerDirectory_GetManifestRequestCode_Response::~CContentServerDirectory_GetManifestRequestCode_Response() {
  SharedDtor();
  _internal_metadata_.Delete<std::string>();
}

void CContentServerDirectory_GetManifestRequestCode_Response::SharedDtor() {
  GOOGLE_DCHECK(GetArena() == nullptr);
}

void CContentServerDirectory_GetManifestRequestCode_Response::ArenaDtor(void* object) {
  CContentServerDirectory_GetManifestRequestCode_Response* _this = reinterpret_cast< CContentServerDirectory_GetManifestRequestCode_Response* >(object);
  (void)_this;
}
void CContentServerDirectory_GetManifestRequestCode_Response::RegisterArenaDtor(::PROTOBUF_NAMESPACE_ID::Arena*) {
}
void CContentServerDirectory_GetManifestRequestCode_Response::SetCachedSize(int size) const {
  _cached_size_.Set(size);
}

void CContentServerDirectory_GetManifestRequestCode_Response::Clear() {
  ::PROTOBUF_NAMESPACE_ID::uint32 cached_has_bits = 0;
  (void) cached_has_bits;

  manifest_request_code_ = PROTOBUF_ULONGLONG(0);
  _has_bits_.Clear();
  _internal_metadata_.Clear<std::string>();
}

const char* CContentServerDirectory_GetManifestRequestCode_Response::_InternalParse(const char* ptr, ::PROTOBUF_NAMESPACE_ID::internal::ParseContext* ctx) {
#define CHK_(x) if (PROTOBUF_PREDICT_FALSE(!(x))) goto failure
  _Internal::HasBits has_bits{};
  while (!ctx->Done(&ptr)) {
    ::PROTOBUF_NAMESPACE_ID::uint32 tag;
    ptr = ::PROTOBUF_NAMESPACE_ID::internal::ReadTag(ptr, &tag);
    CHK_(ptr);
    switch (tag >> 3) {
      case 1:
        if (PROTOBUF_PREDICT_TRUE(static_cast<::PROTOBUF_NAMESPACE_ID::uint8>(tag) == 8)) {
          _Internal::set_has_manifest_request_code(&has_bits);
          manifest_request_code_ = ::PROTOBUF_NAMESPACE_ID::internal::ReadVarint64(&ptr);
          CHK_(ptr);
        } else goto handle_unusual;
        continue;
      default: {
      handle_unusual:
        if ((tag & 7) == 4 || tag == 0) {
          ctx->SetLastTag(tag);
          goto success;
        }
        ptr = UnknownFieldParse(tag,
            _internal_metadata_.mutable_unknown_fields<std::string>(),
            ptr, ctx);
        CHK_(ptr != nullptr);
        continue;
      }
    }  // switch
  }  // while
success:
  _has_bits_.Or(has_bits);
  return ptr;
failure:
  ptr = nullptr;
  goto success;
#undef CHK_
}

::PROTOBUF_NAMESPACE_ID::uint8* CContentServerDirectory_GetManifestRequestCode_Response::_InternalSerialize(
    ::PROTOBUF_NAMESPACE_ID::uint8* target, ::PROTOBUF_NAMESPACE_ID::io::EpsCopyOutputStream* stream) const {
  ::PROTOBUF_NAMESPACE_ID::uint32 cached_has_bits = 0;
  (void) cached_has_bits;

  cached_has_bits = _has_bits_[0];
  if (cached_has_bits & 0x00000001u) {
    target = stream->EnsureSpace(target);
    target = ::PROTOBUF_NAMESPACE_ID::internal::WireFormatLite::WriteUInt64ToArray(1, this->_internal_manifest_request_code(), target);
  }

  if (PROTOBUF_PREDICT_FALSE(_internal_metadata_.have_unknown_fields())) {
    target = stream->WriteRaw(_internal_metadata_.unknown_fields<std::string>(::PROTOBUF_NAMESPACE_ID::internal::GetEmptyString).data(),
        static_cast<int>(_internal_metadata_.unknown_fields<std::string>(::PROTOBUF_NAMESPACE_ID::internal::GetEmptyString).size()), target);
  }
  return target;
}

size_t CContentServerDirectory_GetManifestRequestCode_Response::ByteSizeLong() const {
  size_t total_size = 0;

  ::PROTOBUF_NAMESPACE_ID::uint32 cached_has_bits = 0;
  (void) cached_has_bits;

  cached_has_bits = _has_bits_[0];
  if (cached_has_bits & 0x00000001u) {
    total_size += 1 +
      ::PROTOBUF_NAMESPACE_ID::internal::WireFormatLite::UInt64Size(
        this->_internal_manifest_request_code());
  }

  if (PROTOBUF_PREDICT_FALSE(_internal_metadata_.have_unknown_fields())) {
    total_size += _internal_metadata_.unknown_fields<std::string>(::PROTOBUF_NAMESPACE_ID::internal::GetEmptyString).size();
  }
  int cached_size = ::PROTOBUF_NAMESPACE_ID::internal::ToCachedSize(total_size);
  SetCachedSize(cached_size);
  return total_size;
}

void CContentServerDirectory_GetManifestRequestCode_Response::CheckTypeAndMergeFrom(
    const ::PROTOBUF_NAMESPACE_ID::MessageLite& from) {
  MergeFrom(*::PROTOBUF_NAMESPACE_ID::internal::DownCast<const CContentServerDirectory_GetManifestRequestCode_Response*>(
      &from));
}

void CContentServerDirectory_GetManifestRequestCode_Response::MergeFrom(const CContentServerDirectory_GetManifestRequestCode_Response& from) {
  GOOGLE_DCHECK_NE(&from, this);
  _internal_metadata_.MergeFrom<std::string>(from._internal_metadata_);
  ::PROTOBUF_NAMESPACE_ID::uint32 cached_has_bits = 0;
  (void) cached_has_bits;

  if (from._internal_has_manifest_request_code()) {
    _internal_set_manifest_request_code(from._internal_manifest_request_code());
  }
}

void CContentServerDirectory_GetManifestRequestCode_Response::CopyFrom(const CContentServerDirectory_GetManifestRequestCode_Response& from) {
  if (&from == this) return;
  Clear();
  MergeFrom(from);
}

bool CContentServerDirectory_GetManifestRequestCode_Response::IsInitialized() const {
  return true;
}

void CContentServerDirectory_GetManifestRequestCode_Response::InternalSwap(CContentServerDirectory_GetManifestRequestCode_Response* other) {
  using std::swap;
  _internal_metadata_.Swap<std::string>(&other->_internal_metadata_);
  swap(_has_bits_[0], other->_has_bits_[0]);
  swap(manifest_request_code_, other->manifest_request_code_);
}

std::string CContentServerDirectory_GetManifestRequestCode_Response::GetTypeName() const {
  return "CContentServerDirectory_GetManifestRequestCode_Response";
}


PROTOBUF_NAMESPACE_OPEN
template<> PROTOBUF_NOINLINE ::CContentServerDirectory_GetManifestRequestCode_Request* Arena::CreateMaybeMessage< ::CContentServerDirectory_GetManifestRequestCode_Request >(Arena* arena) {
  return Arena::CreateMessageInternal< ::CContentServerDirectory_GetManifestRequestCode_Request >(arena);
}
template<> PROTOBUF_NOINLINE ::CContentServerDirectory_GetManifestRequestCode_Response* Arena::CreateMaybeMessage< ::CContentServerDirectory_GetManifestRequestCode_Response >(Arena* arena) {
  return Arena::CreateMessageInternal< ::CContentServerDirectory_GetManifestRequestCode_Response >(arena);
}
PROTOBUF_NAMESPACE_CLOSE

#include <google/protobuf/port_undef.inc>
