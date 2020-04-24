// Copyright 2020 The TensorFlow Runtime Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

//===- bef_to_mlir.cc -----------------------------------------------------===//
//
// This file implements ConvertBEFToMLIR for the BEFToMLIR library.
// The converter is implemented in three phases. The first phase reads all
// BEF sections other than the Functions section and keeps all strings, types,
// and attributes as well as their offsets or indices. The second phase reads
// all the functions and converts them to MLIR regions without resolving nested
// regions. The third phases resolves all functions as either top level MLIR
// functions or nested regions, and returns the MLIR module.
//
//===----------------------------------------------------------------------===//

#include "tfrt/bef_converter/bef_to_mlir.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/MathExtras.h"
#include "mlir/IR/Attributes.h"
#include "mlir/IR/Function.h"
#include "mlir/IR/Identifier.h"
#include "mlir/IR/Location.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/Module.h"
#include "mlir/IR/StandardTypes.h"
#include "mlir/Parser.h"
#include "mlir/Support/LogicalResult.h"
#include "tfrt/support/bef_encoding.h"
#include "tfrt/support/bef_reader.h"
#include "tfrt/support/byte_order.h"

namespace tfrt {
namespace {

class BEFSections {
 public:
  BEFSections()
      : sections_(static_cast<uint8_t>(BEFSectionID::kNumSectionIDs)) {}

  ArrayRef<uint8_t> Get(BEFSectionID section_id) {
    return sections_.at(static_cast<uint8_t>(section_id));
  }

  void Set(BEFSectionID section_id, ArrayRef<uint8_t> section_data) {
    sections_.at(static_cast<uint8_t>(section_id)) = section_data;
  }

 private:
  std::vector<ArrayRef<uint8_t>> sections_;
};

// This struct keeps the track of properties of a function (eg, offset, name,
// argument/result types, and kind).
struct BEFFunction {
  BEFFunction(size_t offset, string_view name, FunctionKind kind)
      : function_offset(offset), name(name), kind(kind) {}

  size_t function_offset;
  string_view name;
  FunctionKind kind;

  SmallVector<mlir::Type, 4> argument_types;
  SmallVector<mlir::Type, 4> result_types;

  // Named functions are actual MLIR functions (eg. a mlir::FuncOp) in the MLIR
  // program. It may be a concrete function with region bodies and may also be
  // an external function with no function body (eg. a NativeFunction). Unnamed
  // functions are inlined regions in the MLIR program.
  bool IsNamedFunction() const { return !name.empty(); }
  bool IsNativeFunction() const {
    return kind == FunctionKind::kNativeFunction;
  }
};

// This struct keeps the information of a BEF file.
struct BEFFile {
  explicit BEFFile(mlir::Location loc) : location(loc) {}

  mlir::Location location;

  SmallVector<string_view, 4> location_filenames;

  llvm::DenseMap<size_t, mlir::Location> location_positions;

  llvm::DenseMap<size_t, string_view> strings;

  llvm::DenseMap<size_t, mlir::Attribute> attributes;

  std::vector<string_view> kernels;

  std::vector<mlir::Type> types;

  std::vector<BEFFunction> function_index;

  // Returns the filename at `index` into LocationFilename section.
  llvm::Optional<string_view> GetLocationFilename(int index) const {
    if (index < location_filenames.size()) return location_filenames[index];
    return llvm::None;
  }

  // Returns the location at `offset` into LocationPosition section.
  llvm::Optional<mlir::Location> GetLocationPosition(size_t offset) const {
    auto iter = location_positions.find(offset);
    if (iter != location_positions.end()) return iter->second;
    return llvm::None;
  }

  // Returns the string at `offset` into Strings section.
  llvm::Optional<string_view> GetString(size_t offset) const {
    auto str_iter = strings.find(offset);
    if (str_iter != strings.end()) return str_iter->second;
    return llvm::None;
  }

  // Returns the attributes at `offset` into Attributes section, or a null
  // attribute if it cannot find one.
  mlir::Attribute GetAttribute(size_t offset) const {
    return attributes.lookup(offset);
  }

  // Returns the type at `index` into Types section, or a null
  // type if it cannot find one.
  mlir::Type GetType(int index) const {
    if (index < types.size()) return types[index];
    return mlir::Type();
  }

  // Returns the function at `index` into FunctionIndex section, or a nullptr if
  // it cannot find one.
  const BEFFunction* GetFunction(int index) const {
    if (index < function_index.size()) return &function_index[index];
    return nullptr;
  }
};

// This struct keeps the MLIR region bodies and references to nested regions for
// MLIR operations.
struct BEFFunctionContext {
  // Keeps the region body for each function. Function definitions and nested
  // regions are resolved after processing all functions.
  std::vector<std::pair<mlir::Location, std::unique_ptr<mlir::Region>>> regions;

  // Keeps the indices to FunctionIndex for each MLIR operation if it has nested
  // regions. Nested regions will be resolved after processing all functions.
  llvm::DenseMap<mlir::Operation*, ArrayRef<uint32_t>> region_references;
};

void EmitError(mlir::Location loc, string_view message) {
  mlir::emitError(loc) << message;
}

void EmitWarning(mlir::Location loc, string_view message) {
  mlir::emitWarning(loc) << message;
}

// Reads an integer N, and then reads the following N integer items from
// `reader`. Performs action on each item.
mlir::LogicalResult ReadIntArray(BEFReader* reader,
                                 std::vector<size_t>* items) {
  size_t num_items;
  if (reader->ReadInt(&num_items)) return mlir::failure();
  items->clear();
  items->reserve(num_items);
  for (int i = 0; i < num_items; ++i) {
    size_t item;
    if (reader->ReadInt(&item)) return mlir::failure();
    items->push_back(item);
  }
  return mlir::success();
}

// This class reads a BEF file and converts it to a mlir module.
class BEFToMLIRConverter {
 public:
  BEFToMLIRConverter(ArrayRef<uint8_t> file, mlir::Location location,
                     mlir::MLIRContext* context)
      : file_reader_(file), bef_file_(location), context_(*context) {}

