// Copyright 2017 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_WASM_WASM_CODE_MANAGER_H_
#define V8_WASM_WASM_CODE_MANAGER_H_

#include <functional>
#include <list>
#include <map>
#include <unordered_map>

#include "src/base/macros.h"
#include "src/handles.h"
#include "src/trap-handler/trap-handler.h"
#include "src/vector.h"
#include "src/wasm/module-compiler.h"

namespace v8 {
namespace internal {

struct CodeDesc;
class Code;
class Histogram;

namespace wasm {

class NativeModule;
class WasmCodeManager;
struct WasmModule;

// Convenience macro listing all wasm runtime stubs. Note that the first few
// elements of the list coincide with {compiler::TrapId}, order matters.
#define WASM_RUNTIME_STUB_LIST(V, VTRAP) \
  FOREACH_WASM_TRAPREASON(VTRAP)         \
  V(WasmAllocateHeapNumber)              \
  V(WasmArgumentsAdaptor)                \
  V(WasmCallJavaScript)                  \
  V(WasmStackGuard)                      \
  V(WasmToNumber)                        \
  V(DoubleToI)

struct AddressRange {
  Address start;
  Address end;

  AddressRange(Address s, Address e) : start(s), end(e) {
    DCHECK_LE(start, end);
    DCHECK_IMPLIES(start == kNullAddress, end == kNullAddress);
  }
  AddressRange() : AddressRange(kNullAddress, kNullAddress) {}

  size_t size() const { return static_cast<size_t>(end - start); }
  bool is_empty() const { return start == end; }
  operator bool() const { return start == kNullAddress; }
};

// Sorted, disjoint and non-overlapping memory ranges. A range is of the
// form [start, end). So there's no [start, end), [end, other_end),
// because that should have been reduced to [start, other_end).
class V8_EXPORT_PRIVATE DisjointAllocationPool final {
 public:
  DisjointAllocationPool() = default;

  explicit DisjointAllocationPool(AddressRange range) : ranges_({range}) {}

  DisjointAllocationPool(DisjointAllocationPool&& other) = default;
  DisjointAllocationPool& operator=(DisjointAllocationPool&& other) = default;

  // Merge the parameter range into this object while preserving ordering of the
  // ranges. The assumption is that the passed parameter is not intersecting
  // this object - for example, it was obtained from a previous Allocate.
  void Merge(AddressRange);

  // Allocate a contiguous range of size {size}. Return an empty pool on
  // failure.
  AddressRange Allocate(size_t size);

  bool IsEmpty() const { return ranges_.empty(); }
  const std::list<AddressRange>& ranges() const { return ranges_; }

 private:
  std::list<AddressRange> ranges_;

  DISALLOW_COPY_AND_ASSIGN(DisjointAllocationPool)
};

using ProtectedInstructions =
    std::vector<trap_handler::ProtectedInstructionData>;

class V8_EXPORT_PRIVATE WasmCode final {
 public:
  enum Kind {
    kFunction,
    kWasmToJsWrapper,
    kLazyStub,
    kRuntimeStub,
    kInterpreterEntry,
    kJumpTable
  };

  // Each runtime stub is identified by an id. This id is used to reference the
  // stub via {RelocInfo::WASM_STUB_CALL} and gets resolved during relocation.
  enum RuntimeStubId {
#define DEF_ENUM(Name) k##Name,
#define DEF_ENUM_TRAP(Name) kThrowWasm##Name,
    WASM_RUNTIME_STUB_LIST(DEF_ENUM, DEF_ENUM_TRAP)
#undef DEF_ENUM_TRAP
#undef DEF_ENUM
        kRuntimeStubCount
  };

  // kOther is used if we have WasmCode that is neither
  // liftoff- nor turbofan-compiled, i.e. if Kind is
  // not a kFunction.
  enum Tier : int8_t { kLiftoff, kTurbofan, kOther };

