/******************************************************************************
 *
 * \file
 * \brief Numerical execution engine
 *
 * Author: Maxime Arthaud
 *
 * Contributors: Jorge A. Navas
 *               Clement Decoodt
 *               Thomas Bailleux
 *
 * Contact: ikos@lists.nasa.gov
 *
 * Notices:
 *
 * Copyright (c) 2011-2018 United States Government as represented by the
 * Administrator of the National Aeronautics and Space Administration.
 * All Rights Reserved.
 *
 * Disclaimers:
 *
 * No Warranty: THE SUBJECT SOFTWARE IS PROVIDED "AS IS" WITHOUT ANY WARRANTY OF
 * ANY KIND, EITHER EXPRESSED, IMPLIED, OR STATUTORY, INCLUDING, BUT NOT LIMITED
 * TO, ANY WARRANTY THAT THE SUBJECT SOFTWARE WILL CONFORM TO SPECIFICATIONS,
 * ANY IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE,
 * OR FREEDOM FROM INFRINGEMENT, ANY WARRANTY THAT THE SUBJECT SOFTWARE WILL BE
 * ERROR FREE, OR ANY WARRANTY THAT DOCUMENTATION, IF PROVIDED, WILL CONFORM TO
 * THE SUBJECT SOFTWARE. THIS AGREEMENT DOES NOT, IN ANY MANNER, CONSTITUTE AN
 * ENDORSEMENT BY GOVERNMENT AGENCY OR ANY PRIOR RECIPIENT OF ANY RESULTS,
 * RESULTING DESIGNS, HARDWARE, SOFTWARE PRODUCTS OR ANY OTHER APPLICATIONS
 * RESULTING FROM USE OF THE SUBJECT SOFTWARE.  FURTHER, GOVERNMENT AGENCY
 * DISCLAIMS ALL WARRANTIES AND LIABILITIES REGARDING THIRD-PARTY SOFTWARE,
 * IF PRESENT IN THE ORIGINAL SOFTWARE, AND DISTRIBUTES IT "AS IS."
 *
 * Waiver and Indemnity:  RECIPIENT AGREES TO WAIVE ANY AND ALL CLAIMS AGAINST
 * THE UNITED STATES GOVERNMENT, ITS CONTRACTORS AND SUBCONTRACTORS, AS WELL
 * AS ANY PRIOR RECIPIENT.  IF RECIPIENT'S USE OF THE SUBJECT SOFTWARE RESULTS
 * IN ANY LIABILITIES, DEMANDS, DAMAGES, EXPENSES OR LOSSES ARISING FROM SUCH
 * USE, INCLUDING ANY DAMAGES FROM PRODUCTS BASED ON, OR RESULTING FROM,
 * RECIPIENT'S USE OF THE SUBJECT SOFTWARE, RECIPIENT SHALL INDEMNIFY AND HOLD
 * HARMLESS THE UNITED STATES GOVERNMENT, ITS CONTRACTORS AND SUBCONTRACTORS,
 * AS WELL AS ANY PRIOR RECIPIENT, TO THE EXTENT PERMITTED BY LAW.
 * RECIPIENT'S SOLE REMEDY FOR ANY SUCH MATTER SHALL BE THE IMMEDIATE,
 * UNILATERAL TERMINATION OF THIS AGREEMENT.
 *
 ******************************************************************************/

#pragma once

#include <ikos/ar/semantic/intrinsic.hpp>
#include <ikos/ar/verify/type.hpp>

#include <ikos/core/domain/exception/abstract_domain.hpp>
#include <ikos/core/domain/lifetime/abstract_domain.hpp>
#include <ikos/core/domain/machine_int/abstract_domain.hpp>
#include <ikos/core/domain/memory/abstract_domain.hpp>
#include <ikos/core/domain/nullity/abstract_domain.hpp>
#include <ikos/core/domain/pointer/abstract_domain.hpp>
#include <ikos/core/domain/uninitialized/abstract_domain.hpp>

#include <ikos/analyzer/analysis/context.hpp>
#include <ikos/analyzer/analysis/execution_engine/engine.hpp>
#include <ikos/analyzer/analysis/literal.hpp>
#include <ikos/analyzer/analysis/liveness.hpp>
#include <ikos/analyzer/analysis/option.hpp>
#include <ikos/analyzer/analysis/pointer/value.hpp>
#include <ikos/analyzer/support/assert.hpp>
#include <ikos/analyzer/support/cast.hpp>

namespace ikos {
namespace analyzer {

/// \brief Numerical execution engine
///
/// This class performs the transfer function on each AR (Abstract
/// Representation) statement with different levels of precision.
///
/// It relies on a abstract domain (template parameter AbstractDomain).
///
/// It can reason about registers (Precision::Register), pointers
/// (Precision::Pointer) and memory contents (Precision::Memory).
///
/// Levels of precision:
///
/// 1) If level of precision is Precision::Register then only integer scalar
/// variables are modelled using a numerical abstraction.
///
/// 2) If the level of precision is Precision::Pointer then both integer and
/// pointer scalar variables are modelled. If a variable is a pointer we model
/// its address, offset and size. The offset and size are modelled by a
/// numerical abstraction while the address is modelled by a symbolic
/// abstraction. This symbolic abstraction consists of a set of points-to
/// relationships that keeps track of all possible memory objects (i.e., &'s and
/// mallocs) to which the pointer may point to.
///
/// Thus, a pointer is abstracted by a triple <A,O,S> where A is the set of
/// addresses to which p may point to, O is the offset from the beginning of the
/// block expressed in bytes, and S is the size of the block. The value domain
/// keeps tracks of these triples.
///
/// 3) If the level of precision is Precision::Memory then same level than
/// Precision::Pointer plus memory contents. That is, we can keep track of which
/// values are stored in a triple <A,O,S>.
///
/// Abstractions:
///
/// - For an integer scalar x:
///
///   - A range that over-approximates the value of x. The representation of the
///     range depends on the underlying numerical domain.
///
///   - Whether x might be uninitialized or not.
///
/// - For a pointer scalar p (only if _precision >= Precision::Pointer):
///
///   - The offset from the base address of the object that contains p. For
///     this, we rely on the pointer_domain_impl that uses the variable
///     offset_var(p) = "p.offset" in the underlying numerical domain to
///     represent p's offset.
///
///   - The actual size of the allocated memory for p (including padding). For
///     this, we add a shadow variable get_alloc_size(obj) = "shadow.obj.size"
///     that keeps track of the allocated size by the memory object (&'s and
///     mallocs) associated with p in the underlying numerical domain.
///
///   - The address of p via a set of memory objects (&'s and mallocs) to which
///     p may point to (ie., points-to sets).
///
///   - Whether p might be null or not.
///
///   - Whether p might be uninitialized or not.
///
///   - Whether *p might be allocated or deallocated.
///
///   - In addition to this, if _precision == Precision::Memory, it also keeps
///     track of the content of p (i.e., *p). This is handled internally by the
///     value analysis (Load and Store).
template < typename AbstractDomain >
class NumericalExecutionEngine final : public ExecutionEngine {
private:
  using IntInterval = core::machine_int::Interval;
  using IntVariable = core::VariableExpression< MachineInt, Variable* >;
  using IntLinearExpression = core::LinearExpression< MachineInt, Variable* >;
  using IntLinearConstraint = core::LinearConstraint< MachineInt, Variable* >;
  using IntLinearConstraintSystem =
      core::LinearConstraintSystem< MachineInt, Variable* >;
  using Nullity = core::Nullity;
  using Uninitialized = core::Uninitialized;
  using Lifetime = core::Lifetime;
  using IntUnaryOperator = core::machine_int::UnaryOperator;
  using IntBinaryOperator = core::machine_int::BinaryOperator;
  using IntPredicate = core::machine_int::Predicate;
  using PtrPredicate = core::pointer::Predicate;

private:
  /// \brief Current invariant
  AbstractDomain _inv;

  /// \brief Analysis context
  Context& _ctx;

  /// \brief Memory location factory
  MemoryFactory& _mem_factory;

  /// \brief Variable name factory
  VariableFactory& _var_factory;

  /// \brief Literal factory
  LiteralFactory& _lit_factory;

  /// \brief Data layout
  const ar::DataLayout& _data_layout;

  /// \brief Call context
  CallContext* _call_context;

  /// \brief Precision level of the analysis (Register, Pointer or Memory)
  Precision _precision;

  /// \brief Optional liveness information
  const LivenessAnalysis* _liveness;

  /// \brief Optional pointer information
  const PointerInfo* _pointer_info;

public:
  /// \brief Constructor
  ///
  /// \param inv Initial invariant
  /// \param ctx Analysis context
  /// \param call_context Calling context
  /// \param precision Precision level
  /// \param liveness Liveness analysis, or null
  /// \param pointer_info Pointer information, or null
  NumericalExecutionEngine(AbstractDomain inv,
                           Context& ctx,
                           CallContext* call_context,
                           Precision precision,
                           const LivenessAnalysis* liveness = nullptr,
                           const PointerInfo* pointer_info = nullptr)
      : _inv(std::move(inv)),
        _ctx(ctx),
        _mem_factory(*ctx.mem_factory),
        _var_factory(*ctx.var_factory),
        _lit_factory(*ctx.lit_factory),
        _data_layout(ctx.bundle->data_layout()),
        _call_context(call_context),
        _precision(precision),
        _liveness(liveness),
        _pointer_info(pointer_info) {}

private:
  /// \brief Private copy constructor
  NumericalExecutionEngine(const NumericalExecutionEngine&) = default;

public:
  /// \brief Public move constructor
  NumericalExecutionEngine(NumericalExecutionEngine&&) = default;

  /// \brief Deleted copy assignment operator
  NumericalExecutionEngine& operator=(const NumericalExecutionEngine&) = delete;

  /// \brief Deleted move assignment operator
  NumericalExecutionEngine& operator=(NumericalExecutionEngine&&) = delete;

  /// \brief Destructor
  ~NumericalExecutionEngine() override = default;

public:
  /// \brief Create a fresh numerical execution engine, with its own abstract
  /// domain
  NumericalExecutionEngine fork() const { return *this; }

  /// \brief Return the current invariant
  AbstractDomain& inv() { return this->_inv; }

  /// \brief Return the current invariant
  const AbstractDomain& inv() const { return this->_inv; }

  /// \brief Update the current invariant
  void set_inv(const AbstractDomain& inv) { this->_inv = inv; }

  /// \brief Update the current invariant
  void set_inv(AbstractDomain&& inv) { this->_inv = std::move(inv); }

  /// \brief Return the liveness analysis used, or null
  const LivenessAnalysis* liveness() const { return this->_liveness; }

  /// \brief Return the pointer information, or null
  const PointerInfo* pointer_info() const { return this->_pointer_info; }

public:
  /// \name Helpers for memory statements
  /// @{

  /// \brief Assign a pointer variable `ptr`
  void assign_pointer(Variable* ptr,
                      MemoryLocation* addr,
                      Nullity null_val,
                      Uninitialized uninit_val) {
    if (this->_precision < Precision::Pointer) {
      return;
    }

    // Update pointer info, offset and nullity
    this->_inv.normal().pointers().assign_address(ptr, addr, null_val);

    // Update uninitialized variables
    this->_inv.normal().uninitialized().set(ptr, uninit_val);
  }

  /// \brief Initial value for a memory block
  enum class MemoryInitialValue {
    Zero,
    Uninitialized,
  };

  /// \brief Allocate a new memory object `addr` with unknown size
  ///
  /// We consider as a memory object an alloca (i.e., stack variables), global
  /// variables, malloc-like allocation sites, function pointers, and
  /// destination of inttoptr instructions. Also, variables whose address might
  /// have been taken are translated to global variables by the front-end.
  void allocate_memory(Variable* ptr,
                       MemoryLocation* addr,
                       Nullity null_val,
                       Uninitialized uninit_val,
                       Lifetime lifetime,
                       MemoryInitialValue init_val) {
    this->assign_pointer(ptr, addr, null_val, uninit_val);

    if (this->_precision < Precision::Memory) {
      return;
    }

    // Update memory location lifetime
    this->_inv.normal().lifetime().set(addr, lifetime);

    // Update memory value
    if (init_val == MemoryInitialValue::Zero) {
      this->_inv.normal().zero_reachable_mem(ptr);
    } else if (init_val == MemoryInitialValue::Uninitialized) {
      this->_inv.normal().uninitialize_reachable_mem(ptr);
    } else {
      ikos_unreachable("unreachable");
    }
  }