  // The following methods read the given BEF sections and populates interesting
  // properties in `bef_file_`.
  mlir::LogicalResult ReadHeader();
  mlir::LogicalResult ReadSections(BEFSections* sections);
  mlir::LogicalResult ReadFormatVersion(ArrayRef<uint8_t> format_version);
  mlir::LogicalResult ReadLocationFilenames(
      ArrayRef<uint8_t> location_filenames);
  mlir::LogicalResult ReadLocationPositions(
      ArrayRef<uint8_t> location_positions);
  mlir::LogicalResult ReadStrings(ArrayRef<uint8_t> strings);
  mlir::LogicalResult ReadAttributes(ArrayRef<uint8_t> attributes,
                                     ArrayRef<uint8_t> attribute_types);
  mlir::LogicalResult ReadKernels(ArrayRef<uint8_t> kernels);
  mlir::LogicalResult ReadTypes(ArrayRef<uint8_t> types);
  mlir::LogicalResult ReadFunctionIndex(ArrayRef<uint8_t> function_index);
  mlir::LogicalResult ReadFunctions(ArrayRef<uint8_t> functions,
                                    ArrayRef<uint8_t> attribute_names,
                                    ArrayRef<uint8_t> register_types,
                                    BEFFunctionContext* function_context);

  // Resolves regions in `function_context` as either top level MLIR functions
  // in the MLIR module or nested regions for some MLIR operations, and saves
  // the top level functions in `module`.
  mlir::LogicalResult ResolveFunctions(BEFFunctionContext* function_context,
                                       mlir::ModuleOp module);

 private:
  // Reads the next available section. Unrecognized sections are dropped
  // silently.
  mlir::LogicalResult ReadNextSection(BEFSections* sections);

  // Reads null terminated strings in `section_data`. For each string, `action`
  // is called with its offset and content.
  mlir::LogicalResult ReadNullTerminatedStrings(
      ArrayRef<uint8_t> section_data,
      llvm::function_ref<void(size_t, string_view)> action);

  // Reads a section of offsets into Strings section. For each offset, `action`
  // is called with the string it pointes to.
  mlir::LogicalResult ReadStringOffsetSection(
      ArrayRef<uint8_t> section_data,
      llvm::function_ref<void(string_view)> action);

  // Create a BEF function.
  mlir::FuncOp CreateBEFFuncOp(const mlir::Location& location,
                               const BEFFunction& bef_function,
                               std::unique_ptr<mlir::Region> region);
  // Create a native function which is an external MLIR function.
  mlir::FuncOp CreateNativeFuncOp(const mlir::Location& loc,
                                  const BEFFunction& bef_function);

  BEFReader file_reader_;
  BEFFile bef_file_;
  mlir::MLIRContext& context_;
};

// This reads attributes from attributes sections with the type information from
// attribute types section.
class BEFAttributeReader {
 public:
  BEFAttributeReader(ArrayRef<uint8_t> attributes, const BEFFile& bef_file,
                     mlir::MLIRContext* context)
      : attributes_(attributes), bef_file_(bef_file), context_(*context) {}

  // Reads an attribute at `offset` into attributes section. On error, it
  // returns a null attribute.
  mlir::Attribute ReadAttribute(size_t attribute_type, size_t offset);

 private:
  // Reads a MLIR attribute from BEF.
  mlir::Attribute ReadAttribute(BEFReader* reader, size_t attribute_type);

  // Reads a standard attribute of `type`.
  mlir::Attribute ReadStandardAttribute(BEFReader* reader, mlir::Type type);

  // Reads a bool value, which is one byte.
  mlir::Attribute ReadBoolAttribute(BEFReader* reader);

  // Reads a string attribute.
  mlir::StringAttr ReadStringAttribute(BEFReader* reader);

  // Reads a type attribute.
  mlir::TypeAttr ReadTypeAttribute(BEFReader* reader);

  // Reads a flat array attribute with elements of type `element_type`.
  mlir::ArrayAttr ReadFlatArrayAttribute(BEFReader* reader,
                                         size_t element_type);

  // Reads an offset array at `offset`.
  mlir::ArrayAttr ReadOffsetArrayAttribute(BEFReader* reader);

  // Reads a integer value of `bit_width` from `reader`.
  llvm::Optional<llvm::APInt> ReadIntegerAttribute(BEFReader* reader,
                                                   int bit_width);

  // Reads a float value of `bit_width` from `reader`.
  llvm::Optional<llvm::APFloat> ReadFloatAttribute(BEFReader* reader,
                                                   int bit_width);

  // Reads a dense element attribute at `offset`.
  mlir::DenseElementsAttr ReadDenseElementsAttribute(BEFReader* reader);

  // Decode an encoded dtype.
  mlir::Type DecodeTypeAttribute(uint8_t encoded_dtype);

  // Reads the length of a string attribute or an array attribute. The
  // length is encoded using a modified VBR encoding and placed right before
  // the attribute.
  size_t ReadLength(size_t offset) {
    // The first byte of the VBR stream is at offset - 1, second at offset - 2,
    // etc.
    assert(offset > 0);
    offset--;
    size_t value = 0;
    while ((attributes_[offset] & 0x80) != 0) {
      value = (value << 7) | (attributes_[offset] & 0x7F);
      assert(offset > 0);
      offset--;
    }
    value = (value << 7) | (attributes_[offset] & 0x7F);
    return value;
  }

  ArrayRef<uint8_t> attributes_;
  const BEFFile& bef_file_;
  mlir::MLIRContext& context_;
};

// This reads BEF functions in a BEF file and creates functions in an MLIR
// module.
class BEFFunctionReader {
 public:
  BEFFunctionReader(
      ArrayRef<uint8_t> function, const BEFFile& bef_file,
      const BEFFunction& bef_function,
      llvm::DenseMap<mlir::Operation*, ArrayRef<uint32_t>>* region_references,
      mlir::MLIRContext* context)
      : function_reader_(function),
        bef_file_(bef_file),
        bef_function_(bef_function),
        region_references_(*region_references),
        context_(*context),
        location_(mlir::UnknownLoc::get(&context_)) {}

  // Reads a function and returns the location and region body. Returns None on
  // errors. Nested regions are not resolved yet.
  llvm::Optional<std::pair<mlir::Location, std::unique_ptr<mlir::Region>>>
  ReadFunction(BEFReader* attribute_names, BEFReader* register_types);

