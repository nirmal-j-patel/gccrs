// Copyright (C) 2020 Free Software Foundation, Inc.

// This file is part of GCC.

// GCC is free software; you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free
// Software Foundation; either version 3, or (at your option) any later
// version.

// GCC is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
// for more details.

// You should have received a copy of the GNU General Public License
// along with GCC; see the file COPYING3.  If not see
// <http://www.gnu.org/licenses/>.

#ifndef RUST_HIR_PATH_PROBE_H
#define RUST_HIR_PATH_PROBE_H

#include "rust-hir-type-check-base.h"
#include "rust-hir-full.h"
#include "rust-tyty.h"
#include "rust-substitution-mapper.h"
#include "rust-hir-type-bounds.h"

namespace Rust {
namespace Resolver {

struct PathProbeCandidate
{
  enum CandidateType
  {
    IMPL_CONST,
    IMPL_TYPE_ALIAS,
    IMPL_FUNC,

    TRAIT_ITEM_CONST,
    TRAIT_TYPE_ALIAS,
    TRAIT_FUNC,
  };

  struct ImplItemCandidate
  {
    HIR::ImplItem *impl_item;
    HIR::ImplBlock *parent;
  };

  struct TraitItemCandidate
  {
    const TraitReference *trait_ref;
    const TraitItemReference *item_ref;
    HIR::ImplBlock *impl;
  };

  CandidateType type;
  TyTy::BaseType *ty;
  Location locus;
  union Candidate
  {
    ImplItemCandidate impl;
    TraitItemCandidate trait;

    Candidate (ImplItemCandidate impl) : impl (impl) {}
    Candidate (TraitItemCandidate trait) : trait (trait) {}
  } item;

  PathProbeCandidate (CandidateType type, TyTy::BaseType *ty, Location locus,
		      ImplItemCandidate impl)
    : type (type), ty (ty), item (impl)
  {}

  PathProbeCandidate (CandidateType type, TyTy::BaseType *ty, Location locus,
		      TraitItemCandidate trait)
    : type (type), ty (ty), item (trait)
  {}

  std::string as_string () const
  {
    return "PathProbe candidate TODO - as_string";
  }

  bool is_impl_candidate () const
  {
    switch (type)
      {
      case IMPL_CONST:
      case IMPL_TYPE_ALIAS:
      case IMPL_FUNC:
	return true;

      default:
	return false;
      }
    gcc_unreachable ();
  }

  bool is_trait_candidate () const
  {
    switch (type)
      {
      case TRAIT_ITEM_CONST:
      case TRAIT_TYPE_ALIAS:
      case TRAIT_FUNC:
	return true;

      default:
	return false;
      }
    gcc_unreachable ();
  }
};

class PathProbeType : public TypeCheckBase
{
protected:
  using Rust::Resolver::TypeCheckBase::visit;

public:
  static std::vector<PathProbeCandidate>
  Probe (const TyTy::BaseType *receiver,
	 const HIR::PathIdentSegment &segment_name, bool probe_impls,
	 bool probe_bounds, bool ignore_mandatory_trait_items)
  {
    PathProbeType probe (receiver, segment_name);
    if (probe_impls)
      probe.process_impl_items_for_candidates ();

    if (!probe_bounds)
      return probe.candidates;

    std::vector<std::pair<TraitReference *, HIR::ImplBlock *>> probed_bounds
      = TypeBoundsProbe::Probe (receiver);

    std::vector<std::pair<const TraitReference *, HIR::ImplBlock *>>
      specified_bounds;
    for (const TyTy::TypeBoundPredicate &predicate :
	 receiver->get_specified_bounds ())
      {
	const TraitReference *trait_item = predicate.get ();

	// FIXME lookup impl_block for this trait impl for this receiver
	specified_bounds.push_back ({trait_item, nullptr});
      }

    std::vector<std::pair<const TraitReference *, HIR::ImplBlock *>>
      union_type_bounds = probe.union_bounds (probed_bounds, specified_bounds);
    probe.process_traits_for_candidates (union_type_bounds,
					 ignore_mandatory_trait_items);
    return probe.candidates;
  }

  void visit (HIR::TypeAlias &alias) override
  {
    Identifier name = alias.get_new_type_name ();
    if (search.as_string ().compare (name) == 0)
      {
	HirId tyid = alias.get_mappings ().get_hirid ();
	TyTy::BaseType *ty = nullptr;
	bool ok = context->lookup_type (tyid, &ty);
	rust_assert (ok);

	PathProbeCandidate::ImplItemCandidate impl_item_candidate{&alias,
								  current_impl};
	PathProbeCandidate candidate{
	  PathProbeCandidate::CandidateType::IMPL_TYPE_ALIAS, ty,
	  alias.get_locus (), impl_item_candidate};
	candidates.push_back (std::move (candidate));
      }
  }

