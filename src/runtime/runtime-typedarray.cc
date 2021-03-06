// Copyright 2014 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/runtime/runtime-utils.h"

#include "src/arguments.h"
#include "src/elements.h"
#include "src/factory.h"
#include "src/messages.h"
#include "src/objects-inl.h"
#include "src/runtime/runtime.h"

namespace v8 {
namespace internal {

RUNTIME_FUNCTION(Runtime_ArrayBufferGetByteLength) {
  SealHandleScope shs(isolate);
  DCHECK_EQ(1, args.length());
  CONVERT_ARG_CHECKED(JSArrayBuffer, holder, 0);
  return holder->byte_length();
}


RUNTIME_FUNCTION(Runtime_ArrayBufferNeuter) {
  HandleScope scope(isolate);
  DCHECK_EQ(1, args.length());
  Handle<Object> argument = args.at(0);
  // This runtime function is exposed in ClusterFuzz and as such has to
  // support arbitrary arguments.
  if (!argument->IsJSArrayBuffer()) {
    THROW_NEW_ERROR_RETURN_FAILURE(
        isolate, NewTypeError(MessageTemplate::kNotTypedArray));
  }
  Handle<JSArrayBuffer> array_buffer = Handle<JSArrayBuffer>::cast(argument);
  if (!array_buffer->is_neuterable()) {
    return isolate->heap()->undefined_value();
  }
  if (array_buffer->backing_store() == nullptr) {
    CHECK_EQ(Smi::kZero, array_buffer->byte_length());
    return isolate->heap()->undefined_value();
  }
  // Shared array buffers should never be neutered.
  CHECK(!array_buffer->is_shared());
  DCHECK(!array_buffer->is_external());
  void* backing_store = array_buffer->backing_store();
  size_t byte_length = NumberToSize(array_buffer->byte_length());
  array_buffer->set_is_external(true);
  isolate->heap()->UnregisterArrayBuffer(*array_buffer);
  array_buffer->Neuter();
  isolate->array_buffer_allocator()->Free(backing_store, byte_length);
  return isolate->heap()->undefined_value();
}

RUNTIME_FUNCTION(Runtime_TypedArrayCopyElements) {
  HandleScope scope(isolate);
  DCHECK_EQ(3, args.length());
  CONVERT_ARG_HANDLE_CHECKED(JSTypedArray, target, 0);
  CONVERT_ARG_HANDLE_CHECKED(JSReceiver, source, 1);
  CONVERT_NUMBER_ARG_HANDLE_CHECKED(length_obj, 2);

  size_t length;
  CHECK(TryNumberToSize(*length_obj, &length));

  ElementsAccessor* accessor = target->GetElementsAccessor();
  return accessor->CopyElements(source, target, length);
}

#define BUFFER_VIEW_GETTER(Type, getter, accessor)   \
  RUNTIME_FUNCTION(Runtime_##Type##Get##getter) {    \
    HandleScope scope(isolate);                      \
    DCHECK_EQ(1, args.length());                     \
    CONVERT_ARG_HANDLE_CHECKED(JS##Type, holder, 0); \
    return holder->accessor();                       \
  }

BUFFER_VIEW_GETTER(ArrayBufferView, ByteLength, byte_length)
BUFFER_VIEW_GETTER(ArrayBufferView, ByteOffset, byte_offset)
BUFFER_VIEW_GETTER(TypedArray, Length, length)

#undef BUFFER_VIEW_GETTER

RUNTIME_FUNCTION(Runtime_ArrayBufferViewWasNeutered) {
  HandleScope scope(isolate);
  DCHECK_EQ(1, args.length());
  return isolate->heap()->ToBoolean(JSTypedArray::cast(args[0])->WasNeutered());
}

RUNTIME_FUNCTION(Runtime_TypedArrayGetBuffer) {
  HandleScope scope(isolate);
  DCHECK_EQ(1, args.length());
  CONVERT_ARG_HANDLE_CHECKED(JSTypedArray, holder, 0);
  return *holder->GetBuffer();
}


namespace {

template <typename T>
bool CompareNum(T x, T y) {
  if (x < y) {
    return true;
  } else if (x > y) {
    return false;
  } else if (!std::is_integral<T>::value) {
    double _x = x, _y = y;
    if (x == 0 && x == y) {
      /* -0.0 is less than +0.0 */
      return std::signbit(_x) && !std::signbit(_y);
    } else if (!std::isnan(_x) && std::isnan(_y)) {
      /* number is less than NaN */
      return true;
    }
  }
  return false;
}

}  // namespace

RUNTIME_FUNCTION(Runtime_TypedArraySortFast) {
  HandleScope scope(isolate);
  DCHECK_EQ(1, args.length());

  CONVERT_ARG_HANDLE_CHECKED(Object, target_obj, 0);

  Handle<JSTypedArray> array;
  const char* method = "%TypedArray%.prototype.sort";
  ASSIGN_RETURN_FAILURE_ON_EXCEPTION(
      isolate, array, JSTypedArray::Validate(isolate, target_obj, method));

  // This line can be removed when JSTypedArray::Validate throws
  // if array.[[ViewedArrayBuffer]] is neutered(v8:4648)
  if (V8_UNLIKELY(array->WasNeutered())) return *array;

  size_t length = array->length_value();
  if (length <= 1) return *array;

  Handle<FixedTypedArrayBase> elements(
      FixedTypedArrayBase::cast(array->elements()));
  switch (array->type()) {
#define TYPED_ARRAY_SORT(Type, type, TYPE, ctype, size)     \
  case kExternal##Type##Array: {                            \
    ctype* data = static_cast<ctype*>(elements->DataPtr()); \
    if (kExternal##Type##Array == kExternalFloat64Array ||  \
        kExternal##Type##Array == kExternalFloat32Array)    \
      std::sort(data, data + length, CompareNum<ctype>);    \
    else                                                    \
      std::sort(data, data + length);                       \
    break;                                                  \
  }

    TYPED_ARRAYS(TYPED_ARRAY_SORT)
#undef TYPED_ARRAY_SORT
  }

  return *array;
}

RUNTIME_FUNCTION(Runtime_IsTypedArray) {
  HandleScope scope(isolate);
  DCHECK_EQ(1, args.length());
  return isolate->heap()->ToBoolean(args[0]->IsJSTypedArray());
}

RUNTIME_FUNCTION(Runtime_IsSharedTypedArray) {
  HandleScope scope(isolate);
  DCHECK_EQ(1, args.length());
  return isolate->heap()->ToBoolean(
      args[0]->IsJSTypedArray() &&
      JSTypedArray::cast(args[0])->GetBuffer()->is_shared());
}


RUNTIME_FUNCTION(Runtime_IsSharedIntegerTypedArray) {
  HandleScope scope(isolate);
  DCHECK_EQ(1, args.length());
  if (!args[0]->IsJSTypedArray()) {
    return isolate->heap()->false_value();
  }

  Handle<JSTypedArray> obj(JSTypedArray::cast(args[0]));
  return isolate->heap()->ToBoolean(obj->GetBuffer()->is_shared() &&
                                    obj->type() != kExternalFloat32Array &&
                                    obj->type() != kExternalFloat64Array &&
                                    obj->type() != kExternalUint8ClampedArray);
}


RUNTIME_FUNCTION(Runtime_IsSharedInteger32TypedArray) {
  HandleScope scope(isolate);
  DCHECK_EQ(1, args.length());
  if (!args[0]->IsJSTypedArray()) {
    return isolate->heap()->false_value();
  }

  Handle<JSTypedArray> obj(JSTypedArray::cast(args[0]));
  return isolate->heap()->ToBoolean(obj->GetBuffer()->is_shared() &&
                                    obj->type() == kExternalInt32Array);
}

RUNTIME_FUNCTION(Runtime_TypedArraySpeciesCreateByLength) {
  HandleScope scope(isolate);
  DCHECK_EQ(args.length(), 2);
  Handle<JSTypedArray> exemplar = args.at<JSTypedArray>(0);
  Handle<Object> length = args.at(1);
  int argc = 1;
  ScopedVector<Handle<Object>> argv(argc);
  argv[0] = length;
  Handle<JSTypedArray> result_array;
  // TODO(tebbi): Pass correct method name.
  ASSIGN_RETURN_FAILURE_ON_EXCEPTION(
      isolate, result_array,
      JSTypedArray::SpeciesCreate(isolate, exemplar, argc, argv.start(), ""));
  return *result_array;
}

namespace {
Object* TypedArrayCopyElements(Handle<JSTypedArray> target,
                               Handle<JSReceiver> source, uint32_t length,
                               uint32_t offset) {
  ElementsAccessor* accessor = target->GetElementsAccessor();
  return accessor->CopyElements(source, target, length, offset);
}

enum class TypedArraySetResultCodes {
  // Set from typed array of the different type, overlapping in memory.
  OVERLAPPING,
  // Set from typed array of the different type, non-overlapping.
  NONOVERLAPPING,
  // Set from non-typed array.
  NON_TYPED_ARRAY,
};

MaybeHandle<Object> TypedArraySetFromOverlapping(Isolate* isolate,
                                                 Handle<JSTypedArray> target,
                                                 Handle<JSTypedArray> source,
                                                 uint32_t offset) {
  DCHECK_GE(offset, 0);

  size_t sourceElementSize = source->element_size();
  size_t targetElementSize = target->element_size();

  uint32_t source_length = source->length_value();
  if (source_length == 0) return target;

  // Copy left part.

  // First un-mutated byte after the next write
  uint32_t target_ptr = 0;
  CHECK(target->byte_offset()->ToUint32(&target_ptr));
  target_ptr += (offset + 1) * targetElementSize;

  // Next read at sourcePtr. We do not care for memory changing before
  // sourcePtr - we have already copied it.
  uint32_t source_ptr = 0;
  CHECK(source->byte_offset()->ToUint32(&source_ptr));

  ElementsAccessor* source_accessor = source->GetElementsAccessor();
  ElementsAccessor* target_accessor = target->GetElementsAccessor();

  uint32_t left_index;
  for (left_index = 0; left_index < source_length && target_ptr <= source_ptr;
       left_index++) {
    Handle<Object> value = source_accessor->Get(source, left_index);
    target_accessor->Set(target, offset + left_index, *value);

    target_ptr += targetElementSize;
    source_ptr += sourceElementSize;
  }

  // Copy right part;
  // First unmutated byte before the next write
  CHECK(target->byte_offset()->ToUint32(&target_ptr));
  target_ptr += (offset + source_length - 1) * targetElementSize;

  // Next read before sourcePtr. We do not care for memory changing after
  // sourcePtr - we have already copied it.
  CHECK(target->byte_offset()->ToUint32(&source_ptr));
  source_ptr += source_length * sourceElementSize;

  uint32_t right_index;
  DCHECK_GE(source_length, 1);
  for (right_index = source_length - 1;
       right_index > left_index && target_ptr >= source_ptr; right_index--) {
    Handle<Object> value = source_accessor->Get(source, right_index);
    target_accessor->Set(target, offset + right_index, *value);

    target_ptr -= targetElementSize;
    source_ptr -= sourceElementSize;
  }

  std::vector<Handle<Object>> temp(right_index + 1 - left_index);

  for (uint32_t i = left_index; i <= right_index; i++) {
    temp[i - left_index] = source_accessor->Get(source, i);
  }

  for (uint32_t i = left_index; i <= right_index; i++) {
    target_accessor->Set(target, offset + i, *temp[i - left_index]);
  }

  return target;
}

MaybeHandle<Smi> TypedArraySetFastCases(Isolate* isolate,
                                        Handle<JSTypedArray> target,
                                        Handle<Object> source_obj,
                                        Handle<Object> offset_obj) {
  if (!source_obj->IsJSTypedArray()) {
    return MaybeHandle<Smi>(
        Smi::FromEnum(TypedArraySetResultCodes::NON_TYPED_ARRAY), isolate);
  }

  Handle<JSTypedArray> source = Handle<JSTypedArray>::cast(source_obj);
  DCHECK_NE(target->type(), source->type());  // Handled in SetTypedArraySource.

  size_t offset = 0;
  CHECK(TryNumberToSize(*offset_obj, &offset));
  size_t target_length = target->length_value();
  size_t source_length = source->length_value();
  size_t target_byte_length = NumberToSize(target->byte_length());
  size_t source_byte_length = NumberToSize(source->byte_length());
  if (offset > target_length || offset + source_length > target_length ||
      offset + source_length < offset) {  // overflow
    THROW_NEW_ERROR(
        isolate, NewRangeError(MessageTemplate::kTypedArraySetSourceTooLarge),
        Smi);
  }

  size_t target_offset = NumberToSize(target->byte_offset());
  size_t source_offset = NumberToSize(source->byte_offset());
  uint8_t* target_base =
      static_cast<uint8_t*>(target->GetBuffer()->backing_store()) +
      target_offset;
  uint8_t* source_base =
      static_cast<uint8_t*>(source->GetBuffer()->backing_store()) +
      source_offset;

  // Typed arrays of different types over the same backing store
  if ((source_base <= target_base &&
       source_base + source_byte_length > target_base) ||
      (target_base <= source_base &&
       target_base + target_byte_length > source_base)) {
    // We do not support overlapping ArrayBuffers
    DCHECK(target->GetBuffer()->backing_store() ==
           source->GetBuffer()->backing_store());
    return MaybeHandle<Smi>(
        Smi::FromEnum(TypedArraySetResultCodes::OVERLAPPING), isolate);
  } else {  // Non-overlapping typed arrays
    return MaybeHandle<Smi>(
        Smi::FromEnum(TypedArraySetResultCodes::NONOVERLAPPING), isolate);
  }
}

}  // anonymous namespace

// 22.2.3.23%TypedArray%.prototype.set ( overloaded [ , offset ] )
RUNTIME_FUNCTION(Runtime_TypedArraySet) {
  HandleScope scope(isolate);
  Handle<JSTypedArray> target = args.at<JSTypedArray>(0);
  Handle<Object> obj = args.at(1);
  Handle<Smi> offset = args.at<Smi>(2);

  DCHECK(!target->WasNeutered());  // Checked in TypedArrayPrototypeSet.
  DCHECK(0 <= offset->value() && offset->value() <= Smi::kMaxValue);

  const uint32_t uint_offset = static_cast<uint32_t>(offset->value());

  // TODO(cwhan.tunz): Implement CopyElements for overlapping cases, and use
  // TypedArrayCopyElements for all case instead of this result code based
  // branches
  Handle<Smi> result_code;
  ASSIGN_RETURN_FAILURE_ON_EXCEPTION(
      isolate, result_code,
      TypedArraySetFastCases(isolate, target, obj, offset));

  switch (static_cast<TypedArraySetResultCodes>(result_code->value())) {
    case TypedArraySetResultCodes::OVERLAPPING: {
      RETURN_FAILURE_ON_EXCEPTION(
          isolate,
          TypedArraySetFromOverlapping(
              isolate, target, Handle<JSTypedArray>::cast(obj), uint_offset));
      break;
    }
    case TypedArraySetResultCodes::NONOVERLAPPING: {
      return TypedArrayCopyElements(
          target, Handle<JSTypedArray>::cast(obj),
          Handle<JSTypedArray>::cast(obj)->length_value(), uint_offset);
      break;
    }
    case TypedArraySetResultCodes::NON_TYPED_ARRAY: {
      if (obj->IsNumber()) {
        // For number as a first argument, throw TypeError
        // instead of silently ignoring the call, so that
        // users know they did something wrong.
        // (Consistent with Firefox and Blink/WebKit)
        THROW_NEW_ERROR_RETURN_FAILURE(
            isolate, NewTypeError(MessageTemplate::kInvalidArgument));
      }

      ASSIGN_RETURN_FAILURE_ON_EXCEPTION(isolate, obj,
                                         Object::ToObject(isolate, obj));

      Handle<Object> len;
      ASSIGN_RETURN_FAILURE_ON_EXCEPTION(
          isolate, len,
          Object::GetProperty(obj, isolate->factory()->length_string()));
      if (len->IsUndefined(isolate)) {
        break;
      }
      ASSIGN_RETURN_FAILURE_ON_EXCEPTION(isolate, len,
                                         Object::ToLength(isolate, len));

      DCHECK_GE(uint_offset, 0);
      if (uint_offset + len->Number() > target->length_value()) {
        THROW_NEW_ERROR_RETURN_FAILURE(
            isolate,
            NewRangeError(MessageTemplate::kTypedArraySetSourceTooLarge));
      }
      uint32_t int_l;
      CHECK(DoubleToUint32IfEqualToSelf(len->Number(), &int_l));
      return TypedArrayCopyElements(target, Handle<JSReceiver>::cast(obj),
                                    int_l, uint_offset);
    } break;
  }

  return *isolate->factory()->undefined_value();
}

}  // namespace internal
}  // namespace v8