  /// \brief Allocate a new memory object `addr` of size `alloc_size` (in bytes)
  void allocate_memory(Variable* ptr,
                       MemoryLocation* addr,
                       Nullity null_val,
                       Uninitialized uninit_val,
                       Lifetime lifetime,
                       MemoryInitialValue init_val,
                       const MachineInt& alloc_size) {
    // Update pointer info, offset, nullity, uninitialized variables, lifetime
    // and initial value for the memory location
    this->allocate_memory(ptr, addr, null_val, uninit_val, lifetime, init_val);

    if (this->_precision < Precision::Memory) {
      return;
    }

    // Update allocated size var
    Variable* alloc_size_var = this->_var_factory.get_alloc_size(addr);
    this->_inv.normal().integers().assign(alloc_size_var, alloc_size);
  }

  /// \brief Allocate a new memory object `addr` of size `alloc_size` (in bytes)
  void allocate_memory(Variable* ptr,
                       MemoryLocation* addr,
                       Nullity null_val,
                       Uninitialized uninit_val,
                       Lifetime lifetime,
                       MemoryInitialValue init_val,
                       Variable* alloc_size) {
    // Update pointer info, offset, nullity, uninitialized variables, lifetime
    // and initial value for the memory location
    this->allocate_memory(ptr, addr, null_val, uninit_val, lifetime, init_val);

    if (this->_precision < Precision::Memory) {
      return;
    }

    // Update allocated size var
    Variable* alloc_size_var = this->_var_factory.get_alloc_size(addr);
    this->_inv.normal().integers().assign(alloc_size_var, alloc_size);
  }

private:
  /// \brief Initialize a global variable or function pointer operand
  ///
  /// Global variables and constant function pointers are not stored in the
  /// initial invariant, so it is necessary to initialize them on the fly when
  /// we need them.
  void init_global_operand(ar::Value* value) {
    if (this->_precision < Precision::Pointer) {
      return;
    }

    if (auto gv = dyn_cast< ar::GlobalVariable >(value)) {
      this->assign_pointer(this->_var_factory.get_global(gv),
                           this->_mem_factory.get_global(gv),
                           Nullity::non_null(),
                           Uninitialized::initialized());
    } else if (auto fun_ptr = dyn_cast< ar::FunctionPointerConstant >(value)) {
      auto fun = fun_ptr->function();
      this->assign_pointer(this->_var_factory.get_function_ptr(fun),
                           this->_mem_factory.get_function(fun),
                           Nullity::non_null(),
                           Uninitialized::initialized());
    } else if (auto struct_cst = dyn_cast< ar::StructConstant >(value)) {
      for (auto it = struct_cst->field_begin(), et = struct_cst->field_end();
           it != et;
           ++it) {
        this->init_global_operand(it->second);
      }
    } else if (auto seq_cst = dyn_cast< ar::SequentialConstant >(value)) {
      for (auto it = seq_cst->element_begin(), et = seq_cst->element_end();
           it != et;
           ++it) {
        this->init_global_operand(*it);
      }
    }
  }

private:
  /// \brief Prepare a memory access (read/write) on the given pointer
  ///
  /// Return true if the memory access can be performed, i.e the pointer is
  /// non-null, well defined, and `_precision == Precision::Memory`
  bool prepare_mem_access(const ScalarLit& ptr) {
    if (ptr.is_null() || ptr.is_undefined()) {
      // null/undefined dereference
      this->_inv.set_normal_flow_to_bottom();
      return false;
    }

    ikos_assert_msg(ptr.is_pointer_var(), "unexpected parameter");

    if (this->_precision < Precision::Pointer) {
      return false;
    }

    // reduction between value and pointer analysis
    this->refine_addresses_offset(ptr.var());

    if (this->_inv.is_normal_flow_bottom()) {
      return false;
    }

    if (this->_inv.normal().nullity().is_null(ptr.var()) ||
        this->_inv.normal().uninitialized().is_uninitialized(ptr.var())) {
      // null/undefined dereference
      this->_inv.set_normal_flow_to_bottom();
      return false;
    }

    return this->_precision == Precision::Memory; // ready for read/write
  }

private:
  /// \brief Model a pointer arithmetic `lhs = base + offset`
  void pointer_shift(const ScalarLit& lhs,
                     const ScalarLit& base,
                     const ScalarLit& offset) {
    if (this->_precision < Precision::Pointer) {
      return;
    }

    ikos_assert_msg(lhs.is_pointer_var(),
                    "left hand side is not a pointer variable");
    ikos_assert_msg(!base.is_undefined(), "base is not defined");
    ikos_assert_msg(!offset.is_undefined(), "offset is not defined");

    if (base.is_null()) {
      this->_inv.normal().pointers().assign_address(lhs.var(),
                                                    this->_mem_factory
                                                        .get_absolute_zero(),
                                                    Nullity::null());
      if (offset.is_machine_int_var()) {
        this->_inv.normal().pointers().assign(lhs.var(),
                                              lhs.var(),
                                              offset.var());
      } else if (offset.is_machine_int()) {
        this->_inv.normal().pointers().assign(lhs.var(),
                                              lhs.var(),
                                              offset.machine_int());
      } else {
        ikos_unreachable("unexpected offset operand");
      }
    } else if (base.is_pointer_var()) {
      if (offset.is_machine_int_var()) {
        this->_inv.normal().pointers().assign(lhs.var(),
                                              base.var(),
                                              offset.var());
      } else if (offset.is_machine_int()) {
        this->_inv.normal().pointers().assign(lhs.var(),
                                              base.var(),
                                              offset.machine_int());
      } else {
        ikos_unreachable("unexpected offset operand");
      }
    } else {
      ikos_unreachable("unexpected base operand");
    }

    this->_inv.normal().uninitialized().assign_initialized(lhs.var());
    this->normalize_absolute_zero_nullity(lhs.var());
  }

private:
  /// \brief Normalize the nullity domain
  void normalize_absolute_zero_nullity(Variable* var) {
    // Check if AbsoluteZeroMemoryLocation could be the base of the pointer
    // if so, we have to check if zero is contained in the offset interval of
    // the pointer.

    if (this->_inv.normal().nullity().is_bottom() ||
        this->_inv.normal().nullity().get(var).is_top()) {
      return;
    }

    auto pts = this->_inv.normal().pointers().points_to(var);

    if (pts.contains(this->_mem_factory.get_absolute_zero())) {
      auto offset_interval = this->_inv.normal().integers().to_interval(
          this->_inv.normal().pointers().offset_var(var));
      MachineInt zero =
          MachineInt::zero(offset_interval.bit_width(), offset_interval.sign());

      if (offset_interval.is_bottom()) {
        return;
      } else if (pts.singleton()) {
        if (offset_interval.singleton() ==
            boost::optional< MachineInt >(zero)) {
          // Pointer is definitely null (base is zero, offset = 0)
          this->_inv.normal().pointers().assign_null(var);
        } else if (!offset_interval.contains(zero)) {
          // Pointer is definitely non-null (base is zero, offset != 0)
          this->_inv.normal().nullity().assign_non_null(var);
        } else {
          // Pointer might be null (base is zero, offset contains zero)
          this->_inv.normal().nullity().forget(var);
        }
      } else if (offset_interval.contains(zero)) {
        // Pointer might be null (base might be zero, offset contains zero)
        this->_inv.normal().nullity().forget(var);
      }
    }
  }

private:
  /// \brief Refine the addresses of `ptr` using information from an external
  /// pointer analysis
  void refine_addresses(Variable* ptr) {
    if (this->_precision < Precision::Pointer) {
      return;
    }

    if (!this->_pointer_info) {
      return;
    }

    PointerAbsValue value = this->_pointer_info->get(ptr);
    this->_inv.normal().pointers().refine(ptr, value.points_to());
  }

private:
  /// \brief Refine the addresses and offset of `ptr` using information from an
  /// external pointer analysis
  void refine_addresses_offset(Variable* ptr) {
    if (this->_precision < Precision::Pointer) {
      return;
    }

    if (!this->_pointer_info) {
      return;
    }

    PointerAbsValue value = this->_pointer_info->get(ptr);
    this->_inv.normal().pointers().refine(ptr, value);
  }

private:
  /// @}
  /// \name Helpers for assignments
  /// @{

  /// \brief Integer variable assignment
  ///
  /// Support implicit bitcasts (see is_implicit_bitcast in ar::TypeVerifier)
  class IntegerAssign : public ScalarLit::template Visitor<> {
  private:
    Variable* _lhs;
    AbstractDomain& _inv;

  public:
    IntegerAssign(Variable* lhs, AbstractDomain& inv) : _lhs(lhs), _inv(inv) {}

    void machine_int(const MachineInt& rhs) {
      auto type = ar::cast< ar::IntegerType >(this->_lhs->type());

      // Update numerical abstraction
      ikos_assert(type->bit_width() == rhs.bit_width());
      if (type->sign() == rhs.sign()) {
        this->_inv.normal().integers().assign(this->_lhs, rhs);
      } else {
        this->_inv.normal().integers().assign(this->_lhs,
                                              rhs.sign_cast(type->sign()));
      }

      // Update uninitialized variables
      this->_inv.normal().uninitialized().assign_initialized(this->_lhs);
    }

    void floating_point(const DummyNumber&) { ikos_unreachable("unreachable"); }

    void memory_location(MemoryLocation*) { ikos_unreachable("unreachable"); }

    void null() { ikos_unreachable("unreachable"); }

    void undefined() {
      // Update numerical abstraction
      this->_inv.normal().integers().forget(this->_lhs);

      // Update uninitialized variables
      this->_inv.normal().uninitialized().assign_uninitialized(this->_lhs);
    }

    void machine_int_var(Variable* rhs) {
      auto lhs_type = ar::cast< ar::IntegerType >(this->_lhs->type());
      auto rhs_type = ar::cast< ar::IntegerType >(rhs->type());

      // Update numerical abstraction
      ikos_assert(lhs_type->bit_width() == rhs_type->bit_width());
      if (lhs_type->sign() == rhs_type->sign()) {
        this->_inv.normal().integers().assign(this->_lhs, rhs);
      } else {
        this->_inv.normal().integers().apply(IntUnaryOperator::SignCast,
                                             this->_lhs,
                                             rhs);
      }

      // Update uninitialized variables
      this->_inv.normal().uninitialized().assign(this->_lhs, rhs);
    }

    void floating_point_var(Variable*) { ikos_unreachable("unreachable"); }

    void pointer_var(Variable*) { ikos_unreachable("unreachable"); }

  }; // end class IntegerAssign

  /// \brief Floating point variable assignment
  class FloatingPointAssign : public ScalarLit::template Visitor<> {
  private:
    Variable* _lhs;
    AbstractDomain& _inv;

  public:
    FloatingPointAssign(Variable* lhs, AbstractDomain& inv)
        : _lhs(lhs), _inv(inv) {}

    void machine_int(const MachineInt&) { ikos_unreachable("unreachable"); }

    void floating_point(const DummyNumber&) {
      // TODO(marthaud): Update numerical abstraction

      // Update uninitialized variables
      this->_inv.normal().uninitialized().assign_initialized(this->_lhs);
    }

    void memory_location(MemoryLocation*) { ikos_unreachable("unreachable"); }

    void null() { ikos_unreachable("unreachable"); }

    void undefined() {
      // TODO(marthaud): Update numerical abstraction

      // Update uninitialized variables
      this->_inv.normal().uninitialized().assign_uninitialized(this->_lhs);
    }

    void machine_int_var(Variable*) { ikos_unreachable("unreachable"); }

    void floating_point_var(Variable* rhs) {
      // TODO(marthaud): Update numerical abstraction

      // Update uninitialized variables
      this->_inv.normal().uninitialized().assign(this->_lhs, rhs);
    }