  Vector<byte> instructions() const { return instructions_; }
  Address instruction_start() const {
    return reinterpret_cast<Address>(instructions_.start());
  }
  Vector<const byte> reloc_info() const {
    return {reloc_info_.get(), reloc_size_};
  }
  Vector<const byte> source_positions() const {
    return {source_position_table_.get(), source_position_size_};
  }

  uint32_t index() const { return index_.ToChecked(); }
  // Anonymous functions are functions that don't carry an index.
  bool IsAnonymous() const { return index_.IsNothing(); }
  Kind kind() const { return kind_; }
  NativeModule* native_module() const { return native_module_; }
  Tier tier() const { return tier_; }
  Address constant_pool() const;
  size_t constant_pool_offset() const { return constant_pool_offset_; }
  size_t safepoint_table_offset() const { return safepoint_table_offset_; }
  size_t handler_table_offset() const { return handler_table_offset_; }
  uint32_t stack_slots() const { return stack_slots_; }
  bool is_liftoff() const { return tier_ == kLiftoff; }
  bool contains(Address pc) const {
    return reinterpret_cast<Address>(instructions_.start()) <= pc &&
           pc < reinterpret_cast<Address>(instructions_.end());
  }

  const ProtectedInstructions& protected_instructions() const {
    // TODO(mstarzinger): Code that doesn't have trapping instruction should
    // not be required to have this vector, make it possible to be null.
    DCHECK_NOT_NULL(protected_instructions_);
    return *protected_instructions_.get();
  }

  void Validate() const;
  void Print(Isolate* isolate) const;
  void Disassemble(const char* name, Isolate* isolate, std::ostream& os,
                   Address current_pc = kNullAddress) const;

  static bool ShouldBeLogged(Isolate* isolate);
  void LogCode(Isolate* isolate) const;

  ~WasmCode();

  enum FlushICache : bool { kFlushICache = true, kNoFlushICache = false };

  // Offset of {instructions_.start()}. It is used for tiering, when
  // we check if optimized code is available during the prologue
  // of Liftoff-compiled code.
  static constexpr int kInstructionStartOffset = 0;

 private:
  friend class NativeModule;

  WasmCode(Vector<byte> instructions, std::unique_ptr<const byte[]> reloc_info,
           size_t reloc_size, std::unique_ptr<const byte[]> source_pos,
           size_t source_pos_size, NativeModule* native_module,
           Maybe<uint32_t> index, Kind kind, size_t constant_pool_offset,
           uint32_t stack_slots, size_t safepoint_table_offset,
           size_t handler_table_offset,
           std::unique_ptr<ProtectedInstructions> protected_instructions,
           Tier tier)
      : instructions_(instructions),
        reloc_info_(std::move(reloc_info)),
        reloc_size_(reloc_size),
        source_position_table_(std::move(source_pos)),
        source_position_size_(source_pos_size),
        native_module_(native_module),
        index_(index),
        kind_(kind),
        constant_pool_offset_(constant_pool_offset),
        stack_slots_(stack_slots),
        safepoint_table_offset_(safepoint_table_offset),
        handler_table_offset_(handler_table_offset),
        protected_instructions_(std::move(protected_instructions)),
        tier_(tier) {
    DCHECK_LE(safepoint_table_offset, instructions.size());
    DCHECK_LE(constant_pool_offset, instructions.size());
    DCHECK_LE(handler_table_offset, instructions.size());
    DCHECK_EQ(kInstructionStartOffset, OFFSET_OF(WasmCode, instructions_));
  }

  // Code objects that have been registered with the global trap handler within
  // this process, will have a {trap_handler_index} associated with them.
  size_t trap_handler_index() const;
  void set_trap_handler_index(size_t);
  bool HasTrapHandlerIndex() const;

  // Register protected instruction information with the trap handler. Sets
  // trap_handler_index.
  void RegisterTrapHandlerData();

