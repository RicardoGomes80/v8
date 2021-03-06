// Copyright 2016 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/snapshot/serializer.h"

#include "src/assembler-inl.h"
#include "src/deoptimizer.h"
#include "src/heap/heap-inl.h"
#include "src/macro-assembler.h"
#include "src/snapshot/natives.h"

namespace v8 {
namespace internal {

Serializer::Serializer(Isolate* isolate)
    : isolate_(isolate),
      external_reference_encoder_(isolate),
      root_index_map_(isolate),
      recursion_depth_(0),
      code_address_map_(NULL),
      num_maps_(0),
      large_objects_total_size_(0),
      seen_large_objects_index_(0),
      seen_backing_stores_index_(1) {
  // The serializer is meant to be used only to generate initial heap images
  // from a context in which there is only one isolate.
  for (int i = 0; i < kNumberOfPreallocatedSpaces; i++) {
    pending_chunk_[i] = 0;
    max_chunk_size_[i] = static_cast<uint32_t>(
        MemoryAllocator::PageAreaSize(static_cast<AllocationSpace>(i)));
  }

#ifdef OBJECT_PRINT
  if (FLAG_serialization_statistics) {
    instance_type_count_ = NewArray<int>(kInstanceTypes);
    instance_type_size_ = NewArray<size_t>(kInstanceTypes);
    for (int i = 0; i < kInstanceTypes; i++) {
      instance_type_count_[i] = 0;
      instance_type_size_[i] = 0;
    }
  } else {
    instance_type_count_ = NULL;
    instance_type_size_ = NULL;
  }
#endif  // OBJECT_PRINT
}

Serializer::~Serializer() {
  if (code_address_map_ != NULL) delete code_address_map_;
#ifdef OBJECT_PRINT
  if (instance_type_count_ != NULL) {
    DeleteArray(instance_type_count_);
    DeleteArray(instance_type_size_);
  }
#endif  // OBJECT_PRINT
}

#ifdef OBJECT_PRINT
void Serializer::CountInstanceType(Map* map, int size) {
  int instance_type = map->instance_type();
  instance_type_count_[instance_type]++;
  instance_type_size_[instance_type] += size;
}
#endif  // OBJECT_PRINT

void Serializer::OutputStatistics(const char* name) {
  if (!FLAG_serialization_statistics) return;
  PrintF("%s:\n", name);
  PrintF("  Spaces (bytes):\n");
  for (int space = 0; space < kNumberOfSpaces; space++) {
    PrintF("%16s", AllocationSpaceName(static_cast<AllocationSpace>(space)));
  }
  PrintF("\n");
  for (int space = 0; space < kNumberOfPreallocatedSpaces; space++) {
    size_t s = pending_chunk_[space];
    for (uint32_t chunk_size : completed_chunks_[space]) s += chunk_size;
    PrintF("%16" PRIuS, s);
  }
  PrintF("%16d", num_maps_ * Map::kSize);
  PrintF("%16d\n", large_objects_total_size_);
#ifdef OBJECT_PRINT
  PrintF("  Instance types (count and bytes):\n");
#define PRINT_INSTANCE_TYPE(Name)                                 \
  if (instance_type_count_[Name]) {                               \
    PrintF("%10d %10" PRIuS "  %s\n", instance_type_count_[Name], \
           instance_type_size_[Name], #Name);                     \
  }
  INSTANCE_TYPE_LIST(PRINT_INSTANCE_TYPE)
#undef PRINT_INSTANCE_TYPE
  PrintF("\n");
#endif  // OBJECT_PRINT
}

void Serializer::SerializeDeferredObjects() {
  while (!deferred_objects_.empty()) {
    HeapObject* obj = deferred_objects_.back();
    deferred_objects_.pop_back();
    ObjectSerializer obj_serializer(this, obj, &sink_, kPlain, kStartOfObject);
    obj_serializer.SerializeDeferred();
  }
  sink_.Put(kSynchronize, "Finished with deferred objects");
}

bool Serializer::MustBeDeferred(HeapObject* object) { return false; }

void Serializer::VisitRootPointers(Root root, Object** start, Object** end) {
  for (Object** current = start; current < end; current++) {
    if ((*current)->IsSmi()) {
      PutSmi(Smi::cast(*current));
    } else {
      SerializeObject(HeapObject::cast(*current), kPlain, kStartOfObject, 0);
    }
  }
}

void Serializer::EncodeReservations(
    std::vector<SerializedData::Reservation>* out) const {
  for (int i = 0; i < kNumberOfPreallocatedSpaces; i++) {
    for (size_t j = 0; j < completed_chunks_[i].size(); j++) {
      out->push_back(SerializedData::Reservation(completed_chunks_[i][j]));
    }

    if (pending_chunk_[i] > 0 || completed_chunks_[i].size() == 0) {
      out->push_back(SerializedData::Reservation(pending_chunk_[i]));
    }
    out->back().mark_as_last();
  }
  out->push_back(SerializedData::Reservation(num_maps_ * Map::kSize));
  out->back().mark_as_last();
  out->push_back(SerializedData::Reservation(large_objects_total_size_));
  out->back().mark_as_last();
}

#ifdef DEBUG
bool Serializer::BackReferenceIsAlreadyAllocated(
    SerializerReference reference) {
  DCHECK(reference.is_back_reference());
  AllocationSpace space = reference.space();
  if (space == LO_SPACE) {
    return reference.large_object_index() < seen_large_objects_index_;
  } else if (space == MAP_SPACE) {
    return reference.map_index() < num_maps_;
  } else {
    size_t chunk_index = reference.chunk_index();
    if (chunk_index == completed_chunks_[space].size()) {
      return reference.chunk_offset() < pending_chunk_[space];
    } else {
      return chunk_index < completed_chunks_[space].size() &&
             reference.chunk_offset() < completed_chunks_[space][chunk_index];
    }
  }
}

void Serializer::PrintStack() {
  for (const auto o : stack_) {
    o->Print();
    PrintF("\n");
  }
}
#endif  // DEBUG

bool Serializer::SerializeHotObject(HeapObject* obj, HowToCode how_to_code,
                                    WhereToPoint where_to_point, int skip) {
  if (how_to_code != kPlain || where_to_point != kStartOfObject) return false;
  // Encode a reference to a hot object by its index in the working set.
  int index = hot_objects_.Find(obj);
  if (index == HotObjectsList::kNotFound) return false;
  DCHECK(index >= 0 && index < kNumberOfHotObjects);
  if (FLAG_trace_serializer) {
    PrintF(" Encoding hot object %d:", index);
    obj->ShortPrint();
    PrintF("\n");
  }
  if (skip != 0) {
    sink_.Put(kHotObjectWithSkip + index, "HotObjectWithSkip");
    sink_.PutInt(skip, "HotObjectSkipDistance");
  } else {
    sink_.Put(kHotObject + index, "HotObject");
  }
  return true;
}
bool Serializer::SerializeBackReference(HeapObject* obj, HowToCode how_to_code,
                                        WhereToPoint where_to_point, int skip) {
  SerializerReference reference = reference_map_.Lookup(obj);
  if (!reference.is_valid()) return false;
  // Encode the location of an already deserialized object in order to write
  // its location into a later object.  We can encode the location as an
  // offset fromthe start of the deserialized objects or as an offset
  // backwards from thecurrent allocation pointer.
  if (reference.is_attached_reference()) {
    FlushSkip(skip);
    if (FLAG_trace_serializer) {
      PrintF(" Encoding attached reference %d\n",
             reference.attached_reference_index());
    }
    PutAttachedReference(reference, how_to_code, where_to_point);
  } else {
    DCHECK(reference.is_back_reference());
    if (FLAG_trace_serializer) {
      PrintF(" Encoding back reference to: ");
      obj->ShortPrint();
      PrintF("\n");
    }

    PutAlignmentPrefix(obj);
    AllocationSpace space = reference.space();
    if (skip == 0) {
      sink_.Put(kBackref + how_to_code + where_to_point + space, "BackRef");
    } else {
      sink_.Put(kBackrefWithSkip + how_to_code + where_to_point + space,
                "BackRefWithSkip");
      sink_.PutInt(skip, "BackRefSkipDistance");
    }
    PutBackReference(obj, reference);
  }
  return true;
}

bool Serializer::SerializeBuiltinReference(
    HeapObject* obj, HowToCode how_to_code, WhereToPoint where_to_point,
    int skip, BuiltinReferenceSerializationMode mode) {
  if (!obj->IsCode()) return false;

  Code* code = Code::cast(obj);
  int builtin_index = code->builtin_index();
  if (builtin_index < 0) return false;

  DCHECK((how_to_code == kPlain && where_to_point == kStartOfObject) ||
         (how_to_code == kFromCode));
  DCHECK_LT(builtin_index, Builtins::builtin_count);
  DCHECK_LE(0, builtin_index);

  if (mode == kCanonicalizeCompileLazy &&
      code->is_interpreter_trampoline_builtin()) {
    builtin_index = static_cast<int>(Builtins::kCompileLazy);
  }

  if (FLAG_trace_serializer) {
    PrintF(" Encoding builtin reference: %s\n",
           isolate()->builtins()->name(builtin_index));
  }

  FlushSkip(skip);
  sink_.Put(kBuiltin + how_to_code + where_to_point, "Builtin");
  sink_.PutInt(builtin_index, "builtin_index");

  return true;
}

void Serializer::PutRoot(int root_index, HeapObject* object,
                         SerializerDeserializer::HowToCode how_to_code,
                         SerializerDeserializer::WhereToPoint where_to_point,
                         int skip) {
  if (FLAG_trace_serializer) {
    PrintF(" Encoding root %d:", root_index);
    object->ShortPrint();
    PrintF("\n");
  }

  // Assert that the first 32 root array items are a conscious choice. They are
  // chosen so that the most common ones can be encoded more efficiently.
  STATIC_ASSERT(Heap::kEmptyDescriptorArrayRootIndex ==
                kNumberOfRootArrayConstants - 1);

  if (how_to_code == kPlain && where_to_point == kStartOfObject &&
      root_index < kNumberOfRootArrayConstants &&
      !isolate()->heap()->InNewSpace(object)) {
    if (skip == 0) {
      sink_.Put(kRootArrayConstants + root_index, "RootConstant");
    } else {
      sink_.Put(kRootArrayConstantsWithSkip + root_index, "RootConstant");
      sink_.PutInt(skip, "SkipInPutRoot");
    }
  } else {
    FlushSkip(skip);
    sink_.Put(kRootArray + how_to_code + where_to_point, "RootSerialization");
    sink_.PutInt(root_index, "root_index");
    hot_objects_.Add(object);
  }
}

void Serializer::PutSmi(Smi* smi) {
  sink_.Put(kOnePointerRawData, "Smi");
  byte* bytes = reinterpret_cast<byte*>(&smi);
  for (int i = 0; i < kPointerSize; i++) sink_.Put(bytes[i], "Byte");
}

void Serializer::PutBackReference(HeapObject* object,
                                  SerializerReference reference) {
  DCHECK(BackReferenceIsAlreadyAllocated(reference));
  sink_.PutInt(reference.back_reference(), "BackRefValue");
  hot_objects_.Add(object);
}

void Serializer::PutAttachedReference(SerializerReference reference,
                                      HowToCode how_to_code,
                                      WhereToPoint where_to_point) {
  DCHECK(reference.is_attached_reference());
  DCHECK((how_to_code == kPlain && where_to_point == kStartOfObject) ||
         (how_to_code == kFromCode && where_to_point == kStartOfObject) ||
         (how_to_code == kFromCode && where_to_point == kInnerPointer));
  sink_.Put(kAttachedReference + how_to_code + where_to_point, "AttachedRef");
  sink_.PutInt(reference.attached_reference_index(), "AttachedRefIndex");
}

int Serializer::PutAlignmentPrefix(HeapObject* object) {
  AllocationAlignment alignment = object->RequiredAlignment();
  if (alignment != kWordAligned) {
    DCHECK(1 <= alignment && alignment <= 3);
    byte prefix = (kAlignmentPrefix - 1) + alignment;
    sink_.Put(prefix, "Alignment");
    return Heap::GetMaximumFillToAlign(alignment);
  }
  return 0;
}

SerializerReference Serializer::AllocateOffHeapBackingStore() {
  DCHECK_NE(0, seen_backing_stores_index_);
  return SerializerReference::OffHeapBackingStoreReference(
      seen_backing_stores_index_++);
}

SerializerReference Serializer::AllocateLargeObject(int size) {
  // Large objects are allocated one-by-one when deserializing. We do not
  // have to keep track of multiple chunks.
  large_objects_total_size_ += size;
  return SerializerReference::LargeObjectReference(seen_large_objects_index_++);
}

SerializerReference Serializer::AllocateMap() {
  // Maps are allocated one-by-one when deserializing.
  return SerializerReference::MapReference(num_maps_++);
}

SerializerReference Serializer::Allocate(AllocationSpace space, int size) {
  DCHECK(space >= 0 && space < kNumberOfPreallocatedSpaces);
  DCHECK(size > 0 && size <= static_cast<int>(max_chunk_size(space)));
  uint32_t new_chunk_size = pending_chunk_[space] + size;
  if (new_chunk_size > max_chunk_size(space)) {
    // The new chunk size would not fit onto a single page. Complete the
    // current chunk and start a new one.
    sink_.Put(kNextChunk, "NextChunk");
    sink_.Put(space, "NextChunkSpace");
    completed_chunks_[space].push_back(pending_chunk_[space]);
    pending_chunk_[space] = 0;
    new_chunk_size = size;
  }
  uint32_t offset = pending_chunk_[space];
  pending_chunk_[space] = new_chunk_size;
  return SerializerReference::BackReference(
      space, static_cast<uint32_t>(completed_chunks_[space].size()), offset);
}

void Serializer::Pad() {
  // The non-branching GetInt will read up to 3 bytes too far, so we need
  // to pad the snapshot to make sure we don't read over the end.
  for (unsigned i = 0; i < sizeof(int32_t) - 1; i++) {
    sink_.Put(kNop, "Padding");
  }
  // Pad up to pointer size for checksum.
  while (!IsAligned(sink_.Position(), kPointerAlignment)) {
    sink_.Put(kNop, "Padding");
  }
}

void Serializer::InitializeCodeAddressMap() {
  isolate_->InitializeLoggingAndCounters();
  code_address_map_ = new CodeAddressMap(isolate_);
}

Code* Serializer::CopyCode(Code* code) {
  code_buffer_.clear();  // Clear buffer without deleting backing store.
  int size = code->CodeSize();
  code_buffer_.insert(code_buffer_.end(), code->address(),
                      code->address() + size);
  return Code::cast(HeapObject::FromAddress(&code_buffer_.front()));
}

bool Serializer::HasNotExceededFirstPageOfEachSpace() {
  for (int i = 0; i < kNumberOfPreallocatedSpaces; i++) {
    if (!completed_chunks_[i].empty()) return false;
  }
  return true;
}

void Serializer::ObjectSerializer::SerializePrologue(AllocationSpace space,
                                                     int size, Map* map) {
  if (serializer_->code_address_map_) {
    const char* code_name =
        serializer_->code_address_map_->Lookup(object_->address());
    LOG(serializer_->isolate_,
        CodeNameEvent(object_->address(), sink_->Position(), code_name));
  }

  SerializerReference back_reference;
  if (space == LO_SPACE) {
    sink_->Put(kNewObject + reference_representation_ + space,
               "NewLargeObject");
    sink_->PutInt(size >> kObjectAlignmentBits, "ObjectSizeInWords");
    if (object_->IsCode()) {
      sink_->Put(EXECUTABLE, "executable large object");
    } else {
      sink_->Put(NOT_EXECUTABLE, "not executable large object");
    }
    back_reference = serializer_->AllocateLargeObject(size);
  } else if (space == MAP_SPACE) {
    DCHECK_EQ(Map::kSize, size);
    back_reference = serializer_->AllocateMap();
    sink_->Put(kNewObject + reference_representation_ + space, "NewMap");
    // This is redundant, but we include it anyways.
    sink_->PutInt(size >> kObjectAlignmentBits, "ObjectSizeInWords");
  } else {
    int fill = serializer_->PutAlignmentPrefix(object_);
    back_reference = serializer_->Allocate(space, size + fill);
    sink_->Put(kNewObject + reference_representation_ + space, "NewObject");
    sink_->PutInt(size >> kObjectAlignmentBits, "ObjectSizeInWords");
  }

#ifdef OBJECT_PRINT
  if (FLAG_serialization_statistics) {
    serializer_->CountInstanceType(map, size);
  }
#endif  // OBJECT_PRINT

  // Mark this object as already serialized.
  serializer_->reference_map()->Add(object_, back_reference);

  // Serialize the map (first word of the object).
  serializer_->SerializeObject(map, kPlain, kStartOfObject, 0);
}

int32_t Serializer::ObjectSerializer::SerializeBackingStore(
    void* backing_store, int32_t byte_length) {
  SerializerReference reference =
      serializer_->reference_map()->Lookup(backing_store);

  // Serialize the off-heap backing store.
  if (!reference.is_valid()) {
    sink_->Put(kOffHeapBackingStore, "Off-heap backing store");
    sink_->PutInt(byte_length, "length");
    sink_->PutRaw(static_cast<byte*>(backing_store), byte_length,
                  "BackingStore");
    reference = serializer_->AllocateOffHeapBackingStore();
    // Mark this backing store as already serialized.
    serializer_->reference_map()->Add(backing_store, reference);
  }

  return static_cast<int32_t>(reference.off_heap_backing_store_index());
}

// When a JSArrayBuffer is neutered, the FixedTypedArray that points to the
// same backing store does not know anything about it. This fixup step finds
// neutered TypedArrays and clears the values in the FixedTypedArray so that
// we don't try to serialize the now invalid backing store.
void Serializer::ObjectSerializer::FixupIfNeutered() {
  JSTypedArray* array = JSTypedArray::cast(object_);
  if (!array->WasNeutered()) return;

  FixedTypedArrayBase* fta = FixedTypedArrayBase::cast(array->elements());
  DCHECK(fta->base_pointer() == nullptr);
  fta->set_external_pointer(Smi::kZero);
  fta->set_length(0);
}

void Serializer::ObjectSerializer::SerializeJSArrayBuffer() {
  JSArrayBuffer* buffer = JSArrayBuffer::cast(object_);
  void* backing_store = buffer->backing_store();
  // We cannot store byte_length larger than Smi range in the snapshot.
  // Attempt to make sure that NumberToInt32 produces something sensible.
  CHECK(buffer->byte_length()->IsSmi());
  int32_t byte_length = NumberToInt32(buffer->byte_length());

  // The embedder-allocated backing store only exists for the off-heap case.
  if (backing_store != nullptr) {
    int32_t ref = SerializeBackingStore(backing_store, byte_length);
    buffer->set_backing_store(Smi::FromInt(ref));
  }
  SerializeObject();
}

void Serializer::ObjectSerializer::SerializeFixedTypedArray() {
  FixedTypedArrayBase* fta = FixedTypedArrayBase::cast(object_);
  void* backing_store = fta->DataPtr();
  // We cannot store byte_length larger than Smi range in the snapshot.
  CHECK(fta->ByteLength() < Smi::kMaxValue);
  int32_t byte_length = static_cast<int32_t>(fta->ByteLength());

  // The heap contains empty FixedTypedArrays for each type, with a byte_length
  // of 0 (e.g. empty_fixed_uint8_array). These look like they are are 'on-heap'
  // but have no data to copy, so we skip the backing store here.

  // The embedder-allocated backing store only exists for the off-heap case.
  if (byte_length > 0 && fta->base_pointer() == nullptr) {
    int32_t ref = SerializeBackingStore(backing_store, byte_length);
    fta->set_external_pointer(Smi::FromInt(ref));
  }
  SerializeObject();
}

void Serializer::ObjectSerializer::SerializeExternalString() {
  Heap* heap = serializer_->isolate()->heap();
  if (object_->map() != heap->native_source_string_map()) {
    // Usually we cannot recreate resources for external strings. To work
    // around this, external strings are serialized to look like ordinary
    // sequential strings.
    // The exception are native source code strings, since we can recreate
    // their resources.
    SerializeExternalStringAsSequentialString();
  } else {
    ExternalOneByteString* string = ExternalOneByteString::cast(object_);
    DCHECK(string->is_short());
    const NativesExternalStringResource* resource =
        reinterpret_cast<const NativesExternalStringResource*>(
            string->resource());
    // Replace the resource field with the type and index of the native source.
    string->set_resource(resource->EncodeForSerialization());
    SerializeObject();
    // Restore the resource field.
    string->set_resource(resource);
  }
}

void Serializer::ObjectSerializer::SerializeExternalStringAsSequentialString() {
  // Instead of serializing this as an external string, we serialize
  // an imaginary sequential string with the same content.
  Isolate* isolate = serializer_->isolate();
  DCHECK(object_->IsExternalString());
  DCHECK(object_->map() != isolate->heap()->native_source_string_map());
  ExternalString* string = ExternalString::cast(object_);
  int length = string->length();
  Map* map;
  int content_size;
  int allocation_size;
  const byte* resource;
  // Find the map and size for the imaginary sequential string.
  bool internalized = object_->IsInternalizedString();
  if (object_->IsExternalOneByteString()) {
    map = internalized ? isolate->heap()->one_byte_internalized_string_map()
                       : isolate->heap()->one_byte_string_map();
    allocation_size = SeqOneByteString::SizeFor(length);
    content_size = length * kCharSize;
    resource = reinterpret_cast<const byte*>(
        ExternalOneByteString::cast(string)->resource()->data());
  } else {
    map = internalized ? isolate->heap()->internalized_string_map()
                       : isolate->heap()->string_map();
    allocation_size = SeqTwoByteString::SizeFor(length);
    content_size = length * kShortSize;
    resource = reinterpret_cast<const byte*>(
        ExternalTwoByteString::cast(string)->resource()->data());
  }

  AllocationSpace space =
      (allocation_size > kMaxRegularHeapObjectSize) ? LO_SPACE : OLD_SPACE;
  SerializePrologue(space, allocation_size, map);

  // Output the rest of the imaginary string.
  int bytes_to_output = allocation_size - HeapObject::kHeaderSize;

  // Output raw data header. Do not bother with common raw length cases here.
  sink_->Put(kVariableRawData, "RawDataForString");
  sink_->PutInt(bytes_to_output, "length");

  // Serialize string header (except for map).
  Address string_start = string->address();
  for (int i = HeapObject::kHeaderSize; i < SeqString::kHeaderSize; i++) {
    sink_->PutSection(string_start[i], "StringHeader");
  }

  // Serialize string content.
  sink_->PutRaw(resource, content_size, "StringContent");

  // Since the allocation size is rounded up to object alignment, there
  // maybe left-over bytes that need to be padded.
  int padding_size = allocation_size - SeqString::kHeaderSize - content_size;
  DCHECK(0 <= padding_size && padding_size < kObjectAlignment);
  for (int i = 0; i < padding_size; i++) sink_->PutSection(0, "StringPadding");
}

// Clear and later restore the next link in the weak cell or allocation site.
// TODO(all): replace this with proper iteration of weak slots in serializer.
class UnlinkWeakNextScope {
 public:
  explicit UnlinkWeakNextScope(HeapObject* object) : object_(nullptr) {
    if (object->IsAllocationSite()) {
      object_ = object;
      next_ = AllocationSite::cast(object)->weak_next();
      AllocationSite::cast(object)->set_weak_next(
          object->GetHeap()->undefined_value());
    }
  }

  ~UnlinkWeakNextScope() {
    if (object_ != nullptr) {
      AllocationSite::cast(object_)->set_weak_next(next_,
                                                   UPDATE_WEAK_WRITE_BARRIER);
    }
  }

 private:
  HeapObject* object_;
  Object* next_;
  DisallowHeapAllocation no_gc_;
};

void Serializer::ObjectSerializer::Serialize() {
  if (FLAG_trace_serializer) {
    PrintF(" Encoding heap object: ");
    object_->ShortPrint();
    PrintF("\n");
  }

  if (object_->IsExternalString()) {
    SerializeExternalString();
    return;
  } else if (object_->IsSeqOneByteString()) {
    // Clear padding bytes at the end. Done here to avoid having to do this
    // at allocation sites in generated code.
    SeqOneByteString::cast(object_)->clear_padding();
  } else if (object_->IsSeqTwoByteString()) {
    SeqTwoByteString::cast(object_)->clear_padding();
  }
  if (object_->IsJSTypedArray()) {
    FixupIfNeutered();
  }
  if (object_->IsJSArrayBuffer()) {
    SerializeJSArrayBuffer();
    return;
  }
  if (object_->IsFixedTypedArrayBase()) {
    SerializeFixedTypedArray();
    return;
  }

  // We don't expect fillers.
  DCHECK(!object_->IsFiller());

  if (object_->IsScript()) {
    // Clear cached line ends.
    Object* undefined = serializer_->isolate()->heap()->undefined_value();
    Script::cast(object_)->set_line_ends(undefined);
  }

  SerializeObject();
}

void Serializer::ObjectSerializer::SerializeObject() {
  int size = object_->Size();
  Map* map = object_->map();
  AllocationSpace space =
      MemoryChunk::FromAddress(object_->address())->owner()->identity();
  SerializePrologue(space, size, map);

  // Serialize the rest of the object.
  CHECK_EQ(0, bytes_processed_so_far_);
  bytes_processed_so_far_ = kPointerSize;

  RecursionScope recursion(serializer_);
  // Objects that are immediately post processed during deserialization
  // cannot be deferred, since post processing requires the object content.
  if ((recursion.ExceedsMaximum() && CanBeDeferred(object_)) ||
      serializer_->MustBeDeferred(object_)) {
    serializer_->QueueDeferredObject(object_);
    sink_->Put(kDeferred, "Deferring object content");
    return;
  }

  SerializeContent(map, size);
}

void Serializer::ObjectSerializer::SerializeDeferred() {
  if (FLAG_trace_serializer) {
    PrintF(" Encoding deferred heap object: ");
    object_->ShortPrint();
    PrintF("\n");
  }

  int size = object_->Size();
  Map* map = object_->map();
  SerializerReference back_reference =
      serializer_->reference_map()->Lookup(object_);
  DCHECK(back_reference.is_back_reference());

  // Serialize the rest of the object.
  CHECK_EQ(0, bytes_processed_so_far_);
  bytes_processed_so_far_ = kPointerSize;

  serializer_->PutAlignmentPrefix(object_);
  sink_->Put(kNewObject + back_reference.space(), "deferred object");
  serializer_->PutBackReference(object_, back_reference);
  sink_->PutInt(size >> kPointerSizeLog2, "deferred object size");

  SerializeContent(map, size);
}

void Serializer::ObjectSerializer::SerializeContent(Map* map, int size) {
  UnlinkWeakNextScope unlink_weak_next(object_);
  if (object_->IsCode()) {
    // For code objects, output raw bytes first.
    OutputCode(size);
    // Then iterate references via reloc info.
    object_->IterateBody(map->instance_type(), size, this);
    // Finally skip to the end.
    serializer_->FlushSkip(SkipTo(object_->address() + size));
  } else {
    // For other objects, iterate references first.
    object_->IterateBody(map->instance_type(), size, this);
    // Then output data payload, if any.
    OutputRawData(object_->address() + size);
  }
}

void Serializer::ObjectSerializer::VisitPointers(HeapObject* host,
                                                 Object** start, Object** end) {
  Object** current = start;
  while (current < end) {
    while (current < end && (*current)->IsSmi()) current++;
    if (current < end) OutputRawData(reinterpret_cast<Address>(current));

    while (current < end && !(*current)->IsSmi()) {
      HeapObject* current_contents = HeapObject::cast(*current);
      int root_index = serializer_->root_index_map()->Lookup(current_contents);
      // Repeats are not subject to the write barrier so we can only use
      // immortal immovable root members. They are never in new space.
      if (current != start && root_index != RootIndexMap::kInvalidRootIndex &&
          Heap::RootIsImmortalImmovable(root_index) &&
          current_contents == current[-1]) {
        DCHECK(!serializer_->isolate()->heap()->InNewSpace(current_contents));
        int repeat_count = 1;
        while (&current[repeat_count] < end - 1 &&
               current[repeat_count] == current_contents) {
          repeat_count++;
        }
        current += repeat_count;
        bytes_processed_so_far_ += repeat_count * kPointerSize;
        if (repeat_count > kNumberOfFixedRepeat) {
          sink_->Put(kVariableRepeat, "VariableRepeat");
          sink_->PutInt(repeat_count, "repeat count");
        } else {
          sink_->Put(kFixedRepeatStart + repeat_count, "FixedRepeat");
        }
      } else {
        serializer_->SerializeObject(current_contents, kPlain, kStartOfObject,
                                     0);
        bytes_processed_so_far_ += kPointerSize;
        current++;
      }
    }
  }
}

void Serializer::ObjectSerializer::VisitEmbeddedPointer(Code* host,
                                                        RelocInfo* rinfo) {
  int skip = SkipTo(rinfo->target_address_address());
  HowToCode how_to_code = rinfo->IsCodedSpecially() ? kFromCode : kPlain;
  Object* object = rinfo->target_object();
  serializer_->SerializeObject(HeapObject::cast(object), how_to_code,
                               kStartOfObject, skip);
  bytes_processed_so_far_ += rinfo->target_address_size();
}

void Serializer::ObjectSerializer::VisitExternalReference(Foreign* host,
                                                          Address* p) {
  int skip = SkipTo(reinterpret_cast<Address>(p));
  Address target = *p;
  auto encoded_reference = serializer_->EncodeExternalReference(target);
  if (encoded_reference.is_from_api()) {
    sink_->Put(kApiReference, "ApiRef");
  } else {
    sink_->Put(kExternalReference + kPlain + kStartOfObject, "ExternalRef");
  }
  sink_->PutInt(skip, "SkipB4ExternalRef");
  sink_->PutInt(encoded_reference.index(), "reference index");
  bytes_processed_so_far_ += kPointerSize;
}

void Serializer::ObjectSerializer::VisitExternalReference(Code* host,
                                                          RelocInfo* rinfo) {
  int skip = SkipTo(rinfo->target_address_address());
  Address target = rinfo->target_external_reference();
  auto encoded_reference = serializer_->EncodeExternalReference(target);
  if (encoded_reference.is_from_api()) {
    DCHECK(!rinfo->IsCodedSpecially());
    sink_->Put(kApiReference, "ApiRef");
  } else {
    HowToCode how_to_code = rinfo->IsCodedSpecially() ? kFromCode : kPlain;
    sink_->Put(kExternalReference + how_to_code + kStartOfObject,
               "ExternalRef");
  }
  sink_->PutInt(skip, "SkipB4ExternalRef");
  DCHECK_NOT_NULL(target);  // Code does not reference null.
  sink_->PutInt(encoded_reference.index(), "reference index");
  bytes_processed_so_far_ += rinfo->target_address_size();
}

void Serializer::ObjectSerializer::VisitInternalReference(Code* host,
                                                          RelocInfo* rinfo) {
  // We do not use skip from last patched pc to find the pc to patch, since
  // target_address_address may not return addresses in ascending order when
  // used for internal references. External references may be stored at the
  // end of the code in the constant pool, whereas internal references are
  // inline. That would cause the skip to be negative. Instead, we store the
  // offset from code entry.
  Address entry = Code::cast(object_)->entry();
  intptr_t pc_offset = rinfo->target_internal_reference_address() - entry;
  intptr_t target_offset = rinfo->target_internal_reference() - entry;
  DCHECK(0 <= pc_offset &&
         pc_offset <= Code::cast(object_)->instruction_size());
  DCHECK(0 <= target_offset &&
         target_offset <= Code::cast(object_)->instruction_size());
  sink_->Put(rinfo->rmode() == RelocInfo::INTERNAL_REFERENCE
                 ? kInternalReference
                 : kInternalReferenceEncoded,
             "InternalRef");
  sink_->PutInt(static_cast<uintptr_t>(pc_offset), "internal ref address");
  sink_->PutInt(static_cast<uintptr_t>(target_offset), "internal ref value");
}

void Serializer::ObjectSerializer::VisitRuntimeEntry(Code* host,
                                                     RelocInfo* rinfo) {
  int skip = SkipTo(rinfo->target_address_address());
  HowToCode how_to_code = rinfo->IsCodedSpecially() ? kFromCode : kPlain;
  Address target = rinfo->target_address();
  auto encoded_reference = serializer_->EncodeExternalReference(target);
  DCHECK(!encoded_reference.is_from_api());
  sink_->Put(kExternalReference + how_to_code + kStartOfObject, "ExternalRef");
  sink_->PutInt(skip, "SkipB4ExternalRef");
  sink_->PutInt(encoded_reference.index(), "reference index");
  bytes_processed_so_far_ += rinfo->target_address_size();
}

void Serializer::ObjectSerializer::VisitCodeTarget(Code* host,
                                                   RelocInfo* rinfo) {
  int skip = SkipTo(rinfo->target_address_address());
  Code* object = Code::GetCodeFromTargetAddress(rinfo->target_address());
  serializer_->SerializeObject(object, kFromCode, kInnerPointer, skip);
  bytes_processed_so_far_ += rinfo->target_address_size();
}

void Serializer::ObjectSerializer::OutputRawData(Address up_to) {
  Address object_start = object_->address();
  int base = bytes_processed_so_far_;
  int up_to_offset = static_cast<int>(up_to - object_start);
  int to_skip = up_to_offset - bytes_processed_so_far_;
  int bytes_to_output = to_skip;
  bytes_processed_so_far_ += to_skip;
  DCHECK(to_skip >= 0);
  if (bytes_to_output != 0) {
    DCHECK(to_skip == bytes_to_output);
    if (IsAligned(bytes_to_output, kPointerAlignment) &&
        bytes_to_output <= kNumberOfFixedRawData * kPointerSize) {
      int size_in_words = bytes_to_output >> kPointerSizeLog2;
      sink_->PutSection(kFixedRawDataStart + size_in_words, "FixedRawData");
    } else {
      sink_->Put(kVariableRawData, "VariableRawData");
      sink_->PutInt(bytes_to_output, "length");
    }
#ifdef MEMORY_SANITIZER
    // Check that we do not serialize uninitialized memory.
    __msan_check_mem_is_initialized(object_start + base, bytes_to_output);
#endif  // MEMORY_SANITIZER
    sink_->PutRaw(object_start + base, bytes_to_output, "Bytes");
  }
}

int Serializer::ObjectSerializer::SkipTo(Address to) {
  Address object_start = object_->address();
  int up_to_offset = static_cast<int>(to - object_start);
  int to_skip = up_to_offset - bytes_processed_so_far_;
  bytes_processed_so_far_ += to_skip;
  // This assert will fail if the reloc info gives us the target_address_address
  // locations in a non-ascending order.  Luckily that doesn't happen.
  DCHECK(to_skip >= 0);
  return to_skip;
}

void Serializer::ObjectSerializer::OutputCode(int size) {
  DCHECK_EQ(kPointerSize, bytes_processed_so_far_);
  Code* code = Code::cast(object_);
  if (FLAG_predictable) {
    // To make snapshots reproducible, we make a copy of the code object
    // and wipe all pointers in the copy, which we then serialize.
    code = serializer_->CopyCode(code);
    int mode_mask = RelocInfo::kCodeTargetMask |
                    RelocInfo::ModeMask(RelocInfo::EMBEDDED_OBJECT) |
                    RelocInfo::ModeMask(RelocInfo::EXTERNAL_REFERENCE) |
                    RelocInfo::ModeMask(RelocInfo::RUNTIME_ENTRY) |
                    RelocInfo::ModeMask(RelocInfo::INTERNAL_REFERENCE) |
                    RelocInfo::ModeMask(RelocInfo::INTERNAL_REFERENCE_ENCODED);
    for (RelocIterator it(code, mode_mask); !it.done(); it.next()) {
      RelocInfo* rinfo = it.rinfo();
      rinfo->WipeOut(serializer_->isolate());
    }
    // We need to wipe out the header fields *after* wiping out the
    // relocations, because some of these fields are needed for the latter.
    code->WipeOutHeader();
  }

  Address start = code->address() + Code::kDataStart;
  int bytes_to_output = size - Code::kDataStart;

  sink_->Put(kVariableRawCode, "VariableRawCode");
  sink_->PutInt(bytes_to_output, "length");

#ifdef MEMORY_SANITIZER
  // Check that we do not serialize uninitialized memory.
  __msan_check_mem_is_initialized(start, bytes_to_output);
#endif  // MEMORY_SANITIZER
  sink_->PutRaw(start, bytes_to_output, "Code");
}

}  // namespace internal
}  // namespace v8