    void pointer_var(Variable*) { ikos_unreachable("unreachable"); }

  }; // end class FloatingPointAssign

  /// \brief Pointer variable assignment
  class PointerAssign : public ScalarLit::template Visitor<> {
  private:
    Variable* _lhs;
    AbstractDomain& _inv;

  public:
    PointerAssign(Variable* lhs, AbstractDomain& inv) : _lhs(lhs), _inv(inv) {}

    void machine_int(const MachineInt&) { ikos_unreachable("unreachable"); }

    void floating_point(const DummyNumber&) { ikos_unreachable("unreachable"); }

    void memory_location(MemoryLocation* addr) {
      this->_inv.normal().pointers().assign_address(this->_lhs,
                                                    addr,
                                                    Nullity::non_null());
      this->_inv.normal().uninitialized().assign_initialized(this->_lhs);
    }

    void null() {
      this->_inv.normal().pointers().assign_null(this->_lhs);
      this->_inv.normal().uninitialized().assign_initialized(this->_lhs);
    }

    void undefined() {
      this->_inv.normal().pointers().assign_undef(this->_lhs);
      this->_inv.normal().uninitialized().assign_uninitialized(this->_lhs);
    }

    void machine_int_var(Variable*) { ikos_unreachable("unreachable"); }

    void floating_point_var(Variable*) { ikos_unreachable("unreachable"); }

    void pointer_var(Variable* rhs) {
      this->_inv.normal().pointers().assign(this->_lhs, rhs);
      this->_inv.normal().uninitialized().assign(this->_lhs, rhs);
    }

  }; // end class PointerAssign

public:
  /// \brief Scalar assignment `lhs = rhs`
  void assign(const ScalarLit& lhs, const ScalarLit& rhs) {
    if (lhs.is_machine_int_var()) {
      IntegerAssign v(lhs.var(), this->_inv);
      rhs.apply_visitor(v);
    } else if (lhs.is_floating_point_var()) {
      FloatingPointAssign v(lhs.var(), this->_inv);
      rhs.apply_visitor(v);
    } else if (lhs.is_pointer_var()) {
      if (this->_precision < Precision::Pointer) {
        return; // ignore pointers
      }

      PointerAssign v(lhs.var(), this->_inv);
      rhs.apply_visitor(v);
    } else {
      ikos_unreachable("left hand side is not a variable");
    }
  }

private:
  /// @}
  /// \name Helpers for aggregate (struct,array) statements
  /// @{

  /// \brief Return the AR type void*
  ar::Type* void_ptr_type() const {
    ar::Context& ctx = _ctx.bundle->context();
    return ar::PointerType::get(ctx, ar::VoidType::get(ctx));
  }

  /// \brief Initialize an aggregate memory block
  ///
  /// Internal variables of aggregate types are modeled as if they were in
  /// memory, at a symbolic location.
  void init_aggregate_memory(const AggregateLit& aggregate) {
    ikos_assert_msg(aggregate.is_var(), "aggregate is not a variable");

    auto var = cast< InternalVariable >(aggregate.var());
    this->allocate_memory(var,
                          this->_mem_factory.get_aggregate(var->internal_var()),
                          Nullity::non_null(),
                          Uninitialized::initialized(),
                          Lifetime::top(),
                          MemoryInitialValue::Uninitialized);
  }

  /// \brief Return a pointer to the symbolic location of the aggregate in
  /// memory
  ScalarLit aggregate_pointer(const AggregateLit& aggregate) {
    ikos_assert_msg(aggregate.is_var(), "aggregate is not a variable");

    auto var = cast< InternalVariable >(aggregate.var());
    this->assign_pointer(var,
                         this->_mem_factory.get_aggregate(var->internal_var()),
                         Nullity::non_null(),
                         Uninitialized::initialized());
    return ScalarLit::pointer_var(var);
  }

  /// \brief Write an aggregate in the memory
  void mem_write_aggregate(const ScalarLit& ptr,
                           const AggregateLit& aggregate) {
    ikos_assert_msg(ptr.is_pointer_var(), "unexpected pointer");

    if (this->_precision < Precision::Memory) {
      return;
    }

    if (aggregate.size().is_zero()) {
      return; // nothing to do
    } else if (aggregate.is_cst()) {
      // Pointer to write the aggregate in the memory
      ScalarLit write_ptr = ScalarLit::pointer_var(
          this->_var_factory
              .get_named_shadow(this->void_ptr_type(),
                                "shadow.mem_write_aggregate.ptr"));

      for (const auto& field : aggregate.fields()) {
        this->pointer_shift(write_ptr,
                            ptr,
                            ScalarLit::machine_int(field.offset));
        this->_inv.normal().mem_write(this->_var_factory,
                                      write_ptr.var(),
                                      field.value,
                                      field.size);
      }

      // clean-up
      this->_inv.normal().forget_surface(write_ptr.var());
    } else if (aggregate.is_zero() || aggregate.is_undefined()) {
      // aggregate.size() is in bytes, compute bit-width, and check
      // if the bit-width fits in an unsigned int
      bool overflow;
      MachineInt eight(8, aggregate.size().bit_width(), Unsigned);
      MachineInt bit_width = mul(aggregate.size(), eight, overflow);
      if (overflow || !bit_width.fits< unsigned >()) {
        // too big for a cell
        this->_inv.normal().forget_reachable_mem(ptr.var());
      } else if (aggregate.is_zero()) {
        MachineInt zero(0, bit_width.to< unsigned >(), Signed);
        this->_inv.normal().mem_write(this->_var_factory,
                                      ptr.var(),
                                      ScalarLit::machine_int(zero),
                                      aggregate.size());
      } else if (aggregate.is_undefined()) {
        this->_inv.normal().mem_write(this->_var_factory,
                                      ptr.var(),
                                      ScalarLit::undefined(),
                                      aggregate.size());
      } else {
        ikos_unreachable("unreachable");
      }
    } else if (aggregate.is_var()) {
      ScalarLit aggregate_ptr = this->aggregate_pointer(aggregate);
      this->_inv.normal().mem_copy(this->_var_factory,
                                   ptr.var(),
                                   aggregate_ptr.var(),
                                   ScalarLit::machine_int(aggregate.size()));
    } else {
      ikos_unreachable("unreachable");
    }
  }

public:
  /// \brief Aggregate assignment `lhs = rhs`
  void assign(const AggregateLit& lhs, const AggregateLit& rhs) {
    ikos_assert_msg(lhs.is_var(), "left hand side is not a variable");

    if (this->_precision < Precision::Memory) {
      return;
    }

    this->init_aggregate_memory(lhs);
    this->mem_write_aggregate(this->aggregate_pointer(lhs), rhs);
  }

  /// \brief Assignment `lhs = rhs`
  void assign(const Literal& lhs, const Literal& rhs) {
    if (lhs.is_scalar()) {
      ikos_assert_msg(rhs.is_scalar(), "unexpected right hand side");
      this->assign(lhs.scalar(), rhs.scalar());
    } else if (lhs.is_aggregate()) {
      ikos_assert_msg(rhs.is_aggregate(), "unexpected right hand side");
      this->assign(lhs.aggregate(), rhs.aggregate());
    } else {
      ikos_unreachable("unreachable");
    }
  }

private:
  /// \brief Randomly throw unknown exceptions with the current invariant
  ///
  /// Equivalent to if (rand()) { throw rand(); }
  void throw_unknown_exceptions() {
    this->_inv.caught_exceptions().join_with(this->_inv.normal());
  }

public:
  /// \brief Deallocate the memory for the given local variables
  void deallocate_local_variables(ar::Function::LocalVariableIterator begin,
                                  ar::Function::LocalVariableIterator end) {
    if (this->_precision < Precision::Pointer) {
      return;
    }

    for (auto it = begin; it != end; ++it) {
      LocalVariable* var = this->_var_factory.get_local(*it);
      MemoryLocation* addr = this->_mem_factory.get_local(*it);

      // Forget the allocated size
      AllocSizeVariable* alloc_size_var =
          this->_var_factory.get_alloc_size(addr);
      this->_inv.normal().integers().forget(alloc_size_var);
      this->_inv.caught_exceptions().integers().forget(alloc_size_var);
      this->_inv.propagated_exceptions().integers().forget(alloc_size_var);

      // Set the memory location lifetime to deallocated
      this->_inv.normal().lifetime().assign_deallocated(addr);
      this->_inv.caught_exceptions().lifetime().assign_deallocated(addr);
      this->_inv.propagated_exceptions().lifetime().assign_deallocated(addr);

      if (this->_precision >= Precision::Memory) {
        // Forget the memory content
        this->_inv.normal().forget_mem(addr);
        this->_inv.caught_exceptions().forget_mem(addr);
        this->_inv.propagated_exceptions().forget_mem(addr);
      }

      // Forget local variable pointer
      this->_inv.normal().forget_surface(var);
      this->_inv.caught_exceptions().forget_surface(var);
      this->_inv.propagated_exceptions().forget_surface(var);
    }
  }

public:
  /// @}
  /// \name Implement ExecutionEngine
  /// @{

  /// \brief Enter a basic block
  void exec_enter(ar::BasicBlock*) override {}

  /// \brief Leave a basic block
  ///
  /// Use the liveness analysis to remove dead variables
  void exec_leave(ar::BasicBlock* bb) override {
    if (this->_liveness == nullptr) {
      return;
    }

    boost::optional< const LivenessAnalysis::VariableRefList& > dead =
        this->_liveness->dead_at_end(bb);

    if (!dead) {
      return;
    }

    // Do not remove the returned variable
    Variable* returned_var = nullptr;
    if (!bb->empty() && isa< ar::ReturnValue >(bb->back())) {
      auto ret = cast< ar::ReturnValue >(bb->back());

      if (ret->has_operand()) {
        const Literal& v = this->_lit_factory.get(ret->operand());
        if (v.is_scalar() && v.scalar().is_var()) {
          returned_var = v.scalar().var();
        } else if (v.is_aggregate() && v.aggregate().is_var()) {
          returned_var = v.aggregate().var();
        }
      }
    }

    for (Variable* var : *dead) {
      if (var == returned_var) { // Ignore
        continue;
      }

      // Special case for aggregate internal variables: Clean-up the memory
      if (this->_precision >= Precision::Memory) {
        if (auto iv = dyn_cast< InternalVariable >(var)) {
          ar::InternalVariable* ar_iv = iv->internal_var();
          if (ar_iv->type()->is_aggregate()) {
            MemoryLocation* addr = this->_mem_factory.get_aggregate(ar_iv);
            this->_inv.normal().forget_mem(addr);
            this->_inv.caught_exceptions().forget_mem(addr);
            this->_inv.propagated_exceptions().forget_mem(addr);
          }
        }
      }

      // Clean-up memory surface
      this->_inv.normal().forget_surface(var);
      this->_inv.caught_exceptions().forget_surface(var);
      this->_inv.propagated_exceptions().forget_surface(var);
    }
  }

  /// \brief Execute an edge from `src` to `dest`
  void exec_edge(ar::BasicBlock* src, ar::BasicBlock* dest) override {
    // Check if the source block ends with an invoke

    if (src->empty()) {
      return;
    }

    ar::Statement* stmt = src->back();
    if (!isa< ar::Invoke >(stmt)) {
      return;
    }

    auto invoke = cast< ar::Invoke >(stmt);
    if (invoke->normal_dest() == dest) {
      this->_inv.enter_normal();
    } else if (invoke->exception_dest() == dest) {
      this->_inv.enter_catch();
    } else {
      ikos_unreachable("unreachable");
    }
  }

  /// \brief Execute an Assignment statement
  void exec(ar::Assignment* s) override {
    // initialize lazily global objects
    this->init_global_operand(s->operand());

    this->assign(this->_lit_factory.get(s->result()),
                 this->_lit_factory.get(s->operand()));
  }