  Vector<byte> instructions_;
  std::unique_ptr<const byte[]> reloc_info_;
  size_t reloc_size_ = 0;
  std::unique_ptr<const byte[]> source_position_table_;
  size_t source_position_size_ = 0;
  NativeModule* native_module_ = nullptr;
  Maybe<uint32_t> index_;
  Kind kind_;
  size_t constant_pool_offset_ = 0;
  uint32_t stack_slots_ = 0;
  // we care about safepoint data for wasm-to-js functions,
  // since there may be stack/register tagged values for large number
  // conversions.
  size_t safepoint_table_offset_ = 0;
  size_t handler_table_offset_ = 0;
  intptr_t trap_handler_index_ = -1;
  std::unique_ptr<ProtectedInstructions> protected_instructions_;
  Tier tier_;

  DISALLOW_COPY_AND_ASSIGN(WasmCode);
};

// Return a textual description of the kind.
const char* GetWasmCodeKindAsString(WasmCode::Kind);

class V8_EXPORT_PRIVATE NativeModule final {
 public:
  WasmCode* AddCode(const CodeDesc& desc, uint32_t frame_count, uint32_t index,
                    size_t safepoint_table_offset, size_t handler_table_offset,
                    std::unique_ptr<ProtectedInstructions>,
                    Handle<ByteArray> source_position_table,
                    WasmCode::Tier tier);

  // A way to copy over JS-allocated code. This is because we compile
  // certain wrappers using a different pipeline.
  WasmCode* AddCodeCopy(Handle<Code> code, WasmCode::Kind kind, uint32_t index);

  // Add an interpreter entry. For the same reason as AddCodeCopy, we
  // currently compile these using a different pipeline and we can't get a
  // CodeDesc here. When adding interpreter wrappers, we do not insert them in
  // the code_table, however, we let them self-identify as the {index} function
  WasmCode* AddInterpreterEntry(Handle<Code> code, uint32_t index);

  // When starting lazy compilation, provide the WasmLazyCompile builtin by
  // calling SetLazyBuiltin. It will be copied into this NativeModule and the
  // jump table will be populated with that copy.
  void SetLazyBuiltin(Handle<Code> code);

  // Initializes all runtime stubs by copying them over from the JS-allocated
  // heap into this native module. It must be called exactly once per native
  // module before adding other WasmCode so that runtime stub ids can be
  // resolved during relocation.
  void SetRuntimeStubs(Isolate* isolate);

  WasmCode* code(uint32_t index) const {
    DCHECK_LT(index, num_functions_);
    DCHECK_LE(num_imported_functions_, index);
    return code_table_[index - num_imported_functions_];
  }

  bool has_code(uint32_t index) const {
    DCHECK_LT(index, num_functions_);
    DCHECK_LE(num_imported_functions_, index);
    return code_table_[index - num_imported_functions_] != nullptr;
  }

  WasmCode* runtime_stub(WasmCode::RuntimeStubId index) const {
    DCHECK_LT(index, WasmCode::kRuntimeStubCount);
    WasmCode* code = runtime_stub_table_[index];
    DCHECK_NOT_NULL(code);
    return code;
  }

  bool is_jump_table_slot(Address address) const {
    return jump_table_->contains(address);
  }

  uint32_t GetFunctionIndexFromJumpTableSlot(Address slot_address);

  // Transition this module from code relying on trap handlers (i.e. without
  // explicit memory bounds checks) to code that does not require trap handlers
  // (i.e. code with explicit bounds checks).
  // This method must only be called if {use_trap_handler()} is true (it will be
  // false afterwards). All code in this {NativeModule} needs to be re-added
  // after calling this method.
  void DisableTrapHandler();

  // Returns the target to call for the given function (returns a jump table
  // slot within {jump_table_}).
  Address GetCallTargetForFunction(uint32_t func_index) const;

  bool SetExecutable(bool executable);