  void visit (HIR::ConstantItem &constant) override
  {
    Identifier name = constant.get_identifier ();
    if (search.as_string ().compare (name) == 0)
      {
	HirId tyid = constant.get_mappings ().get_hirid ();
	TyTy::BaseType *ty = nullptr;
	bool ok = context->lookup_type (tyid, &ty);
	rust_assert (ok);

	PathProbeCandidate::ImplItemCandidate impl_item_candidate{&constant,
								  current_impl};
	PathProbeCandidate candidate{
	  PathProbeCandidate::CandidateType::IMPL_CONST, ty,
	  constant.get_locus (), impl_item_candidate};
	candidates.push_back (std::move (candidate));
      }
  }

  void visit (HIR::Function &function) override
  {
    Identifier name = function.get_function_name ();
    if (search.as_string ().compare (name) == 0)
      {
	HirId tyid = function.get_mappings ().get_hirid ();
	TyTy::BaseType *ty = nullptr;
	bool ok = context->lookup_type (tyid, &ty);
	rust_assert (ok);

	PathProbeCandidate::ImplItemCandidate impl_item_candidate{&function,
								  current_impl};
	PathProbeCandidate candidate{
	  PathProbeCandidate::CandidateType::IMPL_FUNC, ty,
	  function.get_locus (), impl_item_candidate};
	candidates.push_back (std::move (candidate));
      }
  }

protected:
  void process_impl_items_for_candidates ()
  {
    mappings->iterate_impl_items ([&] (HirId id, HIR::ImplItem *item,
				       HIR::ImplBlock *impl) mutable -> bool {
      process_impl_item_candidate (id, item, impl);
      return true;
    });
  }

  void process_impl_item_candidate (HirId id, HIR::ImplItem *item,
				    HIR::ImplBlock *impl)
  {
    current_impl = impl;
    HirId impl_ty_id = impl->get_type ()->get_mappings ().get_hirid ();
    TyTy::BaseType *impl_block_ty = nullptr;
    bool ok = context->lookup_type (impl_ty_id, &impl_block_ty);
    rust_assert (ok);

    if (!receiver->can_eq (impl_block_ty, false))
      return;

    // lets visit the impl_item
    item->accept_vis (*this);
  }

  void process_traits_for_candidates (
    const std::vector<std::pair<const TraitReference *, HIR::ImplBlock *>>
      traits,
    bool ignore_mandatory_trait_items)
  {
    for (auto &ref : traits)
      {
	const TraitReference *trait_ref = ref.first;
	HIR::ImplBlock *impl = ref.second;

	const TraitItemReference *trait_item_ref = nullptr;
	if (!trait_ref->lookup_trait_item (search.as_string (),
					   &trait_item_ref))
	  continue;

	bool trait_item_needs_implementation = !trait_item_ref->is_optional ();
	if (ignore_mandatory_trait_items && trait_item_needs_implementation)
	  continue;

	PathProbeCandidate::CandidateType candidate_type;
	switch (trait_item_ref->get_trait_item_type ())
	  {
	  case TraitItemReference::TraitItemType::FN:
	    candidate_type = PathProbeCandidate::CandidateType::TRAIT_FUNC;
	    break;
	  case TraitItemReference::TraitItemType::CONST:
	    candidate_type
	      = PathProbeCandidate::CandidateType::TRAIT_ITEM_CONST;
	    break;
	  case TraitItemReference::TraitItemType::TYPE:
	    candidate_type
	      = PathProbeCandidate::CandidateType::TRAIT_TYPE_ALIAS;
	    break;

	  case TraitItemReference::TraitItemType::ERROR:
	  default:
	    gcc_unreachable ();
	    break;
	  }

	TyTy::BaseType *trait_item_tyty = trait_item_ref->get_tyty ();

	// we cannot auto setup associated type mappings when our receiver is a
	// generic type bound
	const TyTy::BaseType *root = receiver->get_root ();
	bool receiver_is_type_param
	  = root->get_kind () == TyTy::TypeKind::PARAM;
	bool receiver_is_dyn = root->get_kind () == TyTy::TypeKind::DYNAMIC;
	bool receiver_is_generic = receiver_is_type_param || receiver_is_dyn;

	if (impl != nullptr && !receiver_is_generic)

	  {
	    HirId impl_block_id = impl->get_mappings ().get_hirid ();
	    AssociatedImplTrait *lookup_associated = nullptr;
	    bool found_impl_trait
	      = context->lookup_associated_trait_impl (impl_block_id,
						       &lookup_associated);
	    // see testsuite/rust/compile/torture/traits10.rs this can be false
	    if (found_impl_trait)
	      lookup_associated->setup_associated_types ();
	  }

	// we can substitute the Self with the receiver here
	if (trait_item_tyty->get_kind () == TyTy::TypeKind::FNDEF)
	  {
	    TyTy::FnType *fn = static_cast<TyTy::FnType *> (trait_item_tyty);
	    TyTy::SubstitutionParamMapping *param = nullptr;
	    for (auto &param_mapping : fn->get_substs ())
	      {
		const HIR::TypeParam &type_param
		  = param_mapping.get_generic_param ();
		if (type_param.get_type_representation ().compare ("Self") == 0)
		  {
		    param = &param_mapping;
		    break;
		  }
	      }
	    rust_assert (param != nullptr);

	    std::vector<TyTy::SubstitutionArg> mappings;
	    mappings.push_back (
	      TyTy::SubstitutionArg (param, receiver->clone ()));

	    Location locus; // FIXME
	    TyTy::SubstitutionArgumentMappings args (std::move (mappings),
						     locus);
	    trait_item_tyty
	      = SubstMapperInternal::Resolve (trait_item_tyty, args);
	  }

	PathProbeCandidate::TraitItemCandidate trait_item_candidate{
	  trait_ref, trait_item_ref, impl};
	PathProbeCandidate candidate{candidate_type,
				     trait_item_tyty,
				     trait_ref->get_locus (),
				     {trait_item_candidate}};
	candidates.push_back (std::move (candidate));
      }
  }

protected:
  PathProbeType (const TyTy::BaseType *receiver,
		 const HIR::PathIdentSegment &query)
    : TypeCheckBase (), receiver (receiver), search (query),
      current_impl (nullptr)
  {}