  /// \brief Execute an UnaryOperation statement
  void exec(ar::UnaryOperation* s) override {
    const ScalarLit& lhs = this->_lit_factory.get_scalar(s->result());
    const ScalarLit& rhs = this->_lit_factory.get_scalar(s->operand());
    ikos_assert_msg(lhs.is_var(), "left hand side is not a variable");

    if (rhs.is_undefined()) {
      this->_inv.set_normal_flow_to_bottom();
      return;
    }

    switch (s->op()) {
      case ar::UnaryOperation::UTrunc:
      case ar::UnaryOperation::STrunc: {
        this->exec_int_conv(IntUnaryOperator::Trunc, lhs, rhs);
      } break;
      case ar::UnaryOperation::ZExt: {
        this->exec_int_conv(IntUnaryOperator::Ext, lhs, rhs);
      } break;
      case ar::UnaryOperation::SExt: {
        this->exec_int_conv(IntUnaryOperator::Ext, lhs, rhs);
      } break;
      case ar::UnaryOperation::FPTrunc:
      case ar::UnaryOperation::FPExt: {
        this->exec_float_conv(lhs, rhs);
      } break;
      case ar::UnaryOperation::FPToUI:
      case ar::UnaryOperation::FPToSI: {
        this->exec_float_to_int_conv(lhs, rhs);
      } break;
      case ar::UnaryOperation::UIToFP:
      case ar::UnaryOperation::SIToFP: {
        this->exec_int_to_float_conv(lhs, rhs);
      } break;
      case ar::UnaryOperation::PtrToUI:
      case ar::UnaryOperation::PtrToSI: {
        this->exec_ptr_to_int_conv(s, lhs, rhs);
      } break;
      case ar::UnaryOperation::UIToPtr:
      case ar::UnaryOperation::SIToPtr: {
        this->exec_int_to_ptr_conv(lhs, rhs);
      } break;
      case ar::UnaryOperation::Bitcast: {
        this->exec_bitcast(s, lhs, rhs);
      } break;
    }

    this->_inv.normal().uninitialized().assign_initialized(lhs.var());
  }

private:
  /// \brief Execute an integer conversion
  void exec_int_conv(IntUnaryOperator op,
                     const ScalarLit& lhs,
                     const ScalarLit& rhs) {
    ikos_assert_msg(lhs.is_machine_int_var(),
                    "left hand side is not an integer variable");

    if (rhs.is_machine_int()) {
      auto type = cast< ar::IntegerType >(lhs.var()->type());
      this->_inv.normal()
          .integers()
          .assign(lhs.var(),
                  core::machine_int::apply_unary_operator(op,
                                                          rhs.machine_int(),
                                                          type->bit_width(),
                                                          type->sign()));
    } else if (rhs.is_machine_int_var()) {
      this->_inv.normal().integers().apply(op, lhs.var(), rhs.var());
    } else {
      ikos_unreachable("unexpected arguments");
    }
  }

  /// \brief Execute a floating point conversion
  void exec_float_conv(const ScalarLit& lhs, const ScalarLit& /*rhs*/) {
    ikos_assert_msg(lhs.is_floating_point_var(),
                    "left hand side is not a floating point variable");
    ikos_ignore(lhs);
  }

  /// \brief Execute a conversion from floating point to integer
  void exec_float_to_int_conv(const ScalarLit& lhs, const ScalarLit& /*rhs*/) {
    ikos_assert_msg(lhs.is_machine_int_var(),
                    "left hand side is not an integer variable");

    this->_inv.normal().integers().forget(lhs.var());
  }

  /// \brief Execute a conversion from integer to floating point
  void exec_int_to_float_conv(const ScalarLit& lhs, const ScalarLit& /*rhs*/) {
    ikos_assert_msg(lhs.is_floating_point_var(),
                    "left hand side is not a floating point variable");
    ikos_ignore(lhs);
  }

  /// \brief Execute a conversion from pointer to integer
  void exec_ptr_to_int_conv(ar::UnaryOperation* s,
                            const ScalarLit& lhs,
                            const ScalarLit& rhs) {
    ikos_assert_msg(lhs.is_machine_int_var(),
                    "left hand side is not an integer variable");

    if (rhs.is_null()) {
      auto type = cast< ar::IntegerType >(lhs.var()->type());
      MachineInt zero(0, type->bit_width(), type->sign());
      this->_inv.normal().integers().assign(lhs.var(), zero);
    } else if (rhs.is_pointer_var()) {
      if (this->_inv.is_normal_flow_bottom()) {
        return;
      }

      this->init_global_operand(s->operand());
      PointsToSet addrs = this->_inv.normal().pointers().points_to(rhs.var());

      if (this->_inv.normal().nullity().is_null(rhs.var())) {
        auto type = cast< ar::IntegerType >(lhs.var()->type());
        MachineInt zero(0, type->bit_width(), type->sign());
        this->_inv.normal().integers().assign(lhs.var(), zero);
      } else if (addrs == PointsToSet{this->_mem_factory.get_absolute_zero()}) {
        // This could be an hardware address
        auto offset_var = this->_inv.normal().pointers().offset_var(rhs.var());
        this->_inv.normal().integers().apply(IntUnaryOperator::Cast,
                                             lhs.var(),
                                             offset_var);
      } else {
        this->_inv.normal().integers().forget(lhs.var());
      }
    } else {
      ikos_unreachable("unreachable");
    }
  }

  /// \brief Execute a conversion from integer to pointer
  ///
  /// For instance: int x = 5; int *px = x;
  void exec_int_to_ptr_conv(const ScalarLit& lhs, const ScalarLit& rhs) {
    if (this->_precision < Precision::Pointer) {
      return;
    }

    if (rhs.is_machine_int()) {
      MachineInt addr = rhs.machine_int();

      if (addr.is_zero()) {
        this->_inv.normal().pointers().assign_null(lhs.var());
      } else {
        addr = addr.cast(this->_data_layout.pointers.bit_width, Unsigned);
        this->_inv.normal().pointers().assign_address(lhs.var(),
                                                      this->_mem_factory
                                                          .get_absolute_zero(),
                                                      Nullity::non_null());

        auto offset_var = this->_inv.normal().pointers().offset_var(lhs.var());
        this->_inv.normal().integers().assign(offset_var, addr);
      }
    } else if (rhs.is_machine_int_var()) {
      this->_inv.normal().pointers().assign_address(lhs.var(),
                                                    this->_mem_factory
                                                        .get_absolute_zero(),
                                                    Nullity::null());

      auto offset_var = this->_inv.normal().pointers().offset_var(lhs.var());
      this->_inv.normal().integers().apply(IntUnaryOperator::Cast,
                                           offset_var,
                                           rhs.var());
      this->normalize_absolute_zero_nullity(lhs.var());
    } else {
      ikos_unreachable("unexpected operand");
    }
  }

  /// \brief Execute a bitcast
  void exec_bitcast(ar::UnaryOperation* s,
                    const ScalarLit& lhs,
                    const ScalarLit& rhs) {
    if (lhs.is_pointer_var()) {
      // pointer cast: A* to B*
      if (this->_precision < Precision::Pointer) {
        return;
      }

      this->init_global_operand(s->operand());
      this->assign(lhs, rhs);
    } else if (lhs.is_machine_int_var()) {
      // sign cast: (u|s)iN to (u|s)iN
      // float to integer: (float|double|..) to (u|s)iN

      if (rhs.is_machine_int()) {
        auto type = ar::cast< ar::IntegerType >(lhs.var()->type());
        this->_inv.normal()
            .integers()
            .assign(lhs.var(),
                    rhs.machine_int().cast(type->bit_width(), type->sign()));
      } else if (rhs.is_floating_point()) {
        this->_inv.normal().integers().forget(lhs.var());
      } else if (rhs.is_machine_int_var()) {
        this->_inv.normal().integers().apply(IntUnaryOperator::SignCast,
                                             lhs.var(),
                                             rhs.var());
      } else if (rhs.is_floating_point_var()) {
        this->_inv.normal().integers().forget(lhs.var());
      } else {
        ikos_unreachable("unexpected literal");
      }
    } else {
      ikos_unreachable("left hand side is not a variable");
    }
  }

public:
  /// \brief Execute a BinaryOperation statement
  void exec(ar::BinaryOperation* s) override {
    const ScalarLit& lhs = this->_lit_factory.get_scalar(s->result());
    const ScalarLit& left = this->_lit_factory.get_scalar(s->left());
    const ScalarLit& right = this->_lit_factory.get_scalar(s->right());

    if (left.is_undefined() || right.is_undefined()) {
      this->_inv.set_normal_flow_to_bottom();
      return;
    }

    switch (s->op()) {
      case ar::BinaryOperation::UAdd:
      case ar::BinaryOperation::SAdd: {
        this->exec_int_bin_operation(lhs,
                                     s->has_no_wrap()
                                         ? IntBinaryOperator::AddNoWrap
                                         : IntBinaryOperator::Add,
                                     left,
                                     right);
      } break;
      case ar::BinaryOperation::USub:
      case ar::BinaryOperation::SSub: {
        this->exec_int_bin_operation(lhs,
                                     s->has_no_wrap()
                                         ? IntBinaryOperator::SubNoWrap
                                         : IntBinaryOperator::Sub,
                                     left,
                                     right);
      } break;
      case ar::BinaryOperation::UMul:
      case ar::BinaryOperation::SMul: {
        this->exec_int_bin_operation(lhs,
                                     s->has_no_wrap()
                                         ? IntBinaryOperator::MulNoWrap
                                         : IntBinaryOperator::Mul,
                                     left,
                                     right);
      } break;
      case ar::BinaryOperation::UDiv:
      case ar::BinaryOperation::SDiv: {
        this->exec_int_bin_operation(lhs,
                                     s->is_exact() ? IntBinaryOperator::DivExact
                                                   : IntBinaryOperator::Div,
                                     left,
                                     right);
      } break;
      case ar::BinaryOperation::URem:
      case ar::BinaryOperation::SRem: {
        this->exec_int_bin_operation(lhs, IntBinaryOperator::Rem, left, right);
      } break;
      case ar::BinaryOperation::UShl:
      case ar::BinaryOperation::SShl: {
        this->exec_int_bin_operation(lhs,
                                     s->has_no_wrap()
                                         ? IntBinaryOperator::ShlNoWrap
                                         : IntBinaryOperator::Shl,
                                     left,
                                     right);
      } break;
      case ar::BinaryOperation::ULShr:
      case ar::BinaryOperation::SLShr: {
        this->exec_int_bin_operation(lhs,
                                     s->is_exact()
                                         ? IntBinaryOperator::LShrExact
                                         : IntBinaryOperator::LShr,
                                     left,
                                     right);
      } break;
      case ar::BinaryOperation::UAShr:
      case ar::BinaryOperation::SAShr: {
        this->exec_int_bin_operation(lhs,
                                     s->is_exact()
                                         ? IntBinaryOperator::AShrExact
                                         : IntBinaryOperator::AShr,
                                     left,
                                     right);
      } break;
      case ar::BinaryOperation::UAnd:
      case ar::BinaryOperation::SAnd: {
        this->exec_int_bin_operation(lhs, IntBinaryOperator::And, left, right);
      } break;
      case ar::BinaryOperation::UOr:
      case ar::BinaryOperation::SOr: {
        this->exec_int_bin_operation(lhs, IntBinaryOperator::Or, left, right);
      } break;
      case ar::BinaryOperation::UXor:
      case ar::BinaryOperation::SXor: {
        this->exec_int_bin_operation(lhs, IntBinaryOperator::Add, left, right);
      } break;
      case ar::BinaryOperation::FAdd:
      case ar::BinaryOperation::FSub:
      case ar::BinaryOperation::FMul:
      case ar::BinaryOperation::FDiv:
      case ar::BinaryOperation::FRem: {
        this->exec_float_bin_operation(lhs, left, right);
      } break;
      default: { ikos_unreachable("unreachable"); }
    }

    this->_inv.normal().uninitialized().assign_initialized(lhs.var());
  }

private:
  /// \brief Execute an integer binary operation
  void exec_int_bin_operation(const ScalarLit& lhs,
                              IntBinaryOperator op,
                              const ScalarLit& left,
                              const ScalarLit& right) {
    ikos_assert_msg(lhs.is_machine_int_var(),
                    "left hand side is not an integer variable");

    if (left.is_machine_int()) {
      if (right.is_machine_int()) {
        this->_inv.normal().integers().assign(lhs.var(), left.machine_int());
        this->_inv.normal().integers().apply(op,
                                             lhs.var(),
                                             lhs.var(),
                                             right.machine_int());
      } else if (right.is_machine_int_var()) {
        this->_inv.normal().integers().apply(op,
                                             lhs.var(),
                                             left.machine_int(),
                                             right.var());
      } else {
        ikos_unreachable("unexpected right operand");
      }
    } else if (left.is_machine_int_var()) {
      if (right.is_machine_int()) {
        this->_inv.normal().integers().apply(op,
                                             lhs.var(),
                                             left.var(),
                                             right.machine_int());
      } else if (right.is_machine_int_var()) {
        this->_inv.normal().integers().apply(op,
                                             lhs.var(),
                                             left.var(),
                                             right.var());
      } else {
        ikos_unreachable("unexpected right operand");
      }
    } else {
      ikos_unreachable("unexpected left operand");
    }
  }