  // For cctests, where we build both WasmModule and the runtime objects
  // on the fly, and bypass the instance builder pipeline.
  void ReserveCodeTableForTesting(uint32_t max_functions);
  void SetNumFunctionsForTesting(uint32_t num_functions);
  void SetCodeForTesting(uint32_t index, WasmCode* code);

  void LogWasmCodes(Isolate* isolate);

  CompilationState* compilation_state() { return compilation_state_.get(); }

  // TODO(mstarzinger): The link to the {WasmModuleObject} is deprecated and
  // all uses should vanish to make {NativeModule} independent of the Isolate.
  WasmModuleObject* module_object() const;
  void SetModuleObject(Handle<WasmModuleObject>);

  uint32_t num_functions() const { return num_functions_; }
  uint32_t num_imported_functions() const { return num_imported_functions_; }
  Vector<WasmCode*> code_table() const {
    return {code_table_.get(), num_functions_ - num_imported_functions_};
  }
  bool use_trap_handler() const { return use_trap_handler_; }
  void set_lazy_compile_frozen(bool frozen) { lazy_compile_frozen_ = frozen; }
  bool lazy_compile_frozen() const { return lazy_compile_frozen_; }

  WasmCode* Lookup(Address) const;

  const size_t instance_id = 0;
  ~NativeModule();

 private:
  friend class WasmCode;
  friend class WasmCodeManager;
  friend class NativeModuleSerializer;
  friend class NativeModuleDeserializer;
  friend class NativeModuleModificationScope;

  static base::AtomicNumber<size_t> next_id_;
  NativeModule(Isolate* isolate, uint32_t num_functions,
               uint32_t num_imported_functions, bool can_request_more,
               VirtualMemory* code_space, WasmCodeManager* code_manager,
               ModuleEnv& env);

  WasmCode* AddAnonymousCode(Handle<Code>, WasmCode::Kind kind);
  Address AllocateForCode(size_t size);

  // Primitive for adding code to the native module. All code added to a native
  // module is owned by that module. Various callers get to decide on how the
  // code is obtained (CodeDesc vs, as a point in time, Code*), the kind,
  // whether it has an index or is anonymous, etc.
  WasmCode* AddOwnedCode(Vector<const byte> orig_instructions,
                         std::unique_ptr<const byte[]> reloc_info,
                         size_t reloc_size,
                         std::unique_ptr<const byte[]> source_pos,
                         size_t source_pos_size, Maybe<uint32_t> index,
                         WasmCode::Kind kind, size_t constant_pool_offset,
                         uint32_t stack_slots, size_t safepoint_table_offset,
                         size_t handler_table_offset,
                         std::unique_ptr<ProtectedInstructions>, WasmCode::Tier,
                         WasmCode::FlushICache);

  WasmCode* CreateEmptyJumpTable(uint32_t num_wasm_functions);

  void PatchJumpTable(uint32_t func_index, Address target,
                      WasmCode::FlushICache);

  void set_code(uint32_t index, WasmCode* code) {
    DCHECK_LT(index, num_functions_);
    DCHECK_LE(num_imported_functions_, index);
    DCHECK_EQ(code->index(), index);
    code_table_[index - num_imported_functions_] = code;
  }

  // Holds all allocated code objects, is maintained to be in ascending order
  // according to the codes instruction start address to allow lookups.
  std::vector<std::unique_ptr<WasmCode>> owned_code_;

  uint32_t num_functions_;
  uint32_t num_imported_functions_;
  std::unique_ptr<WasmCode* []> code_table_;

  WasmCode* runtime_stub_table_[WasmCode::kRuntimeStubCount] = {nullptr};

  // Jump table used to easily redirect wasm function calls.
  WasmCode* jump_table_ = nullptr;

  std::unique_ptr<CompilationState, CompilationStateDeleter> compilation_state_;