  std::vector<std::pair<const TraitReference *, HIR::ImplBlock *>>
  union_bounds (
    const std::vector<std::pair</*const*/ TraitReference *, HIR::ImplBlock *>>
      a,
    const std::vector<std::pair<const TraitReference *, HIR::ImplBlock *>> b)
    const
  {
    std::map<DefId, std::pair<const TraitReference *, HIR::ImplBlock *>> mapper;
    for (auto &ref : a)
      {
	mapper.insert ({ref.first->get_mappings ().get_defid (), ref});
      }
    for (auto &ref : b)
      {
	mapper.insert ({ref.first->get_mappings ().get_defid (), ref});
      }

    std::vector<std::pair<const TraitReference *, HIR::ImplBlock *>> union_set;
    for (auto it = mapper.begin (); it != mapper.end (); it++)
      {
	union_set.push_back ({it->second.first, it->second.second});
      }
    return union_set;
  }

  const TyTy::BaseType *receiver;
  const HIR::PathIdentSegment &search;
  std::vector<PathProbeCandidate> candidates;
  HIR::ImplBlock *current_impl;
};

class ReportMultipleCandidateError : private TypeCheckBase
{
  using Rust::Resolver::TypeCheckBase::visit;

public:
  static void Report (std::vector<PathProbeCandidate> &candidates,
		      const HIR::PathIdentSegment &query, Location query_locus)
  {
    RichLocation r (query_locus);
    ReportMultipleCandidateError visitor (r);
    for (auto &c : candidates)
      {
	switch (c.type)
	  {
	  case PathProbeCandidate::CandidateType::IMPL_CONST:
	  case PathProbeCandidate::CandidateType::IMPL_TYPE_ALIAS:
	  case PathProbeCandidate::CandidateType::IMPL_FUNC:
	    c.item.impl.impl_item->accept_vis (visitor);
	    break;

	  case PathProbeCandidate::CandidateType::TRAIT_ITEM_CONST:
	  case PathProbeCandidate::CandidateType::TRAIT_TYPE_ALIAS:
	  case PathProbeCandidate::CandidateType::TRAIT_FUNC:
	    r.add_range (c.item.trait.item_ref->get_locus ());
	    break;
	  }
      }

    rust_error_at (r, "multiple applicable items in scope for: %s",
		   query.as_string ().c_str ());
  }

  void visit (HIR::TypeAlias &alias) override
  {
    r.add_range (alias.get_locus ());
  }

  void visit (HIR::ConstantItem &constant) override
  {
    r.add_range (constant.get_locus ());
  }

  void visit (HIR::Function &function) override
  {
    r.add_range (function.get_locus ());
  }

private:
  ReportMultipleCandidateError (RichLocation &r) : TypeCheckBase (), r (r) {}

  RichLocation &r;
};

class PathProbeImplTrait : public PathProbeType
{
public:
  static std::vector<PathProbeCandidate>
  Probe (const TyTy::BaseType *receiver,
	 const HIR::PathIdentSegment &segment_name,
	 const TraitReference *trait_reference)
  {
    PathProbeImplTrait probe (receiver, segment_name, trait_reference);
    // iterate all impls for this trait and receiver
    // then search for possible candidates using base class behaviours
    probe.process_trait_impl_items_for_candidates ();
    return probe.candidates;
  }

private:
  void process_trait_impl_items_for_candidates ();

  PathProbeImplTrait (const TyTy::BaseType *receiver,
		      const HIR::PathIdentSegment &query,
		      const TraitReference *trait_reference)
    : PathProbeType (receiver, query), trait_reference (trait_reference)
  {}

  const TraitReference *trait_reference;
};

} // namespace Resolver
} // namespace Rust

#endif // RUST_HIR_PATH_PROBE_H