  /// \brief Execute a floating point binary operation
  void exec_float_bin_operation(const ScalarLit& lhs,
                                const ScalarLit& /*left*/,
                                const ScalarLit& /*right*/) {
    ikos_assert_msg(lhs.is_floating_point_var(),
                    "left hand side is not a floating point variable");

    // TODO(marthaud): floating point reasoning
    ikos_ignore(lhs);
  }

public:
  /// \brief Execute a Comparison statement
  void exec(ar::Comparison* s) override {
    const ScalarLit& left = this->_lit_factory.get_scalar(s->left());
    const ScalarLit& right = this->_lit_factory.get_scalar(s->right());

    if (left.is_undefined() || right.is_undefined()) {
      this->_inv.set_normal_flow_to_bottom();
      return;
    }

    if (s->is_pointer_predicate()) {
      this->init_global_operand(s->left());
      this->init_global_operand(s->right());
    }

    switch (s->predicate()) {
      case ar::Comparison::UIEQ:
      case ar::Comparison::SIEQ: {
        this->exec_int_comparison(IntPredicate::EQ, left, right);
      } break;
      case ar::Comparison::UINE:
      case ar::Comparison::SINE: {
        this->exec_int_comparison(IntPredicate::NE, left, right);
      } break;
      case ar::Comparison::UIGT:
      case ar::Comparison::SIGT: {
        this->exec_int_comparison(IntPredicate::GT, left, right);
      } break;
      case ar::Comparison::UIGE:
      case ar::Comparison::SIGE: {
        this->exec_int_comparison(IntPredicate::GE, left, right);
      } break;
      case ar::Comparison::UILT:
      case ar::Comparison::SILT: {
        this->exec_int_comparison(IntPredicate::LT, left, right);
      } break;
      case ar::Comparison::UILE:
      case ar::Comparison::SILE: {
        this->exec_int_comparison(IntPredicate::LE, left, right);
      } break;
      case ar::Comparison::FOEQ:
      case ar::Comparison::FOGT:
      case ar::Comparison::FOGE:
      case ar::Comparison::FOLT:
      case ar::Comparison::FOLE:
      case ar::Comparison::FONE:
      case ar::Comparison::FORD:
      case ar::Comparison::FUNO:
      case ar::Comparison::FUEQ:
      case ar::Comparison::FUGT:
      case ar::Comparison::FUGE:
      case ar::Comparison::FULT:
      case ar::Comparison::FULE:
      case ar::Comparison::FUNE: {
        this->exec_float_comparison(left, right);
      } break;
      case ar::Comparison::PEQ: {
        this->exec_ptr_comparison(PtrPredicate::EQ, left, right);
      } break;
      case ar::Comparison::PNE: {
        this->exec_ptr_comparison(PtrPredicate::NE, left, right);
      } break;
      case ar::Comparison::PGT: {
        this->exec_ptr_comparison(PtrPredicate::GT, left, right);
      } break;
      case ar::Comparison::PGE: {
        this->exec_ptr_comparison(PtrPredicate::GE, left, right);
      } break;
      case ar::Comparison::PLT: {
        this->exec_ptr_comparison(PtrPredicate::LT, left, right);
      } break;
      case ar::Comparison::PLE: {
        this->exec_ptr_comparison(PtrPredicate::LE, left, right);
      } break;
      default: { ikos_unreachable("unreachable"); }
    }
  }

private:
  /// \brief Execute an integer comparison
  void exec_int_comparison(IntPredicate pred,
                           const ScalarLit& left,
                           const ScalarLit& right) {
    if (left.is_machine_int()) {
      if (right.is_machine_int()) {
        // TODO(marthaud): check if `left pred right` for MachineInt
      } else if (right.is_machine_int_var()) {
        this->_inv.normal().integers().add(pred,
                                           left.machine_int(),
                                           right.var());
      } else {
        ikos_unreachable("unexpected right operand");
      }
    } else if (left.is_machine_int_var()) {
      if (right.is_machine_int()) {
        this->_inv.normal().integers().add(pred,
                                           left.var(),
                                           right.machine_int());
      } else if (right.is_machine_int_var()) {
        this->_inv.normal().integers().add(pred, left.var(), right.var());
      } else {
        ikos_unreachable("unexpected right operand");
      }
    } else {
      ikos_unreachable("unexpected left operand");
    }
  }

  /// \brief Execute a floating point comparison
  void exec_float_comparison(const ScalarLit& /*left*/,
                             const ScalarLit& /*right*/) {
    // TODO(marthaud): no floating point reasoning
  }

  /// \brief Execute a pointer comparison
  void exec_ptr_comparison(PtrPredicate pred,
                           const ScalarLit& left,
                           const ScalarLit& right) {
    if (this->_precision < Precision::Pointer) {
      return;
    } else if (left.is_null()) {
      if (right.is_null()) {
        // Compare `null pred null`
        if (pred == PtrPredicate::NE || pred == PtrPredicate::GT ||
            pred == PtrPredicate::LT) {
          this->_inv.set_normal_flow_to_bottom();
        }
      } else if (right.is_pointer_var()) {
        // Compare `null pred p`
        this->refine_addresses(right.var());
        if (pred == PtrPredicate::EQ) {
          this->_inv.normal().pointers().assert_null(right.var());
        } else if (pred == PtrPredicate::NE || pred == PtrPredicate::GT ||
                   pred == PtrPredicate::LT) {
          this->_inv.normal().pointers().assert_non_null(right.var());
        }
      } else {
        ikos_unreachable("unexpected right operand");
      }
    } else if (left.is_pointer_var()) {
      if (right.is_null()) {
        // Compare `p pred null`
        this->refine_addresses(left.var());
        if (pred == PtrPredicate::EQ) {
          this->_inv.normal().pointers().assert_null(left.var());
        } else if (pred == PtrPredicate::NE || pred == PtrPredicate::GT ||
                   pred == PtrPredicate::LT) {
          this->_inv.normal().pointers().assert_non_null(left.var());
        }
      } else if (right.is_pointer_var()) {
        // Compare `p pred q`

        // Reduction with the external pointer analysis
        this->refine_addresses_offset(left.var());
        this->refine_addresses_offset(right.var());

        this->_inv.normal().pointers().add(pred, left.var(), right.var());
      } else {
        ikos_unreachable("unexpected right operand");
      }
    } else {
      ikos_unreachable("unexpected left operand");
    }
  }

public:
  /// \brief Execute an Unreachable statement
  void exec(ar::Unreachable*) override {
    // Unreachable should propagate exceptions
    this->_inv.set_normal_flow_to_bottom();
  }

  /// \brief Execute an Allocate statement
  void exec(ar::Allocate* s) override {
    if (this->_precision < Precision::Pointer) {
      return;
    }

    const ScalarLit& lhs = this->_lit_factory.get_scalar(s->result());
    const ScalarLit& array_size =
        this->_lit_factory.get_scalar(s->array_size());
    ikos_assert_msg(lhs.is_pointer_var(),
                    "left hand side is not a pointer variable");

    // Allocate the memory
    MemoryLocation* addr = this->_mem_factory.get_local(s->result());
    this->allocate_memory(lhs.var(),
                          addr,
                          Nullity::non_null(),
                          Uninitialized::initialized(),
                          Lifetime::allocated(),
                          MemoryInitialValue::Uninitialized);

    // Set the alloc size symbolic variable
    Variable* alloc_size_var = this->_var_factory.get_alloc_size(addr);
    MachineInt element_size(this->_data_layout.alloc_size_in_bytes(
                                s->allocated_type()),
                            this->_data_layout.pointers.bit_width,
                            Unsigned);
    if (array_size.is_machine_int()) {
      bool overflow;
      MachineInt alloc_size_int =
          mul(array_size.machine_int(), element_size, overflow);
      if (overflow) {
        this->_inv.set_normal_flow_to_bottom(); // undefined behavior
      } else {
        this->_inv.normal().integers().assign(alloc_size_var, alloc_size_int);
      }
    } else if (array_size.is_machine_int_var()) {
      this->_inv.normal().integers().apply(IntBinaryOperator::MulNoWrap,
                                           alloc_size_var,
                                           array_size.var(),
                                           element_size);
    } else {
      ikos_unreachable("unexpected array size parameter");
    }
  }

  /// \brief Execute a PointerShift statement
  void exec(ar::PointerShift* s) override {
    if (this->_precision < Precision::Pointer) {
      return;
    }

    // initialize lazily global objects
    this->init_global_operand(s->pointer());

    const ScalarLit& lhs = this->_lit_factory.get_scalar(s->result());
    const ScalarLit& base = this->_lit_factory.get_scalar(s->pointer());
    ikos_assert_msg(lhs.is_pointer_var(),
                    "left hand side is not a pointer variable");

    if (base.is_undefined()) {
      this->_inv.set_normal_flow_to_bottom();
      return;
    }

    ikos_assert_msg(base.is_null() || base.is_pointer_var(),
                    "unexpected base operand");

    // Update the pointer info, offset, nullity and uninitialized variables
    unsigned bit_width = this->_data_layout.pointers.bit_width;
    IntLinearExpression offset_expr(MachineInt::zero(bit_width, Unsigned));

    for (auto it = s->term_begin(), et = s->term_end(); it != et; ++it) {
      auto term = *it;
      const ScalarLit& offset = this->_lit_factory.get_scalar(term.second);

      if (offset.is_undefined()) {
        this->_inv.set_normal_flow_to_bottom();
        return;
      }

      if (offset.is_machine_int()) {
        offset_expr.add(
            mul(term.first, offset.machine_int().cast(bit_width, Unsigned)));
      } else if (offset.is_machine_int_var()) {
        offset_expr.add(term.first, offset.var());
      } else {
        ikos_unreachable("unexpected offset operand");
      }
    }

    if (base.is_null()) {
      this->_inv.normal().pointers().assign_address(lhs.var(),
                                                    this->_mem_factory
                                                        .get_absolute_zero(),
                                                    Nullity::null());
      this->_inv.normal().pointers().assign(lhs.var(), lhs.var(), offset_expr);
    } else {
      this->_inv.normal().pointers().assign(lhs.var(), base.var(), offset_expr);
    }

    this->_inv.normal().uninitialized().assign_initialized(lhs.var());
    this->normalize_absolute_zero_nullity(lhs.var());
  }