 private:
  // RegisterInfo keeps properties of a register used in this function (eg.
  // type, value, uses).
  struct RegisterInfo {
    RegisterInfo(mlir::Type type, size_t num_uses)
        : type(type), num_uses(num_uses) {}

    mlir::Type type;
    size_t num_uses;

    // `usedbys` contains the indices to the kernel_table_.
    ArrayRef<uint32_t> usedbys;
    // `value` will be assigned after processing the defining MLIR operation.
    mlir::Value value = nullptr;
  };

  struct KernelTableEntry {
    size_t offset;
    size_t num_operands;
  };

  mlir::LogicalResult ReadRegisterTable(BEFReader* register_types);
  mlir::LogicalResult ReadKernelTable();
  mlir::LogicalResult ReadResultRegs();

  // Reads kernels from `kernels` which contains kernel entries of all kernels
  // in this function, and inserts them as MLIR operations into `block`.
  // Attribute names are read from `attribute_names`.
  mlir::LogicalResult ReadKernels(ArrayRef<uint32_t> kernels,
                                  BEFReader* attribute_names,
                                  mlir::Block* block);

  // Reads the arguments pseudo op that defines the registers for function input
  // arguments.
  mlir::LogicalResult ReadArgumentsPseudoKernel(
      ArrayRef<uint32_t> kernels,
      ArrayRef<mlir::BlockArgument> entry_arguments);

  // Reads a kernel at `offset` from `kernels` and returns the corresponding
  // MLIR operation. Attribute names are read from `attribute_names`. Returns
  // nullptr on errors.
  mlir::Operation* ReadKernel(ArrayRef<uint32_t> kernels, size_t offset,
                              BEFReader* attribute_names);

  // Add a register definition.
  mlir::LogicalResult AddDefinition(mlir::Value value, size_t register_index);
  RegisterInfo& GetRegister(int register_index);

  BEFReader function_reader_;
  const BEFFile& bef_file_;
  const BEFFunction& bef_function_;
  llvm::DenseMap<mlir::Operation*, ArrayRef<uint32_t>>& region_references_;
  mlir::MLIRContext& context_;