  // A phantom reference to the {WasmModuleObject}. It is intentionally not
  // typed {Handle<WasmModuleObject>} because this location will be cleared
  // when the phantom reference is cleared.
  WasmModuleObject** module_object_ = nullptr;

  DisjointAllocationPool free_code_space_;
  DisjointAllocationPool allocated_code_space_;
  std::list<VirtualMemory> owned_code_space_;

  WasmCodeManager* wasm_code_manager_;
  base::Mutex allocation_mutex_;
  size_t committed_code_space_ = 0;
  int modification_scope_depth_ = 0;
  bool can_request_more_memory_;
  bool use_trap_handler_ = false;
  bool is_executable_ = false;
  bool lazy_compile_frozen_ = false;

  DISALLOW_COPY_AND_ASSIGN(NativeModule);
};

class V8_EXPORT_PRIVATE WasmCodeManager final {
 public:
  explicit WasmCodeManager(size_t max_committed);
  // Create a new NativeModule. The caller is responsible for its
  // lifetime. The native module will be given some memory for code,
  // which will be page size aligned. The size of the initial memory
  // is determined with a heuristic based on the total size of wasm
  // code. The native module may later request more memory.
  // TODO(titzer): isolate is only required here for CompilationState.
  std::unique_ptr<NativeModule> NewNativeModule(Isolate* isolate,
                                                ModuleEnv& env);
  // TODO(titzer): isolate is only required here for CompilationState.
  std::unique_ptr<NativeModule> NewNativeModule(
      Isolate* isolate, size_t memory_estimate, uint32_t num_functions,
      uint32_t num_imported_functions, bool can_request_more, ModuleEnv& env);

  NativeModule* LookupNativeModule(Address pc) const;
  WasmCode* LookupCode(Address pc) const;
  WasmCode* GetCodeFromStartAddress(Address pc) const;
  size_t remaining_uncommitted_code_space() const;

  void SetModuleCodeSizeHistogram(Histogram* histogram) {
    module_code_size_mb_ = histogram;
  }
  static size_t EstimateNativeModuleSize(const WasmModule* module);

 private:
  friend class NativeModule;

  void TryAllocate(size_t size, VirtualMemory*, void* hint = nullptr);
  bool Commit(Address, size_t);
  // Currently, we uncommit a whole module, so all we need is account
  // for the freed memory size. We do that in FreeNativeModule.
  // There's no separate Uncommit.

  void FreeNativeModule(NativeModule*);
  void Free(VirtualMemory* mem);
  void AssignRanges(Address start, Address end, NativeModule*);

  std::map<Address, std::pair<Address, NativeModule*>> lookup_map_;
  // Count of NativeModules not yet collected. Helps determine if it's
  // worth requesting a GC on memory pressure.
  size_t active_ = 0;
  std::atomic<size_t> remaining_uncommitted_code_space_;

  // Histogram to update with the maximum used code space for each NativeModule.
  Histogram* module_code_size_mb_ = nullptr;

  DISALLOW_COPY_AND_ASSIGN(WasmCodeManager);
};

// Within the scope, the native_module is writable and not executable.
// At the scope's destruction, the native_module is executable and not writable.
// The states inside the scope and at the scope termination are irrespective of
// native_module's state when entering the scope.
// We currently mark the entire module's memory W^X:
//  - for AOT, that's as efficient as it can be.
//  - for Lazy, we don't have a heuristic for functions that may need patching,
//    and even if we did, the resulting set of pages may be fragmented.
//    Currently, we try and keep the number of syscalls low.
// -  similar argument for debug time.
class NativeModuleModificationScope final {
 public:
  explicit NativeModuleModificationScope(NativeModule* native_module);
  ~NativeModuleModificationScope();

 private:
  NativeModule* native_module_;
};

}  // namespace wasm
}  // namespace internal
}  // namespace v8

#endif  // V8_WASM_WASM_CODE_MANAGER_H_