  /// \brief Execute a Load statement
  void exec(ar::Load* s) override {
    // initialize lazily global objects
    this->init_global_operand(s->operand());

    const ScalarLit& ptr = this->_lit_factory.get_scalar(s->operand());

    if (!this->prepare_mem_access(ptr)) {
      return;
    }

    const Literal& result = this->_lit_factory.get(s->result());

    MachineInt size(this->_data_layout.store_size_in_bytes(s->result()->type()),
                    this->_data_layout.pointers.bit_width,
                    Unsigned);

    if (result.is_scalar()) {
      const ScalarLit& lhs = result.scalar();
      ikos_assert_msg(lhs.is_var(), "left hand side is not a variable");

      if (!s->is_volatile()) {
        // perform memory read in the value domain
        this->_inv.normal().mem_read(this->_var_factory, lhs, ptr.var(), size);
      } else {
        this->_inv.normal().forget_surface(lhs.var());
      }

      // reduction between value and pointer analysis
      if (lhs.is_pointer_var()) {
        this->refine_addresses_offset(lhs.var());
      }
    } else if (result.is_aggregate()) {
      const AggregateLit& lhs = result.aggregate();
      ikos_assert_msg(lhs.is_var(), "left hand side is not a variable");

      this->init_aggregate_memory(lhs);
      ScalarLit lhs_ptr = this->aggregate_pointer(lhs);

      if (!s->is_volatile()) {
        // perform memory read in the value domain
        this->_inv.normal().mem_copy(this->_var_factory,
                                     lhs_ptr.var(),
                                     ptr.var(),
                                     ScalarLit::machine_int(size));
      } else {
        this->_inv.normal().forget_reachable_mem(lhs_ptr.var());
      }
    } else {
      ikos_unreachable("unexpected left hand side");
    }
  }

  /// \brief Execute a Store statement
  void exec(ar::Store* s) override {
    // initialize lazily global objects
    this->init_global_operand(s->pointer());

    const ScalarLit& ptr = this->_lit_factory.get_scalar(s->pointer());

    if (!this->prepare_mem_access(ptr)) {
      return;
    }

    if (this->_inv.normal().pointers().points_to(ptr.var()).is_top()) {
      // Ignore memory write, analysis could be unsound.
      // See CheckKind::IgnoredUnknownStore
      return;
    }

    // initialize lazily global objects
    this->init_global_operand(s->value());

    const Literal& val = this->_lit_factory.get(s->value());

    MachineInt size(this->_data_layout.store_size_in_bytes(s->value()->type()),
                    this->_data_layout.pointers.bit_width,
                    Unsigned);

    if (val.is_scalar()) {
      const ScalarLit& rhs = val.scalar();

      if (rhs.is_undefined()) {
        this->_inv.set_normal_flow_to_bottom();
        return;
      }

      if (rhs.is_var()) {
        // Store of undefined is U.B. (but checked by uva)
        // if it's successful, assume that the variable is initialized
        this->_inv.normal().uninitialized().assign_initialized(rhs.var());
      }

      if (rhs.is_pointer_var()) {
        this->refine_addresses_offset(rhs.var());
      }

      // perform memory write in the value domain
      this->_inv.normal().mem_write(this->_var_factory, ptr.var(), rhs, size);
    } else if (val.is_aggregate()) {
      this->mem_write_aggregate(ptr, val.aggregate());
    } else {
      ikos_unreachable("unexpected right hand side");
    }
  }

  /// \brief Execute an ExtractElement statement
  void exec(ar::ExtractElement* s) override {
    const Literal& lhs = this->_lit_factory.get(s->result());
    const AggregateLit& rhs = this->_lit_factory.get_aggregate(s->aggregate());
    const ScalarLit& offset = this->_lit_factory.get_scalar(s->offset());
    ikos_assert_msg(rhs.is_var(), "right hand side is not a variable");

    if (this->_precision < Precision::Memory) {
      return;
    }

    ScalarLit rhs_ptr = this->aggregate_pointer(rhs);
    ScalarLit read_ptr = ScalarLit::pointer_var(
        this->_var_factory.get_named_shadow(this->void_ptr_type(),
                                            "shadow.extract_element.ptr"));
    this->pointer_shift(read_ptr, rhs_ptr, offset);

    MachineInt size(this->_data_layout.store_size_in_bytes(s->result()->type()),
                    this->_data_layout.pointers.bit_width,
                    Unsigned);

    if (lhs.is_scalar()) {
      ikos_assert_msg(lhs.scalar().is_var(),
                      "left hand side is not a variable");

      this->_inv.normal().mem_read(this->_var_factory,
                                   lhs.scalar(),
                                   read_ptr.var(),
                                   size);
    } else if (lhs.is_aggregate()) {
      ikos_assert_msg(lhs.aggregate().is_var(),
                      "left hand side is not a variable");

      this->init_aggregate_memory(lhs.aggregate());
      ScalarLit lhs_ptr = this->aggregate_pointer(lhs.aggregate());
      this->_inv.normal().mem_copy(this->_var_factory,
                                   lhs_ptr.var(),
                                   read_ptr.var(),
                                   ScalarLit::machine_int(size));
    } else {
      ikos_unreachable("unexpected left hand side");
    }

    // clean-up
    this->_inv.normal().forget_surface(read_ptr.var());
  }

  /// \brief Execute an InsertElement statement
  void exec(ar::InsertElement* s) override {
    const AggregateLit& lhs = this->_lit_factory.get_aggregate(s->result());
    const AggregateLit& rhs = this->_lit_factory.get_aggregate(s->aggregate());
    const ScalarLit& offset = this->_lit_factory.get_scalar(s->offset());
    const Literal& element = this->_lit_factory.get(s->element());
    ikos_assert_msg(lhs.is_var(), "left hand side is not a variable");

    if (this->_precision < Precision::Memory) {
      return;
    }

    this->init_aggregate_memory(lhs);
    ScalarLit lhs_ptr = this->aggregate_pointer(lhs);

    // first, copy the aggregate value
    this->mem_write_aggregate(lhs_ptr, rhs);

    // then insert the element
    ScalarLit write_ptr = ScalarLit::pointer_var(
        this->_var_factory.get_named_shadow(this->void_ptr_type(),
                                            "shadow.insert_element.ptr"));
    this->pointer_shift(write_ptr, lhs_ptr, offset);

    MachineInt size(this->_data_layout.store_size_in_bytes(
                        s->element()->type()),
                    this->_data_layout.pointers.bit_width,
                    Unsigned);

    if (element.is_scalar()) {
      this->_inv.normal().mem_write(this->_var_factory,
                                    write_ptr.var(),
                                    element.scalar(),
                                    size);
    } else if (element.is_aggregate()) {
      this->mem_write_aggregate(write_ptr, element.aggregate());
    } else {
      ikos_unreachable("unexpected element operand");
    }

    // clean-up
    this->_inv.normal().forget_surface(write_ptr.var());
  }

  /// \brief Execute a LandingPad statement
  void exec(ar::LandingPad*) override {}

  /// \brief Execute a Resume statement
  void exec(ar::Resume*) override { this->_inv.resume_exception(); }

  /// @}
  /// \name Execute call statements
  /// @{

  /// \brief Execute a call to the given extern function
  void exec_extern_call(ar::CallBase* call, ar::Function* fun) override {
    ikos_assert(fun->is_declaration());
    ikos_assert(ar::TypeVerifier::is_valid_call(call, fun->type()));

    if (this->_inv.is_normal_flow_bottom()) {
      return;
    }

    if (fun->is_intrinsic()) {
      this->exec_intrinsic_call(call, fun);
    } else {
      this->exec_unknown_extern_call(call);
    }
  }

private:
  /// \brief Execute a call to the given intrinsic function
  void exec_intrinsic_call(ar::CallBase* call, ar::Function* fun) {
    switch (fun->intrinsic_id()) {
      case ar::Intrinsic::MemoryCopy: {
        this->exec_memcpy_or_memmove(call);
      } break;
      case ar::Intrinsic::MemoryMove: {
        this->exec_memcpy_or_memmove(call);
      } break;
      case ar::Intrinsic::MemorySet: {
        this->exec_memset(call);
      } break;
      case ar::Intrinsic::LibcMalloc: {
        this->exec_malloc(call);
      } break;
      case ar::Intrinsic::LibcCalloc: {
        this->exec_calloc(call);
      } break;
      case ar::Intrinsic::LibcppNew:
      case ar::Intrinsic::LibcppNewArray: {
        this->exec_new(call);
      } break;
      case ar::Intrinsic::LibcppAllocateException: {
        this->exec_allocate_exception(call);
      } break;
      case ar::Intrinsic::LibcFree:
      case ar::Intrinsic::LibcppDelete:
      case ar::Intrinsic::LibcppDeleteArray:
      case ar::Intrinsic::LibcppFreeException: {
        // TODO(marthaud): delete[] also calls the destructor on each element
        this->exec_free(call);
      } break;
      case ar::Intrinsic::LibcRead: {
        this->exec_read(call);
      } break;
      case ar::Intrinsic::LibcppThrow: {
        this->exec_throw(call);
      } break;
      case ar::Intrinsic::LibcppBeginCatch: {
        this->exec_begin_catch(call);
      } break;
      case ar::Intrinsic::LibcStrlen: {
        this->exec_strlen(call);
      } break;
      case ar::Intrinsic::LibcStrnlen: {
        this->exec_strnlen(call);
      } break;
      case ar::Intrinsic::LibcStrcpy: {
        this->exec_strcpy(call);
      } break;
      case ar::Intrinsic::LibcStrncpy: {
        this->exec_strncpy(call);
      } break;
      case ar::Intrinsic::LibcStrcat: {
        this->exec_strcat(call);
      } break;
      case ar::Intrinsic::LibcStrncat: {
        this->exec_strncat(call);
      } break;
      case ar::Intrinsic::IkosAssert:
      case ar::Intrinsic::IkosAssume:
      case ar::Intrinsic::IkosPrintInvariant:
      case ar::Intrinsic::IkosPrintValues: {
        // Nothing to do
        ikos_assert(!call->has_result());
      } break;
      case ar::Intrinsic::IkosNonDetSi32:
      case ar::Intrinsic::IkosNonDetUi32: {
        this->exec_ikos_non_det(call);
      } break;
      case ar::Intrinsic::IkosCounterInit: {
        this->exec_ikos_counter_init(call);
      } break;
      case ar::Intrinsic::IkosCounterIncr: {
        this->exec_ikos_counter_incr(call);
      } break;
      default: { this->exec_unknown_extern_call(call); } break;
    }

    // TODO(marthaud): support va_start, va_end, va_copy
  }

public:
  /// \brief Execute a call to an unknown extern function
  void exec_unknown_extern_call(ar::CallBase* call) override {
    if (this->_inv.is_normal_flow_bottom()) {
      return;
    }

    // Forget all parameters of pointer type (very conservative)
    if (this->_precision >= Precision::Memory) {
      for (auto it = call->arg_begin(), et = call->arg_end(); it != et; ++it) {
        ar::Value* arg = *it;
        if (!arg->type()->is_pointer()) {
          continue;
        }

        const ScalarLit& ptr = this->_lit_factory.get_scalar(arg);
        if (!ptr.is_pointer_var()) {
          continue;
        }

        this->init_global_operand(arg);
        this->refine_addresses(ptr.var());

        if (this->_inv.normal().uninitialized().is_uninitialized(ptr.var())) {
          this->_inv.set_normal_flow_to_bottom(); // undefined behavior
          return;
        } else if (this->_inv.normal().nullity().is_null(ptr.var())) {
          continue; // safe
        } else if (this->_inv.normal()
                       .pointers()
                       .points_to(ptr.var())
                       .is_top()) {
          // Ignore side effect on the memory, analysis could be unsound.
          // See CheckKind::IgnoredCallSideEffect
          continue;
        } else {
          this->_inv.normal().forget_reachable_mem(ptr.var());
        }
      }
    }

    // Forget the result
    if (call->has_result()) {
      const Literal& ret = this->_lit_factory.get(call->result());

      if (ret.is_scalar()) {
        ikos_assert_msg(ret.scalar().is_var(),
                        "left hand side is not a variable");

        this->_inv.normal().forget_surface(ret.scalar().var());
      } else if (ret.is_aggregate()) {
        ikos_assert_msg(ret.aggregate().is_var(),
                        "left hand side is not a variable");

        if (this->_precision >= Precision::Memory) {
          ScalarLit ret_ptr = this->aggregate_pointer(ret.aggregate());
          this->_inv.normal().forget_reachable_mem(ret_ptr.var());
        }
      } else {
        ikos_unreachable("unexpected left hand side");
      }
    }

    // The external call can throw exceptions
    this->throw_unknown_exceptions();

    // Initialize the result
    if (call->has_result()) {
      const Literal& ret = this->_lit_factory.get(call->result());

      if (ret.is_scalar()) {
        // ASSUMPTION:
        // The claim about the correctness of the program under analysis can be
        // made only if all calls to unavailable code are assumed to be correct
        // and without side-effects. We will assume that the lhs of an external
        // call site is always initialized. However, in case of a pointer, we do
        // not assume that a non-null pointer is returned.
        this->_inv.normal().uninitialized().assign_initialized(
            ret.scalar().var());
      }
    }
  }