  mlir::Location location_;
  std::vector<RegisterInfo> register_table_;
  std::vector<KernelTableEntry> kernel_table_;
  SmallVector<int, 2> result_regs_;
};

mlir::LogicalResult BEFToMLIRConverter::ReadHeader() {
  // Read magic number.
  uint8_t byte;
  if (file_reader_.ReadByte(&byte) || (byte != kBEFMagic1) ||
      file_reader_.ReadByte(&byte) || (byte != kBEFMagic2)) {
    return mlir::failure();
  }
  return mlir::success();
}

mlir::LogicalResult BEFToMLIRConverter::ReadSections(BEFSections* sections) {
  // Read all sections without any processing.
  while (!file_reader_.Empty()) {
    if (mlir::failed(ReadNextSection(sections))) return mlir::failure();
  }

  if (sections->Get(BEFSectionID::kAttributeTypes).empty() ||
      sections->Get(BEFSectionID::kAttributeNames).empty() ||
      sections->Get(BEFSectionID::kRegisterTypes).empty()) {
    EmitWarning(
        bef_file_.location,
        "Missing AttributeTypes, AttributeNames or RegisterTypes sections.");
  }
  return mlir::success();
}

mlir::LogicalResult BEFToMLIRConverter::ReadNextSection(BEFSections* sections) {
  uint8_t section_id;
  ArrayRef<uint8_t> section_data;
  if (file_reader_.ReadSection(&section_id, &section_data))
    return mlir::failure();
  file_reader_.SkipPast(section_data);
  sections->Set(static_cast<BEFSectionID>(section_id), section_data);
  return mlir::success();
}

mlir::LogicalResult BEFToMLIRConverter::ReadFormatVersion(
    ArrayRef<uint8_t> format_version) {
  // Read and check the version byte.
  BEFReader format_version_reader(format_version);
  uint8_t version;
  if (format_version_reader.ReadByte(&version) || version != kBEFVersion0)
    return mlir::failure();
  return mlir::success();
}

mlir::LogicalResult BEFToMLIRConverter::ReadNullTerminatedStrings(
    ArrayRef<uint8_t> section_data,
    llvm::function_ref<void(size_t, string_view)> action) {
  size_t original_size = section_data.size();
  while (!section_data.empty()) {
    size_t offset = original_size - section_data.size();
    // Find the null terminated string.
    string_view str(reinterpret_cast<const char*>(section_data.data()));
    if (str.size() >= section_data.size()) return mlir::failure();
    action(offset, str);
    // Skip the string and the null terminator.
    section_data = section_data.drop_front(str.size() + 1);
  }
  return mlir::success();
}

mlir::LogicalResult BEFToMLIRConverter::ReadLocationFilenames(
    ArrayRef<uint8_t> location_filenames) {
  return ReadNullTerminatedStrings(
      location_filenames, [this](size_t offset, string_view str) {
        bef_file_.location_filenames.push_back(str);
      });
}

mlir::LogicalResult BEFToMLIRConverter::ReadLocationPositions(
    ArrayRef<uint8_t> location_positions) {
  BEFReader location_positions_reader(location_positions);
  unsigned original_size = location_positions_reader.file().size();
  while (!location_positions_reader.Empty()) {
    size_t location_filename_index;
    size_t line_number;
    size_t column_number;
    size_t offset = original_size - location_positions_reader.file().size();

    // Read the filename index, line number and column number.
    if (location_positions_reader.ReadInt(&location_filename_index) ||
        location_positions_reader.ReadInt(&line_number) ||
        location_positions_reader.ReadInt(&column_number))
      return mlir::failure();

    // Find the filename string in LocationFilenames section.
    auto filename = bef_file_.GetLocationFilename(location_filename_index);
    if (!filename) return mlir::failure();

    // Populates `bef_file_` with locations.
    bef_file_.location_positions.insert(
        {offset, mlir::FileLineColLoc::get(*filename, line_number,
                                           column_number, &context_)});
  }
  return mlir::success();
}

mlir::LogicalResult BEFToMLIRConverter::ReadStrings(ArrayRef<uint8_t> strings) {
  return ReadNullTerminatedStrings(strings,
                                   [this](size_t offset, string_view str) {
                                     bef_file_.strings[offset] = str;
                                   });
}

mlir::LogicalResult BEFToMLIRConverter::ReadAttributes(
    ArrayRef<uint8_t> attributes, ArrayRef<uint8_t> attribute_types) {
  // If AttributeTypes section does not exist, dummy attributes will be used.
  if (attribute_types.empty()) return mlir::success();

  BEFReader attribute_types_reader(attribute_types);
  size_t num_attributes;
  if (attribute_types_reader.ReadInt(&num_attributes)) return mlir::failure();

  BEFAttributeReader attribute_reader(attributes, bef_file_, &context_);
  for (int i = 0; i < num_attributes; ++i) {
    // Read the offset and attribute_type of the attribute in attribute types
    // section and find out the corresponding attribute in attributes section.
    size_t offset;
    size_t attribute_type;
    if (attribute_types_reader.ReadInt(&offset) ||
        attribute_types_reader.ReadInt(&attribute_type))
      return mlir::failure();

    bef_file_.attributes.insert(
        {offset, attribute_reader.ReadAttribute(attribute_type, offset)});
  }
  return mlir::success();
}

mlir::LogicalResult BEFToMLIRConverter::ReadStringOffsetSection(
    ArrayRef<uint8_t> section_data,
    llvm::function_ref<void(string_view)> action) {
  BEFReader reader(section_data);
  std::vector<size_t> offsets;
  if (mlir::failed(ReadIntArray(&reader, &offsets))) return mlir::failure();
  for (auto offset : offsets) {
    auto str = bef_file_.GetString(offset);
    if (!str) return mlir::failure();
    action(*str);
  }
  return mlir::success();
}

mlir::LogicalResult BEFToMLIRConverter::ReadKernels(ArrayRef<uint8_t> kernels) {
  // Read the offsets of kernel names and keeps the kernel names in
  // `bef_file_`.
  return ReadStringOffsetSection(
      kernels, [this](string_view str) { bef_file_.kernels.push_back(str); });
}

mlir::LogicalResult BEFToMLIRConverter::ReadTypes(ArrayRef<uint8_t> types) {
  // Read the offsets of type names and keeps the parsed types in
  // `bef_file_`.
  return ReadStringOffsetSection(types, [this](string_view str) {
    // Keeps the parsed type.
    bef_file_.types.push_back(mlir::parseType(str, &context_));
  });
}

mlir::LogicalResult BEFToMLIRConverter::ReadFunctionIndex(
    ArrayRef<uint8_t> function_index) {
  BEFReader function_index_reader(function_index);
  size_t function_count;
  if (function_index_reader.ReadInt(&function_count)) return mlir::failure();
  for (int i = 0; i < function_count; ++i) {
    uint8_t function_kind;
    size_t function_offset, name_offset;
    if (function_index_reader.ReadByte(&function_kind) ||
        function_index_reader.ReadInt(&function_offset) ||
        function_index_reader.ReadInt(&name_offset))
      return mlir::failure();

    // Add this function to `bef_file_`.
    bef_file_.function_index.emplace_back(
        function_offset, bef_file_.GetString(name_offset).getValue(),
        static_cast<FunctionKind>(function_kind));
    auto& bef_function = bef_file_.function_index.back();

    // And populate argument/result types of this function.
    auto read_types =
        [this, &function_index_reader](SmallVector<mlir::Type, 4>* out) {
          std::vector<size_t> indices;
          if (mlir::failed(ReadIntArray(&function_index_reader, &indices)))
            return mlir::failure();
          for (auto type_index : indices) {
            auto type = bef_file_.GetType(type_index);
            if (!type) return mlir::failure();
            out->push_back(type);
          }
          return mlir::success();
        };

    if (mlir::failed(read_types(&bef_function.argument_types)) ||
        mlir::failed(read_types(&bef_function.result_types)))
      return mlir::failure();
  }
  return mlir::success();
}

mlir::LogicalResult BEFToMLIRConverter::ReadFunctions(
    ArrayRef<uint8_t> functions, ArrayRef<uint8_t> attribute_names,
    ArrayRef<uint8_t> register_types, BEFFunctionContext* function_context) {
  // Set up the readers for attribute names and register types. Attribute names
  // and register types will be read if they exist.
  BEFReader attribute_names_reader(attribute_names);
  if (!attribute_names_reader.Empty()) {
    size_t num_attribute_tables;
    (void)attribute_names_reader.ReadInt(&num_attribute_tables);
  }
  BEFReader register_types_reader(register_types);
  if (!register_types_reader.Empty()) {
    size_t num_reg_type_tables;
    (void)register_types_reader.ReadInt(&num_reg_type_tables);
  }

  // Process all functions.
  for (const auto& bef_function : bef_file_.function_index) {
    if (bef_function.IsNativeFunction()) {
      // Special handling for native functions.
      function_context->regions.push_back(
          {mlir::UnknownLoc::get(&context_), nullptr});
    } else {
      // BEF functions are handled here. It reads the function body from the
      // Functions section in BEF.
      auto function = functions.drop_front(bef_function.function_offset);
      BEFFunctionReader function_reader(function, bef_file_, bef_function,
                                        &function_context->region_references,
                                        &context_);
      auto loc_and_region = function_reader.ReadFunction(
          &attribute_names_reader, &register_types_reader);
      if (!loc_and_region) return mlir::failure();
      function_context->regions.push_back(std::move(loc_and_region).getValue());
    }
  }
  return mlir::success();
}

mlir::FuncOp BEFToMLIRConverter::CreateBEFFuncOp(
    const mlir::Location& location, const BEFFunction& bef_function,
    std::unique_ptr<mlir::Region> region) {
  // Use return_op's operand types as function result types.
  auto& return_op = region->front().back();
  SmallVector<mlir::Type, 4> result_types(return_op.operand_type_begin(),
                                          return_op.operand_type_end());

  // If it is a named function, create a top level mlir function.
  auto function_type = mlir::FunctionType::get(bef_function.argument_types,
                                               result_types, &context_);
  auto func_op =
      mlir::FuncOp::create(location, bef_function.name, function_type);
  func_op.getBody().takeBody(*region);

  return func_op;
}

mlir::FuncOp BEFToMLIRConverter::CreateNativeFuncOp(
    const mlir::Location& location, const BEFFunction& bef_function) {
  assert(bef_function.kind == FunctionKind::kNativeFunction);
  auto type = mlir::FunctionType::get(bef_function.argument_types,
                                      bef_function.result_types, &context_);
  auto func_op = mlir::FuncOp::create(location, bef_function.name, type);
  func_op.setAttr("hex.native", mlir::UnitAttr::get(&context_));
  return func_op;
}

mlir::LogicalResult BEFToMLIRConverter::ResolveFunctions(
    BEFFunctionContext* function_context, mlir::ModuleOp module) {
  // Resolve top level functions.
  for (int i = 0; i < bef_file_.function_index.size(); ++i) {
    auto& bef_function = bef_file_.function_index[i];
    if (bef_function.IsNamedFunction()) {
      auto& region = function_context->regions.at(i);
      if (bef_function.IsNativeFunction()) {
        // Resolve native functions.
        assert(region.second == nullptr);
        module.push_back(CreateNativeFuncOp(region.first, bef_function));
      } else {
        // Resolve BEF functions.
        assert(region.second != nullptr);
        module.push_back(CreateBEFFuncOp(region.first, bef_function,
                                         std::move(region.second)));
      }
    }
  }

  // Resolve nested regions.
  for (const auto& iter : function_context->region_references) {
    auto* op = iter.first;
    const auto& region_indices = iter.second;
    assert(op->getNumRegions() == region_indices.size());
    for (int i = 0; i < region_indices.size(); ++i) {
      auto& child_region =
          function_context->regions.at(region_indices[i]).second;
      assert(child_region != nullptr);
      op->getRegion(i).takeBody(*child_region);
      child_region = nullptr;
    }
  }

  // Check all regions are resolved.
  for (const auto& loc_and_region : function_context->regions) {
    if (loc_and_region.second != nullptr) {
      EmitError(bef_file_.location, "Failed to resolve functions.");
      return mlir::failure();
    }
  }

  return mlir::success();
}

mlir::Attribute BEFAttributeReader::ReadAttribute(size_t attribute_type,
                                                  size_t offset) {
  BEFReader reader(attributes_.drop_front(offset));
  return ReadAttribute(&reader, attribute_type);
}

mlir::Attribute BEFAttributeReader::ReadAttribute(BEFReader* reader,
                                                  size_t attribute_type) {
  AttributeTypeID attribute_type_id =
      static_cast<AttributeTypeID>(attribute_type & kAttributeTypeIDMask);
  size_t payload = attribute_type >> kAttributeTypeIDShift;

  switch (attribute_type_id) {
    case AttributeTypeID::kStandardAttribute:
      return ReadStandardAttribute(reader, bef_file_.GetType(payload));
    case AttributeTypeID::kBoolAttribute:
      return ReadBoolAttribute(reader);
    case AttributeTypeID::kStringAttribute:
      return ReadStringAttribute(reader);
    case AttributeTypeID::kTypeAttribute:
      return ReadTypeAttribute(reader);
    case AttributeTypeID::kDenseElementsAttribute:
      return ReadDenseElementsAttribute(reader);
    case AttributeTypeID::kFlatArrayAttribute:
      return ReadFlatArrayAttribute(reader, payload);
    case AttributeTypeID::kOffsetArrayAttribute:
      return ReadOffsetArrayAttribute(reader);
  }

  EmitError(bef_file_.location, "Unknown attribute type");
  return {};
}

mlir::Attribute BEFAttributeReader::ReadStandardAttribute(BEFReader* reader,
                                                          mlir::Type type) {
  if (auto int_type = type.dyn_cast<mlir::IntegerType>()) {
    auto int_attr = ReadIntegerAttribute(reader, int_type.getWidth());
    if (!int_attr) return {};
    return mlir::IntegerAttr::get(int_type, *int_attr);
  }

  if (auto float_type = type.dyn_cast<mlir::FloatType>()) {
    auto float_attr = ReadFloatAttribute(reader, float_type.getWidth());
    if (!float_attr) return {};
    return mlir::FloatAttr::get(float_type, *float_attr);
  }

  EmitError(bef_file_.location, "Unknown standard attribute type");
  return {};
}

mlir::Attribute BEFAttributeReader::ReadBoolAttribute(BEFReader* reader) {
  uint8_t byte;
  if (reader->ReadByte(&byte)) return {};
  return mlir::BoolAttr::get(static_cast<bool>(byte), &context_);
}

mlir::StringAttr BEFAttributeReader::ReadStringAttribute(BEFReader* reader) {
  assert(reader->file().data() > attributes_.data());
  size_t offset = reader->file().data() - attributes_.data();

  auto length = ReadLength(offset);
  auto string_attr = mlir::StringAttr::get(
      string_view(reinterpret_cast<const char*>(&attributes_[offset]), length),
      &context_);
  return string_attr;
}

mlir::TypeAttr BEFAttributeReader::ReadTypeAttribute(BEFReader* reader) {
  uint8_t byte;
  if (reader->ReadByte(&byte)) return {};
  auto kind = static_cast<AttributeKind>(byte);

  switch (kind) {
    case AttributeKind::kI1:
      return mlir::TypeAttr::get(mlir::IntegerType::get(1, &context_));
    case AttributeKind::kI32:
      return mlir::TypeAttr::get(mlir::IntegerType::get(32, &context_));
    case AttributeKind::kI64:
      return mlir::TypeAttr::get(mlir::IntegerType::get(64, &context_));
    case AttributeKind::kF16:
      return mlir::TypeAttr::get(mlir::FloatType::getF16(&context_));
    case AttributeKind::kF32:
      return mlir::TypeAttr::get(mlir::FloatType::getF32(&context_));
    case AttributeKind::kF64:
      return mlir::TypeAttr::get(mlir::FloatType::getF64(&context_));
    default:
      llvm_unreachable("unsupported type attribute.");
  }
}

mlir::ArrayAttr BEFAttributeReader::ReadFlatArrayAttribute(
    BEFReader* reader, size_t element_type) {
  assert(reader->file().data() > attributes_.data());
  size_t offset = reader->file().data() - attributes_.data();

  auto length = ReadLength(offset);
  if (length == 0) return mlir::ArrayAttr::get({}, &context_);

  SmallVector<mlir::Attribute, 8> elements;
  elements.reserve(length);
  for (int i = 0; i < length; ++i)
    elements.push_back(ReadAttribute(reader, element_type));

  return mlir::ArrayAttr::get(elements, &context_);
}

mlir::ArrayAttr BEFAttributeReader::ReadOffsetArrayAttribute(
    BEFReader* reader) {
  assert(reader->file().data() > attributes_.data());
  size_t offset = reader->file().data() - attributes_.data();

  auto length = ReadLength(offset);
  if (length == 0) return mlir::ArrayAttr::get({}, &context_);

  assert(length * 4 + offset <= attributes_.size());
  auto element_descriptors = llvm::makeArrayRef(
      reinterpret_cast<const AttributeDescriptor*>(reader->file().data()),
      length);

  SmallVector<mlir::Attribute, 8> elements;
  elements.reserve(length);

  // Elements are already read as they are encoded before this array attribute.
  for (auto& descriptor : element_descriptors)
    elements.push_back(bef_file_.GetAttribute(descriptor.offset));

  return mlir::ArrayAttr::get(elements, &context_);
}

mlir::Type BEFAttributeReader::DecodeTypeAttribute(uint8_t encoded_dtype) {
  switch (static_cast<AttributeKind>(encoded_dtype)) {
    case AttributeKind::kI1:
      return mlir::IntegerType::get(1, &context_);
    case AttributeKind::kI32:
      return mlir::IntegerType::get(32, &context_);
    case AttributeKind::kI64:
      return mlir::IntegerType::get(64, &context_);
    case AttributeKind::kF16:
      return mlir::FloatType::getF16(&context_);
    case AttributeKind::kF32:
      return mlir::FloatType::getF32(&context_);
    case AttributeKind::kF64:
      return mlir::FloatType::getF64(&context_);
    default:
      llvm_unreachable("unknown type attribute.");
  }
}

mlir::DenseElementsAttr BEFAttributeReader::ReadDenseElementsAttribute(
    BEFReader* reader) {
  uint64_t dtype_and_shape_rank;
  uint64_t elements_count;
  if (reader->ReadInt8(&dtype_and_shape_rank) ||
      reader->ReadInt8(&elements_count))
    return {};

  mlir::Type dtype = DecodeTypeAttribute(dtype_and_shape_rank >> 56);
  uint64_t shape_rank = (dtype_and_shape_rank << 8) >> 8;

  // Read shape elements.
  SmallVector<int64_t, 4> shape_elts;
  shape_elts.reserve(shape_rank);
  for (size_t i = 0; i != shape_rank; ++i) {
    uint64_t dim;
    if (reader->ReadInt8(&dim)) return {};
    shape_elts.push_back(dim);
  }

  // Read elements.
  SmallVector<mlir::Attribute, 8> elements;
  elements.reserve(elements_count);
  for (int i = 0; i != elements_count; ++i) {
    elements.push_back(ReadStandardAttribute(reader, dtype));
  }

  // TODO(zhangqiaorjc): Distinguish between vector and tensor type.
  auto rank_tensor_shape = mlir::RankedTensorType::get(shape_elts, dtype);
  return mlir::DenseElementsAttr::get(rank_tensor_shape, elements);
}

llvm::Optional<llvm::APInt> BEFAttributeReader::ReadIntegerAttribute(
    BEFReader* reader, int bit_width) {
  int num_bytes = 0;
  switch (bit_width) {
    default:
      EmitError(bef_file_.location, "Unknown integer attribute width");
      return llvm::None;
    case 1:
      num_bytes = 1;
      break;
    case 32:
      num_bytes = 4;
      break;
    case 64:
      num_bytes = 8;
      break;
  }

  // TODO(chky): Check alignment
  uint64_t value = 0;
  for (int i = 0; i < num_bytes; ++i) {
    uint8_t byte;
    if (reader->ReadByte(&byte)) return llvm::None;
    value = value | (static_cast<uint64_t>(byte) << (8 * i));
  }
  return llvm::APInt(bit_width, value);
}

llvm::Optional<llvm::APFloat> BEFAttributeReader::ReadFloatAttribute(
    BEFReader* reader, int bit_width) {
  if (bit_width == 32) {
    auto value = ReadIntegerAttribute(reader, bit_width);
    if (!value) return llvm::None;
    return llvm::APFloat(value->bitsToFloat());
  }

  EmitError(bef_file_.location, "Unknown float attribute width");
  return llvm::None;
}

llvm::Optional<std::pair<mlir::Location, std::unique_ptr<mlir::Region>>>
BEFFunctionReader::ReadFunction(BEFReader* attribute_names,
                                BEFReader* register_types) {
  auto emit_error = [this](string_view message) {
    EmitError(bef_file_.location, message);
    return llvm::None;
  };

  // Read function location.
  size_t location_position_offset;
  if (function_reader_.ReadInt(&location_position_offset))
    return emit_error("Failed to read function location");
  auto location = bef_file_.GetLocationPosition(location_position_offset);
  if (!location) return emit_error("Failed to read function location");
  location_ = *location;

  if (mlir::failed(ReadRegisterTable(register_types)))
    return emit_error("Failed to read register table.");
  if (mlir::failed(ReadKernelTable()))
    return emit_error("Failed to read kernel table.");
  if (mlir::failed(ReadResultRegs()))
    return emit_error("Failed to read result regs.");

  // Create a region body for this function.
  auto region = std::make_unique<mlir::Region>();
  region->push_back(new mlir::Block());
  auto* block = &region->back();
  block->addArguments(bef_function_.argument_types);

  // Kernels are 4-byte aligned.
  if (function_reader_.ReadAlignment(kKernelEntryAlignment) ||
      mlir::failed(ReadKernels(
          llvm::makeArrayRef(
              reinterpret_cast<const uint32_t*>(
                  function_reader_.file().begin()),
              function_reader_.file().size() / kKernelEntryAlignment),
          attribute_names, block)))
    return emit_error("Failed to read kernels.");

  return std::pair<mlir::Location, std::unique_ptr<mlir::Region>>{
      location_, std::move(region)};
}

mlir::LogicalResult BEFFunctionReader::ReadRegisterTable(
    BEFReader* register_types) {
  std::vector<size_t> reg_type_indices;
  if (mlir::failed(ReadIntArray(register_types, &reg_type_indices)))
    reg_type_indices.clear();

  std::vector<size_t> reg_uses;
  if (mlir::failed(ReadIntArray(&function_reader_, &reg_uses)))
    return mlir::failure();

  assert(reg_type_indices.empty() ||
         reg_type_indices.size() == reg_uses.size());

  for (int i = 0; i < reg_uses.size(); ++i) {
    mlir::Type type;
    if (!reg_type_indices.empty())
      type = bef_file_.GetType(reg_type_indices.at(i));

    // Use NoneType if no register type info exists.
    if (!type) type = mlir::NoneType::get(&context_);

    // Pre-allocate RegisterInfo so that they can be used in later passes.
    register_table_.push_back(RegisterInfo(type, reg_uses[i]));
  }

  return mlir::success();
}

mlir::LogicalResult BEFFunctionReader::ReadKernelTable() {
  size_t num_kernels;
  if (function_reader_.ReadInt(&num_kernels)) return mlir::failure();
  for (int i = 0; i < num_kernels; ++i) {
    KernelTableEntry entry;
    if (function_reader_.ReadInt(&entry.offset) ||
        function_reader_.ReadInt(&entry.num_operands))
      return mlir::failure();
    kernel_table_.push_back(entry);
  }
  return mlir::success();
}

mlir::LogicalResult BEFFunctionReader::ReadResultRegs() {
  for (int i = 0; i < bef_function_.result_types.size(); ++i) {
    size_t register_index;
    if (function_reader_.ReadInt(&register_index)) return mlir::failure();
    result_regs_.push_back(register_index);
  }
  return mlir::success();
}

mlir::LogicalResult BEFFunctionReader::ReadKernels(ArrayRef<uint32_t> kernels,
                                                   BEFReader* attribute_names,
                                                   mlir::Block* block) {
  size_t num_kernels;
  if (!attribute_names->ReadInt(&num_kernels))
    assert(num_kernels == kernel_table_.size());

  int kernel_start = 0;
  if (!block->getArguments().empty()) {
    // Reads the first op as arguments pseudo op which only defines the argument
    // registers.
    kernel_start = 1;
    if (mlir::failed(
            ReadArgumentsPseudoKernel(kernels, block->getArguments()))) {
      EmitError(bef_file_.location, "Failed to read pseudo.");
      return mlir::failure();
    }

    // pseudo op must not be bef.nonstrict.
    uint8_t pseudo_op_non_strict;
    if (attribute_names->ReadByte(&pseudo_op_non_strict))
      assert(static_cast<SpecialAttribute>(pseudo_op_non_strict) ==
             SpecialAttribute::kUnknown);
  }

  for (int i = kernel_start; i < kernel_table_.size(); ++i) {
    auto offset = kernel_table_[i].offset;
    auto* op = ReadKernel(kernels, offset, attribute_names);
    if (op == nullptr) return mlir::failure();
    block->push_back(op);
  }

  // TODO(chky): check def/use relations.

  // All functions end with a return op.
  mlir::OperationState return_op_state(
      // use enclosing function's location as return op's location.
      location_, "hex.return");

  // Add function's result regs as ReturnOp's operands.
  for (auto result_reg_index : result_regs_) {
    auto result = GetRegister(result_reg_index).value;
    if (result == nullptr) {
      EmitError(bef_file_.location,
                "Using an undefined register in return op.");
      return mlir::failure();
    }
    return_op_state.operands.push_back(result);
  }

  block->push_back(mlir::Operation::create(return_op_state));
  return mlir::success();
}

mlir::LogicalResult BEFFunctionReader::ReadArgumentsPseudoKernel(
    ArrayRef<uint32_t> kernels, ArrayRef<mlir::BlockArgument> entry_arguments) {
  // ArgumentsPseudoOp is the first kernel.
  BEFKernel kernel(kernels.data());

  assert(kernel.num_arguments() == 0);
  assert(kernel.num_attributes() == 0);
  assert(kernel.num_functions() == 0);
  assert(kernel.num_results() == entry_arguments.size() &&
         "PseudoOp not found for function args.");

  // Read results.
  int entry_offset = 0;
  auto results = kernel.GetKernelEntries(entry_offset, kernel.num_results());
  for (int i = 0; i < results.size(); ++i) {
    auto arg = entry_arguments[i];

    // Read the result register and add the definition to register table.
    auto register_index = results[i];
    if (mlir::failed(AddDefinition(arg, register_index)))
      return mlir::failure();
  }

  // Read usedbys.
  entry_offset += results.size();
  for (int i = 0; i < results.size(); ++i) {
    auto register_index = results[i];
    auto& reg_info = GetRegister(register_index);

    auto num_used_bys = kernel.num_used_bys(i);
    reg_info.usedbys = kernel.GetKernelEntries(entry_offset, num_used_bys);
    entry_offset += num_used_bys;
  }

  return mlir::success();
}

mlir::Operation* BEFFunctionReader::ReadKernel(ArrayRef<uint32_t> kernels,
                                               size_t offset,
                                               BEFReader* attribute_names) {
  auto emit_error = [this](string_view message) {
    EmitError(bef_file_.location, message);
    return nullptr;
  };

  // kernel offset is aligned to kKernelEntryAlignment.
  assert(offset % kKernelEntryAlignment == 0);
  BEFKernel kernel(kernels.data() + offset / kKernelEntryAlignment);

  // The first two entry must be kernel_code and kernel_location.
  auto name = bef_file_.kernels.at(kernel.kernel_code());

  auto location =
      bef_file_.GetLocationPosition(kernel.kernel_location()).getValue();

  mlir::OperationState state(location, name);

  // Resolve arguments
  int entry_offset = 0;
  auto arguments =
      kernel.GetKernelEntries(entry_offset, kernel.num_arguments());
  for (auto register_index : arguments) {
    auto value = GetRegister(register_index).value;
    if (value == nullptr) return emit_error("Using undefined registers.");
    state.operands.push_back(value);
  }

  // Resolve special attributes
  uint8_t special_attribute;
  if (!attribute_names->ReadByte(&special_attribute)) {
    if (static_cast<SpecialAttribute>(special_attribute) ==
        SpecialAttribute::kNonStrict) {
      state.addAttribute("bef.nonstrict", mlir::UnitAttr::get(&context_));
    }
  }

  // Resolve attributes
  entry_offset += arguments.size();
  auto attributes =
      kernel.GetKernelEntries(entry_offset, kernel.num_attributes());
  for (int i = 0; i < attributes.size(); ++i) {
    size_t attribute_offset = attributes[i];
    size_t attribute_name_offset;
    // Assign a dummy name first and it will be overwritten if there is a valid
    // one.
    std::string attr_name = std::string("attr") + llvm::utostr(i);
    if (!attribute_names->ReadInt(&attribute_name_offset)) {
      if (auto n = bef_file_.GetString(attribute_name_offset)) {
        attr_name = std::string(*n);
      }
    }
    auto attr = bef_file_.GetAttribute(attribute_offset);
    if (!attr)
      // Use dummy values for unknown attributes.
      attr = mlir::IntegerAttr::get(mlir::IntegerType::get(32, &context_),
                                    0xdeadbeef);

    state.addAttribute(attr_name, attr);
  }

  // Resolve function references
  entry_offset += attributes.size();
  auto functions =
      kernel.GetKernelEntries(entry_offset, kernel.num_functions());
  for (auto fn_idx : functions) {
    const auto* bef_function = bef_file_.GetFunction(fn_idx);
    if (bef_function == nullptr) return emit_error("Unknown callee.");
    if (bef_function->IsNamedFunction()) {
      // If it is a named function, then it is a function reference.
      state.addAttribute(
          "callee", mlir::SymbolRefAttr::get(bef_function->name, &context_));
    } else {
      // Otherwise, it is a nested region. Add placeholder here and will be
      // resolved later.
      state.addRegion(nullptr);
    }
  }

  // Resolve results
  entry_offset += functions.size();
  auto results = kernel.GetKernelEntries(entry_offset, kernel.num_results());
  for (auto register_index : results) {
    state.types.push_back(GetRegister(register_index).type);
  }

  auto* op = mlir::Operation::create(state);

  // Add definitions.
  entry_offset += results.size();
  for (int i = 0; i < results.size(); ++i) {
    if (mlir::failed(AddDefinition(op->getResult(i), results[i]))) {
      op->destroy();
      return nullptr;
    }

    auto num_used_bys = kernel.num_used_bys(i);
    auto& reg_info = GetRegister(results[i]);
    reg_info.usedbys = kernel.GetKernelEntries(entry_offset, num_used_bys);
    entry_offset += num_used_bys;
  }

  // Nested regions will be resolved after all functions are processed.
  if (op->getNumRegions() > 0) {
    assert(op->getNumRegions() == functions.size());
    region_references_.insert({op, functions});
  }

  return op;
}

mlir::LogicalResult BEFFunctionReader::AddDefinition(mlir::Value value,
                                                     size_t register_index) {
  auto& reg_info = GetRegister(register_index);
  if (reg_info.value != nullptr) {
    EmitError(bef_file_.location, "Redefinition of registers");
    return mlir::failure();
  }
  assert(reg_info.type == value.getType() ||
         reg_info.type.isa<mlir::NoneType>());
  reg_info.value = value;
  return mlir::success();
}

BEFFunctionReader::RegisterInfo& BEFFunctionReader::GetRegister(
    int register_index) {
  assert(register_index < register_table_.size());
  return register_table_[register_index];
}

}  // namespace

mlir::OwningModuleRef ConvertBEFToMLIR(mlir::Location location,
                                       ArrayRef<uint8_t> bef_file,
                                       mlir::MLIRContext* context) {
  auto emit_error = [&location](string_view message) {
    EmitError(location, message);
    return nullptr;
  };

  BEFToMLIRConverter converter(bef_file, location, context);

  if (mlir::failed(converter.ReadHeader()))
    return emit_error("Invalid BEF file header.");

  BEFSections sections;
  // Read all sections without processing.
  if (mlir::failed(converter.ReadSections(&sections)))
    return emit_error("Invalid BEF section header.");

  // The first phase processes all sections and saves types, names and
  // attributes.
  if (mlir::failed(converter.ReadFormatVersion(
          sections.Get(BEFSectionID::kFormatVersion))))
    return emit_error("Invalid BEF version.");
  if (mlir::failed(converter.ReadLocationFilenames(
          sections.Get(BEFSectionID::kLocationFilenames))))
    return emit_error("Invalid LocationFilenames section.");
  if (mlir::failed(converter.ReadLocationPositions(
          sections.Get(BEFSectionID::kLocationPositions))))
    return emit_error("Invalid LocationPositions section.");
  if (mlir::failed(converter.ReadStrings(sections.Get(BEFSectionID::kStrings))))
    return emit_error("Invalid Strings section.");
  if (mlir::failed(converter.ReadTypes(sections.Get(BEFSectionID::kTypes))))
    return emit_error("Invalid Types section.");
  if (mlir::failed(converter.ReadAttributes(
          sections.Get(BEFSectionID::kAttributes),
          sections.Get(BEFSectionID::kAttributeTypes))))
    EmitWarning(location, "Invalid Attributes/AttributeTypes section.");
  if (mlir::failed(converter.ReadKernels(sections.Get(BEFSectionID::kKernels))))
    return emit_error("Invalid Kernels section.");
  if (mlir::failed(converter.ReadFunctionIndex(
          sections.Get(BEFSectionID::kFunctionIndex))))
    return emit_error("Invalid FunctionIndex section.");

  // The second phase processes all functions and creates corresponding region
  // bodies. Nested regions in MLIR operations are not resolved yet.
  BEFFunctionContext function_context;
  if (mlir::failed(converter.ReadFunctions(
          sections.Get(BEFSectionID::kFunctions),
          sections.Get(BEFSectionID::kAttributeNames),
          sections.Get(BEFSectionID::kRegisterTypes), &function_context)))
    return emit_error("Invalid Functions section.");

  // The third phase resolves all functions as either top level MLIR functions
  // or nested regions for MLIR operations.
  mlir::OwningModuleRef module(mlir::ModuleOp::create(location));
  if (mlir::failed(converter.ResolveFunctions(&function_context, module.get())))
    return emit_error("Failed to resolve functions.");

  return module;
}

}  // namespace tfrt