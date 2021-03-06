/*
 * Copyright 2020 The TensorFlow Runtime Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//===- kernel_frame.h - Information for kernel invocation -------*- C++ -*-===//
//
// This file implements KernelFrame which captures argument, result, and other
// related information provided to kernels on kernel invocation.
//
//===----------------------------------------------------------------------===//

#ifndef TFRT_HOST_CONTEXT_KERNEL_CONTEXT_H_
#define TFRT_HOST_CONTEXT_KERNEL_CONTEXT_H_

#include <string>
#include <utility>

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "tfrt/host_context/async_value.h"
#include "tfrt/host_context/attribute_utils.h"
#include "tfrt/host_context/execution_context.h"
#include "tfrt/host_context/host_context.h"
#include "tfrt/host_context/location.h"
#include "tfrt/support/forward_decls.h"
#include "tfrt/support/string_util.h"

namespace tfrt {

// KernelFrame captures the states associated with a kernel invocation,
// including the input arguments, attributes, result values, location and host
// context. KernelFrame is constructed by the kernel caller (currently only
// BEFExecutor) using the KernelFrameBuilder subclass. The kernel implementation
// is passed a pointer to a KernelFrame object for them to access the inputs and
// attributes, and return result values.
//
// The result AsyncValue pointers are not initialized when a kernel is called.
// The Kernel implementation is responsible for creating AsyncValue objects and
// setting the result AsyncValue pointers.
class KernelFrame {
 public:
  explicit KernelFrame(HostContext* host) : exec_ctx_{host} {}

  const ExecutionContext& GetExecutionContext() const { return exec_ctx_; }
  HostContext* GetHostContext() const { return exec_ctx_.host(); }

  // Get the location.
  Location GetLocation() const { return exec_ctx_.location(); }

  ArrayRef<uint8_t> GetAttributeSection() const { return attribute_section_; }

  // Get the number of arguments.
  int GetNumArgs() const { return num_arguments_; }

  // Get the argument at the given index as type T.
  template <typename T>
  T& GetArgAt(int index) const {
    return GetArgAt(index)->get<T>();
  }

  // Get the argument at the given index as AsyncValue*.
  AsyncValue* GetArgAt(int index) const {
    assert(index < GetNumArgs());
    return async_value_or_attrs_[index].async_value;
  }

  // Get all arguments.
  ArrayRef<AsyncValue*> GetArguments() const {
    return GetAsyncValues(0, num_arguments_);
  }

  // Get all attributes.
  ArrayRef<const void*> GetAttributes() const {
    if (async_value_or_attrs_.empty()) return {};

    return llvm::makeArrayRef(
        &async_value_or_attrs_[num_arguments_ + num_results_].attr,
        GetNumAttributes());
  }

  // Get the number of attributes.
  int GetNumAttributes() const {
    return async_value_or_attrs_.size() - num_arguments_ - num_results_;
  }

  // Get the attribute at the given index as type T.
  // TODO(jingdong): Disable const char*.
  template <typename T>
  Attribute<T> GetAttributeAt(int index) const {
    assert(index < GetNumAttributes());
    return Attribute<T>(GetAttributes()[index]);
  }

  AggregateAttr GetAggregateAttr(int index) const {
    assert(index < GetNumAttributes());
    return AggregateAttr(GetAttributes()[index]);
  }

  // Get the array attribute at the given index as type T.
  template <typename T>
  ArrayAttribute<T> GetArrayAttributeAt(int index) const {
    assert(index < GetNumAttributes());
    return ArrayAttribute<T>(GetAttributes()[index]);
  }

  // Get array attribute as a string. Equivalent to
  // GetArrayAttributeAt<char>, except that this returns StringRef instead
  // of ArrayRef<char>.
  StringAttribute GetStringAttribute(int index) const {
    return StringAttribute(GetAttributes()[index]);
  }

  // Get the number of results.
  int GetNumResults() const { return num_results_; }

  // Emplace construct the result at index 0.
  template <typename T, typename... Args>
  void EmplaceResult(Args&&... args) {
    EmplaceResultAt<T>(0, std::forward<Args>(args)...);
  }

  // Emplace construct the result at given index.
  template <typename T, typename... Args>
  void EmplaceResultAt(int index, Args&&... args) {
    SetResultAt(index, GetHostContext()->MakeAvailableAsyncValueRef<T>(
                           std::forward<Args>(args)...));
  }

  // Allocate an AsyncValue with uninitialized payload as the result at the
  // index 0 and return the allocated AsyncValue.
  template <typename T>
  AsyncValueRef<T> AllocateResult() {
    return AllocateResultAt<T>(0);
  }

  // Allocate an AsyncValue with uninitialized payload as the result at the
  // given index and return the allocated AsyncValue.
  template <typename T>
  AsyncValueRef<T> AllocateResultAt(int index) {
    auto result = GetHostContext()->MakeUnconstructedAsyncValueRef<T>();
    SetResultAt(index, result.CopyRef());
    return result;
  }

  // Set the result at the given index with the given AsyncValue.
  void SetResultAt(int index, RCReference<AsyncValue> value) {
    assert(index < num_results_ && "Invalid result index");
    AsyncValue*& result =
        async_value_or_attrs_[num_arguments_ + index].async_value;
    assert(!result && "Result is not nullptr");
    result = value.release();
  }

  template <typename T>
  void SetResultAt(int index, AsyncValueRef<T> value) {
    SetResultAt(index, value.ReleaseRCRef());
  }

  // Allocate an AsyncValue with uninitialized payload as the result at the
  // given index and return the allocated AsyncValue.
  RCReference<IndirectAsyncValue> AllocateIndirectResultAt(int index) {
    auto result = GetHostContext()->MakeIndirectAsyncValue();
    SetResultAt(index, result.CopyRef());
    return result;
  }

  // Get all results as an immutable ArrayRef
  ArrayRef<AsyncValue*> GetResults() const {
    return GetAsyncValues(num_arguments_, num_results_);
  }

  // Get all results as MutableArrayRef.
  MutableArrayRef<AsyncValue*> GetResults() {
    return GetMutableAsyncValues(num_arguments_, num_results_);
  }

  // Example usage:
  //
  // kernel_handler.ReportError("This is an error message");
  // int i = 2;
  // TensorShape shape = ...
  // kernel_handler.ReportError("Error: i is ", i, ", shape is ", shape);
  template <typename... Args>
  void ReportError(Args&&... args) {
    ReportError(string_view(StrCat(std::forward<Args>(args)...)));
  }
  // Report error and set any unset results with an error AsyncValue.
  void ReportError(string_view msg);

  template <typename... Args>
  RCReference<AsyncValue> EmitError(Args&&... args) {
    return EmitError(string_view(StrCat(std::forward<Args>(args)...)));
  }

  // Emit an AsyncValue that contains an error using the kernel location.
  // For consistency, the error message should start with a lower case letter
  // and not end with a period.
  RCReference<AsyncValue> EmitError(string_view msg) {
    return EmitErrorAsync(exec_ctx_, msg);
  }

  // Assert the size of arguments, attributes, and results are as expected.
  void AssertArity(int num_arguments, int num_attributes,
                   int num_results) const;

 protected:
  union AsyncValueOrAttribute {
    AsyncValue* async_value;
    const void* attr;
  };

  ArrayRef<AsyncValue*> GetAsyncValues(size_t from, size_t length) const {
    assert((from + length) <= (num_arguments_ + num_results_));

    if (length == 0) return {};

    return llvm::makeArrayRef(&(async_value_or_attrs_[from].async_value),
                              length);
  }

  MutableArrayRef<AsyncValue*> GetMutableAsyncValues(size_t from,
                                                     size_t length) {
    assert((from + length) <= (num_arguments_ + num_results_));

    if (length == 0) return {};

    return llvm::makeMutableArrayRef(&(async_value_or_attrs_[from].async_value),
                                     length);
  }

  // This SmallVector stores the kernel argument AsyncValues, result
  // AsyncValues, and attributes in order.
  SmallVector<AsyncValueOrAttribute, 8> async_value_or_attrs_;
  int num_arguments_ = 0;
  // num_results is set to -1 so we can check that AddAttribute() is called
  // after SetNumResults.
  int num_results_ = -1;
  ArrayRef<uint8_t> attribute_section_;
  ExecutionContext exec_ctx_;
};

// KernelFrameBuilder is used by the kernel caller to construct a KernelFrame
// object without exposing the builder methods to the kernel implementation.
//
// As an optimization, KernelFrame stores arguments, attributes, and results in
// a single SmallVector. As a result, to initialize a KernelFrame, this class
// requires that the client performs the following actions in order:
// 1. Adds the arguments (using AddArg()),
// 2. Set the number of results (using SetNumResults())
// 3. Add the attributes (using AddAttribute())
class KernelFrameBuilder : public KernelFrame {
 public:
  explicit KernelFrameBuilder(HostContext* host) : KernelFrame{host} {}

  // Get result AsyncValue at the given index.
  AsyncValue* GetResultAt(int index) const { return GetResults()[index]; }

  void SetAttributeSection(ArrayRef<uint8_t> attribute_section) {
    attribute_section_ = attribute_section;
  }

  // Add a new argument to the KernelFrame.
  void AddArg(AsyncValue* async_value) {
    assert(num_results_ == -1 &&
           "Must call AddArg before calling SetNumResults");
    AsyncValueOrAttribute value;
    value.async_value = async_value;
    async_value_or_attrs_.push_back(value);
    ++num_arguments_;
  }

  // Add a new attribute to the KernelFrame.
  void AddAttribute(const void* attr) {
    assert(num_results_ != -1 &&
           "Must call SetNumResults before calling AddAttribute");
    AsyncValueOrAttribute value;
    value.attr = attr;
    async_value_or_attrs_.push_back(value);
  }

  // Set the number of results expected.
  void SetNumResults(size_t n) {
    assert(num_arguments_ == async_value_or_attrs_.size());
    assert(num_results_ == -1);
    num_results_ = n;
    async_value_or_attrs_.resize(async_value_or_attrs_.size() + n);
  }

  // Set the location.
  void SetLocation(const Location& location) {
    exec_ctx_.set_location(location);
  }

  // Clear all fields.
  void Reset() {
    async_value_or_attrs_.clear();
    num_arguments_ = 0;
    num_results_ = -1;
  }
};

// RAIIKernelFrame is like KernelFrame, but adds a ref to each contained value
// upon construction, and drops the refs on destruction. This is useful when
// implementing async kernels.
class RAIIKernelFrame : public KernelFrame {
 public:
  RAIIKernelFrame() = delete;
  RAIIKernelFrame(const KernelFrame& frame) : KernelFrame(frame) {
    AddRefAll();
  }

  RAIIKernelFrame(const RAIIKernelFrame& that) : KernelFrame(that) {
    AddRefAll();
  }
  RAIIKernelFrame(RAIIKernelFrame&& that) : KernelFrame(std::move(that)) {}

  ~RAIIKernelFrame() {
    // async_value_or_attrs_ is empty when this object has been moved from.
    if (!async_value_or_attrs_.empty()) DropRefAll();
  }

 private:
  // Increment the refcounts of all arguments and results.
  void AddRefAll() const {
    for (auto* v : GetAsyncValues(0, num_arguments_ + num_results_)) {
      v->AddRef();
    }
  }

  // Decrement the refcounts of all arguments and results.
  void DropRefAll() const {
    for (auto* v : GetAsyncValues(0, num_arguments_ + num_results_)) {
      v->DropRef();
    }
  }
};

// Implementation details

inline void KernelFrame::AssertArity(int num_arguments, int num_attributes,
                                     int num_results) const {
  assert(num_arguments_ == num_arguments);
  assert(GetNumAttributes() == num_attributes);
  assert(GetNumResults() == num_results);
}

}  // namespace tfrt

#endif  // TFRT_HOST_CONTEXT_KERNEL_CONTEXT_H_