  /// \brief Execute a call to an unknown internal function
  void exec_unknown_intern_call(ar::CallBase* call) override {
    if (this->_inv.is_normal_flow_bottom()) {
      return;
    }

    if (this->_precision >= Precision::Memory) {
      // Forget all memory contents
      this->_inv.normal().forget_mem();
    }

    // Forget the result
    if (call->has_result()) {
      const Literal& ret = this->_lit_factory.get(call->result());

      if (ret.is_scalar()) {
        ikos_assert_msg(ret.scalar().is_var(),
                        "left hand side is not a variable");

        this->_inv.normal().forget_surface(ret.scalar().var());
      } else if (ret.is_aggregate()) {
        // Nothing to do, because we already forgot all memory contents
      } else {
        ikos_unreachable("unexpected left hand side");
      }
    }

    // Might throw exceptions
    this->throw_unknown_exceptions();
  }

private:
  ///
  /// @}
  /// \name Execution of intrinsic functions
  /// @{

  /// \brief Execute a call to memcpy(dest, src, len) or memmove(dest, src, len)
  void exec_memcpy_or_memmove(ar::CallBase* call) {
    // Initialize lazily global objects
    this->init_global_operand(call->argument(0));
    this->init_global_operand(call->argument(1));

    // Both src and dest must be already allocated in memory so offsets and
    // sizes for both src and dest are already part of the invariants
    const ScalarLit& dest = this->_lit_factory.get_scalar(call->argument(0));
    const ScalarLit& src = this->_lit_factory.get_scalar(call->argument(1));
    const ScalarLit& size = this->_lit_factory.get_scalar(call->argument(2));

    if (!this->prepare_mem_access(dest) || !this->prepare_mem_access(src)) {
      return;
    }

    if (this->_inv.normal().pointers().points_to(dest.var()).is_top()) {
      // Ignore memory copy/move, analysis could be unsound.
      // See CheckKind::IgnoredMemoryCopy, CheckKind::IgnoredMemoryMove
      return;
    }

    if (cast< ar::IntegerConstant >(call->argument(5))->value() == 0) {
      // non-volatile
      this->_inv.normal().mem_copy(this->_var_factory,
                                   dest.var(),
                                   src.var(),
                                   size);
    } else {
      // volatile
      IntInterval s;
      if (size.is_machine_int()) {
        s = IntInterval(size.machine_int());
      } else if (size.is_machine_int_var()) {
        s = this->_inv.normal().integers().to_interval(size.var());
      } else {
        ikos_unreachable("unreachable");
      }
      this->_inv.normal().forget_reachable_mem(dest.var(), s.ub());
    }
  }

  /// \brief Execute a call to memset(dest, byte, len)
  void exec_memset(ar::CallBase* call) {
    // Initialize lazily global objects
    this->init_global_operand(call->argument(0));

    const ScalarLit& dest = this->_lit_factory.get_scalar(call->argument(0));
    const ScalarLit& value = this->_lit_factory.get_scalar(call->argument(1));
    const ScalarLit& size = this->_lit_factory.get_scalar(call->argument(2));

    ikos_assert_msg(value.is_machine_int_var() || value.is_machine_int(),
                    "unexpected value operand");
    ikos_assert_msg(size.is_machine_int_var() || size.is_machine_int(),
                    "unexpected size operand");

    if (!this->prepare_mem_access(dest)) {
      return;
    }

    if (this->_inv.normal().pointers().points_to(dest.var()).is_top()) {
      // Ignore memory set, analysis could be unsound.
      // See CheckKind::IgnoredMemorySet
      return;
    }

    this->_inv.normal().mem_set(this->_var_factory, dest.var(), value, size);
  }

  /// \brief Execute a dynamic allocation
  void exec_dynamic_alloc(ar::CallBase* call,
                          ar::Value* size,
                          bool may_return_null,
                          bool may_throw_exc,
                          MemoryInitialValue init_val) {
    if (this->_precision < Precision::Pointer) {
      return;
    }

    if (!call->has_result()) {
      return;
    }

    const ScalarLit& lhs = this->_lit_factory.get_scalar(call->result());
    const ScalarLit& size_l = this->_lit_factory.get_scalar(size);
    ikos_assert_msg(lhs.is_pointer_var(),
                    "left hand side is not a pointer variable");

    this->_inv.normal().forget_surface(lhs.var());

    if (may_throw_exc) {
      this->throw_unknown_exceptions();
    }

    Nullity null_val = may_return_null ? Nullity::top() : Nullity::non_null();

    MemoryLocation* addr =
        this->_mem_factory.get_dyn_alloc(call, this->_call_context);

    if (size_l.is_machine_int_var()) {
      this->allocate_memory(lhs.var(),
                            addr,
                            null_val,
                            Uninitialized::initialized(),
                            Lifetime::allocated(),
                            init_val,
                            size_l.var());
    } else if (size_l.is_machine_int()) {
      this->allocate_memory(lhs.var(),
                            addr,
                            null_val,
                            Uninitialized::initialized(),
                            Lifetime::allocated(),
                            init_val,
                            size_l.machine_int());
    } else {
      ikos_unreachable("unexpected size operand");
    }
  }

  /// \brief Execute a libc malloc call
  ///
  /// #include <stdlib.h>
  /// void* malloc(size_t size)
  ///
  /// This function returns a pointer to a newly allocated block size bytes
  /// long, or a null pointer if the block could not be allocated.
  void exec_malloc(ar::CallBase* call) {
    this->exec_dynamic_alloc(call,
                             call->argument(0),
                             /* may_return_null = */ true,
                             /* may_throw_exc = */ false,
                             MemoryInitialValue::Uninitialized);
  }

  /// \brief Execute a libc calloc call
  ///
  /// #include <stdlib.h>
  /// void* calloc(size_t count, size_t size)
  ///
  /// The calloc() function contiguously allocates enough space for count
  /// objects that are size bytes of memory each and returns a pointer to the
  /// allocated memory. The allocated memory is filled with bytes of value zero.
  void exec_calloc(ar::CallBase* call) {
    if (this->_precision < Precision::Pointer) {
      return;
    }

    if (!call->has_result()) {
      return;
    }

    const ScalarLit& lhs = this->_lit_factory.get_scalar(call->result());
    const ScalarLit& count = this->_lit_factory.get_scalar(call->argument(0));
    const ScalarLit& size = this->_lit_factory.get_scalar(call->argument(1));
    ikos_assert_msg(lhs.is_pointer_var(),
                    "left hand side is not a pointer variable");

    // Allocate the memory
    MemoryLocation* addr =
        this->_mem_factory.get_dyn_alloc(call, this->_call_context);
    this->allocate_memory(lhs.var(),
                          addr,
                          Nullity::top(),
                          Uninitialized::initialized(),
                          Lifetime::allocated(),
                          MemoryInitialValue::Zero);

    // Set the alloc size symbolic variable
    Variable* alloc_size_var = this->_var_factory.get_alloc_size(addr);
    if (count.is_machine_int()) {
      if (size.is_machine_int()) {
        bool overflow;
        MachineInt alloc_size_int =
            mul(count.machine_int(), size.machine_int(), overflow);
        if (overflow) {
          this->_inv.set_normal_flow_to_bottom(); // undefined behavior
        } else {
          this->_inv.normal().integers().assign(alloc_size_var, alloc_size_int);
        }
      } else if (size.is_machine_int_var()) {
        this->_inv.normal().integers().apply(IntBinaryOperator::MulNoWrap,
                                             alloc_size_var,
                                             count.machine_int(),
                                             size.var());
      } else {
        ikos_unreachable("unexpected size parameter");
      }
    } else if (count.is_machine_int_var()) {
      if (size.is_machine_int()) {
        this->_inv.normal().integers().apply(IntBinaryOperator::MulNoWrap,
                                             alloc_size_var,
                                             count.var(),
                                             size.machine_int());
      } else if (size.is_machine_int_var()) {
        this->_inv.normal().integers().apply(IntBinaryOperator::MulNoWrap,
                                             alloc_size_var,
                                             count.var(),
                                             size.var());
      } else {
        ikos_unreachable("unexpected size parameter");
      }
    } else {
      ikos_unreachable("unexpected count parameter");
    }
  }

  /// \brief Execute a libcpp new or new[]
  ///
  /// operator new(unsigned long)
  /// operator new[](unsigned long)
  ///
  /// Allocates requested number of bytes. These allocation functions are called
  /// by new-expressions to allocate memory in which new object would then be
  /// initialized. They may also be called using regular function call syntax.
  void exec_new(ar::CallBase* call) {
    this->exec_dynamic_alloc(call,
                             call->argument(0),
                             /* may_return_null = */ false,
                             /* may_throw_exc = */ true,
                             MemoryInitialValue::Uninitialized);
  }

  /// \brief Execute a libcpp allocate exception
  ///
  /// void* __cxa_allocate_exception(size_t thrown_size) throw();
  ///
  /// Allocates memory to hold the exception to be thrown. thrown_size is the
  /// size of the exception object. Can allocate additional memory to hold
  /// private data. If memory can not be allocated, call std::terminate().
  void exec_allocate_exception(ar::CallBase* call) {
    this->exec_dynamic_alloc(call,
                             call->argument(0),
                             /* may_return_null = */ false,
                             /* may_throw_exc = */ false,
                             MemoryInitialValue::Uninitialized);
  }

  /// \brief Execute a libc free, libcpp delete or delete[], etc.
  ///
  /// #include <stdlib.h>
  /// void free(void* ptr)
  ///
  /// This function deallocates the memory allocated via a previous call to
  /// malloc().
  void exec_free(ar::CallBase* call) {
    const ScalarLit& ptr = this->_lit_factory.get_scalar(call->argument(0));

    if (ptr.is_null()) {
      // this is safe, according to C/C++ standards
      return;
    }

    ikos_assert_msg(ptr.is_pointer_var(), "unexpected parameter");

    if (this->_precision < Precision::Pointer) {
      return;
    }

    if (this->_inv.normal().nullity().is_null(ptr.var())) {
      // this is safe, according to C/C++ standards
      return;
    }

    // Reduction between value and pointer analysis
    this->refine_addresses(ptr.var());

    PointsToSet points_to = this->_inv.normal().pointers().points_to(ptr.var());

    if (points_to.is_bottom()) {
      return;
    } else if (points_to.is_top()) {
      // Ignored memory deallocation, analysis could be unsound.
      // See CheckKind::IgnoredFree
      return;
    }

    if (this->_precision >= Precision::Memory) {
      // Forget memory contents
      this->_inv.normal().forget_reachable_mem(ptr.var());
    }

    // Forget the allocated size and set the new lifetime
    for (auto addr : points_to) {
      if (!isa< DynAllocMemoryLocation >(addr)) {
        if (points_to.size() == 1) {
          // This is an error
          this->_inv.set_normal_flow_to_bottom();
          return;
        } else {
          continue;
        }
      }

      Variable* alloc_size_var = this->_var_factory.get_alloc_size(addr);
      this->_inv.normal().integers().forget(alloc_size_var);
      if (points_to.size() == 1) {
        this->_inv.normal().lifetime().assign_deallocated(addr);
      } else {
        this->_inv.normal().lifetime().forget(addr);
      }
    }
  }

  /// \brief Execute a libc read call
  ///
  /// #include <fcntl.h>
  /// int read(int handle, void* buffer, int nbyte);
  ///
  /// The read() function attempts to read nbytes from the file associated with
  /// handle, and places the characters read into buffer. If the file is opened
  /// using O_TEXT, it removes carriage returns and detects the end of the file.
  ///
  /// The function returns the number of bytes read. On end-of-file, 0 is
  /// returned, on error it returns -1, setting errno to indicate the type of
  /// error that occurred.
  void exec_read(ar::CallBase* call) {
    if (call->has_result()) {
      const ScalarLit& lhs = this->_lit_factory.get_scalar(call->result());
      ikos_assert_msg(lhs.is_machine_int_var(),
                      "left hand side is not an integer variable");

      this->_inv.normal().integers().forget(lhs.var());
      this->_inv.normal().uninitialized().assign_initialized(lhs.var());
    }

    if (this->_precision < Precision::Memory) {
      return;
    }

    // Initialize lazily global objects
    this->init_global_operand(call->argument(1));

    const ScalarLit& ptr = this->_lit_factory.get_scalar(call->argument(1));
    const ScalarLit& size = this->_lit_factory.get_scalar(call->argument(2));

    if (!this->prepare_mem_access(ptr)) {
      return;
    }

    if (size.is_machine_int()) {
      this->_inv.normal().abstract_reachable_mem(ptr.var(), size.machine_int());
    } else if (size.is_machine_int_var()) {
      IntInterval size_intv =
          this->_inv.normal().integers().to_interval(size.var());
      this->_inv.normal().abstract_reachable_mem(ptr.var(), size_intv.ub());
    } else {
      ikos_unreachable("unreachable");
    }
  }

  /// \brief Execute a libcpp throw call
  ///
  /// __cxa_throw(void* exception, std::type_info* tinfo, void (*dest)(void*))
  ///
  /// After constructing the exception object with the throw argument value, the
  /// generated code calls the __cxa_throw runtime library routine. This routine
  /// never returns.
  void exec_throw(ar::CallBase* /*call*/) { this->_inv.throw_exception(); }

  /// \brief Execute a libcpp begin catch
  ///
  /// void* __cxa_begin_catch(void* exceptionObject) throw();
  ///
  /// When entering a catch scope, __cxa_begin_catch is called with the
  /// exceptionObject. This routine returns the adjusted pointer to the
  /// exception object.
  /// We assume that it doesn't modify exceptionObject, and the return value
  /// is equals to exceptionObject.
  void exec_begin_catch(ar::CallBase* call) {
    if (!call->has_result()) {
      return;
    }
    const ScalarLit& exception_obj =
        this->_lit_factory.get_scalar(call->argument(0));
    this->assign(this->_lit_factory.get_scalar(call->result()), exception_obj);
  }

  /// \brief Execute a libc strlen call
  ///
  /// #include <string.h>
  /// size_t strlen(const char* s);
  ///
  /// The strlen() function computes the length of the string s.
  ///
  /// The strlen() function returns the number of characters that precede the
  /// terminating NULL character.
  void exec_strlen(ar::CallBase* call) {
    // Initialize lazily global objects
    this->init_global_operand(call->argument(0));

    const ScalarLit& str = this->_lit_factory.get_scalar(call->argument(0));

    if (!this->prepare_mem_access(str)) {
      return;
    }

    if (!call->has_result()) {
      return;
    }

    const ScalarLit& lhs = this->_lit_factory.get_scalar(call->result());
    ikos_assert_msg(lhs.is_machine_int_var(),
                    "left hand side is not an integer variable");

    // lhs is in [0, size - 1]
    this->_inv.normal().integers().forget(lhs.var());
    this->_inv.normal().uninitialized().assign_initialized(lhs.var());

    if (this->_inv.is_normal_flow_bottom()) {
      return;
    }

    PointsToSet points_to = this->_inv.normal().pointers().points_to(str.var());

    if (points_to.is_top()) {
      return;
    }

    AbstractDomain inv(AbstractDomain::bottom());

    for (MemoryLocation* addr : points_to) {
      AbstractDomain tmp(this->_inv);

      if (auto gv = dyn_cast< GlobalMemoryLocation >(addr)) {
        MachineInt alloc_size(this->_data_layout.store_size_in_bytes(
                                  gv->global_var()->type()->pointee()),
                              this->_data_layout.pointers.bit_width,
                              Unsigned);
        tmp.normal().integers().add(IntPredicate::LT, lhs.var(), alloc_size);
      } else {
        Variable* size_var = this->_var_factory.get_alloc_size(addr);
        tmp.normal().integers().add(IntPredicate::LT, lhs.var(), size_var);
      }

      inv.join_with(tmp);
    }

    this->_inv = std::move(inv);
  }

  /// \brief Execute a libc strlen call
  ///
  /// #include <string.h>
  /// size_t strnlen(const char* s, size_t maxlen);
  ///
  /// The strnlen() function attempts to compute the length of s, but never
  /// scans beyond the first maxlen bytes of s.
  ///
  /// The strnlen() function returns either the same result as strlen() or
  /// maxlen, whichever is smaller.
  void exec_strnlen(ar::CallBase* call) {
    this->exec_strlen(call);

    if (this->_inv.is_normal_flow_bottom()) {
      return;
    }

    if (!call->has_result()) {
      return;
    }

    // lhs <= maxlen
    const ScalarLit& lhs = this->_lit_factory.get_scalar(call->result());
    const ScalarLit& maxlen = this->_lit_factory.get_scalar(call->argument(1));

    if (maxlen.is_machine_int()) {
      this->_inv.normal().integers().add(IntPredicate::LE,
                                         lhs.var(),
                                         maxlen.machine_int());
    } else if (maxlen.is_machine_int_var()) {
      this->_inv.normal().integers().add(IntPredicate::LE,
                                         lhs.var(),
                                         maxlen.var());
    } else {
      ikos_unreachable("unexpected maxlen parameter");
    }
  }

  /// \brief Execute a libc strcpy call
  ///
  /// #include <string.h>
  /// char* strcpy(char* dst, const char* src);
  ///
  /// The strcpy() function copies the string src to dst (including the
  /// terminating `\0' character).
  ///
  /// The strcpy() function returns dst.
  void exec_strcpy(ar::CallBase* call) {
    // Initialize lazily global objects
    this->init_global_operand(call->argument(0));
    this->init_global_operand(call->argument(1));

    const ScalarLit& dest = this->_lit_factory.get_scalar(call->argument(0));
    const ScalarLit& src = this->_lit_factory.get_scalar(call->argument(1));

    if (!this->prepare_mem_access(dest) || !this->prepare_mem_access(src)) {
      return;
    }

    // Do not keep track of the content
    this->_inv.normal().forget_reachable_mem(dest.var());

    if (call->has_result()) {
      this->assign(this->_lit_factory.get_scalar(call->result()), dest);
    }
  }

  /// \brief Execute a libc strncpy call
  ///
  /// #include <string.h>
  /// char* strncpy(char* dst, const char* src, size_t n);
  ///
  /// The strncpy() function copies at most n characters from src into dst.
  /// If src is less than n characters long, the remainder of dst is filled with
  /// `\0' characters. Otherwise, dst is not terminated.
  ///
  /// The strncpy() function returns dst.
  void exec_strncpy(ar::CallBase* call) { this->exec_strcpy(call); }

  /// \brief Execute a libc strcat call
  ///
  /// #include <string.h>
  /// char* strcat(char* s1, const char* s2);
  ///
  /// The strcat() function appends a copy of the null-terminated string s2 to
  /// the end of the null-terminated string s1, then add a terminating \0. The
  /// string s1 must have sufficient space to hold the result.
  ///
  /// The strcat() function returns the pointer s1.
  void exec_strcat(ar::CallBase* call) {
    // Initialize lazily global objects
    this->init_global_operand(call->argument(0));
    this->init_global_operand(call->argument(1));

    const ScalarLit& s1 = this->_lit_factory.get_scalar(call->argument(0));
    const ScalarLit& s2 = this->_lit_factory.get_scalar(call->argument(1));

    if (!this->prepare_mem_access(s1) || !this->prepare_mem_access(s2)) {
      return;
    }

    // Do not keep track of the content
    this->_inv.normal().forget_reachable_mem(s1.var());

    if (call->has_result()) {
      this->assign(this->_lit_factory.get_scalar(call->result()), s1);
    }
  }

  /// \brief Execute a libc strcat call
  ///
  /// #include <string.h>
  /// char* strncat(char* s1, const char* s2, size_t n);
  ///
  /// The strncat() function appends a copy of the null-terminated string s2 to
  /// the end of the null-terminated string s1, then add a terminating `\0'. The
  /// string s1 must have sufficient space to hold the result.
  ///
  /// The strncat() function appends not more than n characters from s2, and
  /// then adds a terminating `\0'.
  ///
  /// The strncat() function returns the pointer s1.
  void exec_strncat(ar::CallBase* call) { this->exec_strcat(call); }

  /// \brief Execute a __ikos_nondet_X function call
  void exec_ikos_non_det(ar::CallBase* call) {
    // No side effects

    if (call->has_result()) {
      const ScalarLit& ret = this->_lit_factory.get_scalar(call->result());
      ikos_assert_msg(ret.is_machine_int_var(),
                      "left hand side is not an integer variable");

      this->_inv.normal().integers().forget(ret.var());
      this->_inv.normal().uninitialized().assign_initialized(ret.var());
    }
  }

  /// \brief Execute a ikos.counter.init function call
  void exec_ikos_counter_init(ar::CallBase* call) {
    ikos_assert(call->has_result());
    ikos_assert(call->num_arguments() == 1);

    const ScalarLit& ret = this->_lit_factory.get_scalar(call->result());
    const ScalarLit& init = this->_lit_factory.get_scalar(call->argument(0));

    ikos_assert_msg(ret.is_machine_int_var(),
                    "left hand side is not an integer variable");
    ikos_assert_msg(init.is_machine_int(), "operand is not a machine integer");

    this->_inv.normal().integers().init_counter(ret.var(), init.machine_int());
    this->_inv.normal().uninitialized().assign_initialized(ret.var());
  }

  /// \brief Execute a ikos.counter.incr function call
  void exec_ikos_counter_incr(ar::CallBase* call) {
    ikos_assert(call->has_result());
    ikos_assert(call->num_arguments() == 2);
    ikos_assert(call->result() == call->argument(0));

    const ScalarLit& ret = this->_lit_factory.get_scalar(call->result());
    const ScalarLit& incr = this->_lit_factory.get_scalar(call->argument(1));

    ikos_assert_msg(ret.is_machine_int_var(),
                    "left hand side is not an integer variable");
    ikos_assert_msg(incr.is_machine_int(), "operand is not a machine integer");

    this->_inv.normal().integers().incr_counter(ret.var(), incr.machine_int());
    this->_inv.normal().uninitialized().assign_initialized(ret.var());
  }

public:
  void match_down(ar::CallBase* call, ar::Function* called) override {
    ikos_assert(called->is_definition());
    ikos_assert(ar::TypeVerifier::is_valid_call(call, called->type()));

    auto fit = called->param_begin(), fet = called->param_end();
    auto ait = call->arg_begin(), aet = call->arg_end();
    for (; fit != fet && ait != aet; ++fit, ++ait) {
      this->init_global_operand(*ait);
      this->assign(this->_lit_factory.get(*fit), this->_lit_factory.get(*ait));
    }

    // TODO(marthaud): support va_arg
  }

  void match_up(ar::CallBase* call, ar::ReturnValue* ret) override {
    if (ret != nullptr && ret->has_operand() && call->has_result()) {
      this->init_global_operand(ret->operand());
      this->assign(this->_lit_factory.get(call->result()),
                   this->_lit_factory.get(ret->operand()));
    }

    if (ret != nullptr && ret->has_operand()) {
      // Clean-up the invariant
      const Literal& val = this->_lit_factory.get(ret->operand());

      if (val.is_scalar()) {
        if (val.scalar().is_var()) {
          this->_inv.normal().forget_surface(val.scalar().var());
        }
      } else if (val.is_aggregate()) {
        if (val.aggregate().is_var()) {
          this->_inv.normal().forget_reachable_mem(val.aggregate().var());
          this->_inv.normal().forget_surface(val.aggregate().var());
        }
      } else {
        ikos_unreachable("unreachable");
      }
    }

    // TODO(marthaud): support va_arg
  }

}; // end class NumericalExecutionEngine

} // end namespace analyzer
} // end namespace ikos
