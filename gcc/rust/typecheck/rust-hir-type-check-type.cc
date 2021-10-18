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

#include "rust-hir-type-check-type.h"
#include "rust-hir-trait-resolve.h"

namespace Rust {
namespace Resolver {

void
TypeCheckType::visit (HIR::TypePath &path)
{
  // lookup the Node this resolves to
  NodeId ref;
  auto nid = path.get_mappings ().get_nodeid ();
  bool is_fully_resolved = resolver->lookup_resolved_type (nid, &ref);

  TyTy::BaseType *lookup = nullptr;
  if (!is_fully_resolved)
    {
      // this can happen so we need to look up the root then resolve the
      // remaining segments if possible
      size_t offset = 0;
      NodeId resolved_node_id = UNKNOWN_NODEID;
      TyTy::BaseType *root
	= resolve_root_path (path, &offset, &resolved_node_id);

      rust_assert (root != nullptr);
      if (root->get_kind () == TyTy::TypeKind::ERROR)
	return;

      translated
	= resolve_segments (resolved_node_id, path.get_mappings ().get_hirid (),
			    path.get_segments (), offset, root,
			    path.get_mappings (), path.get_locus ());
      return;
    }

  HirId hir_lookup;
  if (!context->lookup_type_by_node_id (ref, &hir_lookup))
    {
      rust_error_at (path.get_locus (), "failed to lookup HIR %d for node '%s'",
		     ref, path.as_string ().c_str ());
      return;
    }

  if (!context->lookup_type (hir_lookup, &lookup))
    {
      rust_error_at (path.get_locus (), "failed to lookup HIR TyTy");
      return;
    }

  TyTy::BaseType *path_type = lookup->clone ();
  path_type->set_ref (path.get_mappings ().get_hirid ());

  HIR::TypePathSegment *final_seg = path.get_final_segment ().get ();
  HIR::GenericArgs args = TypeCheckResolveGenericArguments::resolve (final_seg);

  bool is_big_self = final_seg->is_ident_only ()
		     && (final_seg->as_string ().compare ("Self") == 0);

  if (path_type->needs_generic_substitutions ())
    {
      if (is_big_self)
	{
	  translated = path_type;
	  return;
	}

      translated = SubstMapper::Resolve (path_type, path.get_locus (), &args);
      if (translated->get_kind () != TyTy::TypeKind::ERROR
	  && mappings != nullptr)
	{
	  check_for_unconstrained (args.get_type_args ());
	}
    }
  else if (!args.is_empty ())
    {
      rust_error_at (path.get_locus (),
		     "TypePath %s declares generic arguments but "
		     "the type %s does not have any",
		     path.as_string ().c_str (),
		     translated->as_string ().c_str ());
    }
  else
    {
      translated = path_type;
    }
}

void
TypeCheckType::visit (HIR::QualifiedPathInType &path)
{
  HIR::QualifiedPathType qual_path_type = path.get_path_type ();
  TyTy::BaseType *root
    = TypeCheckType::Resolve (qual_path_type.get_type ().get ());
  if (root->get_kind () == TyTy::TypeKind::ERROR)
    {
      rust_debug_loc (path.get_locus (), "failed to resolve the root");
      return;
    }

  if (!qual_path_type.has_as_clause ())
    {
      // then this is just a normal path-in-expression
      NodeId root_resolved_node_id = UNKNOWN_NODEID;
      bool ok = resolver->lookup_resolved_type (
	qual_path_type.get_type ()->get_mappings ().get_nodeid (),
	&root_resolved_node_id);
      rust_assert (ok);

      translated = resolve_segments (root_resolved_node_id,
				     path.get_mappings ().get_hirid (),
				     path.get_segments (), 0, translated,
				     path.get_mappings (), path.get_locus ());

      return;
    }

  // Resolve the trait now
  TraitReference *trait_ref
    = TraitResolver::Resolve (*qual_path_type.get_trait ().get ());
  if (trait_ref->is_error ())
    return;

  // does this type actually implement this type-bound?
  if (!TypeBoundsProbe::is_bound_satisfied_for_type (root, trait_ref))
    return;

  // we need resolve to the impl block
  NodeId impl_resolved_id = UNKNOWN_NODEID;
  bool ok = resolver->lookup_resolved_name (
    qual_path_type.get_mappings ().get_nodeid (), &impl_resolved_id);
  rust_assert (ok);

  HirId impl_block_id;
  ok = mappings->lookup_node_to_hir (path.get_mappings ().get_crate_num (),
				     impl_resolved_id, &impl_block_id);
  rust_assert (ok);

  AssociatedImplTrait *lookup_associated = nullptr;
  bool found_impl_trait
    = context->lookup_associated_trait_impl (impl_block_id, &lookup_associated);
  rust_assert (found_impl_trait);

  std::unique_ptr<HIR::TypePathSegment> &item_seg
    = path.get_associated_segment ();

  const TraitItemReference *trait_item_ref = nullptr;
  ok
    = trait_ref->lookup_trait_item (item_seg->get_ident_segment ().as_string (),
				    &trait_item_ref);
  if (!ok)
    {
      rust_error_at (item_seg->get_locus (), "unknown associated item");
      return;
    }

  // project
  lookup_associated->setup_associated_types ();

  HIR::GenericArgs trait_generics = qual_path_type.trait_has_generic_args ()
				      ? qual_path_type.get_trait_generic_args ()
				      : HIR::GenericArgs::create_empty ();

  translated = lookup_associated->get_projected_type (
    trait_item_ref, root, item_seg->get_mappings ().get_hirid (),
    trait_generics, item_seg->get_locus ());

  if (translated->get_kind () == TyTy::TypeKind::PLACEHOLDER)
    {
      // lets grab the actual projection type
      TyTy::PlaceholderType *p
	= static_cast<TyTy::PlaceholderType *> (translated);
      if (p->can_resolve ())
	{
	  translated = p->resolve ();
	}
    }

  if (item_seg->get_type () == HIR::TypePathSegment::SegmentType::GENERIC)
    {
      HIR::TypePathSegmentGeneric &generic_seg
	= static_cast<HIR::TypePathSegmentGeneric &> (*item_seg.get ());

      // turbo-fish segment path::<ty>
      if (generic_seg.has_generic_args ())
	{
	  if (!translated->can_substitute ())
	    {
	      rust_error_at (item_seg->get_locus (),
			     "substitutions not supported for %s",
			     translated->as_string ().c_str ());
	      translated
		= new TyTy::ErrorType (path.get_mappings ().get_hirid ());
	      return;
	    }
	  translated = SubstMapper::Resolve (translated, path.get_locus (),
					     &generic_seg.get_generic_args ());
	}
    }

  // continue on as a path-in-expression
  NodeId root_resolved_node_id = trait_item_ref->get_mappings ().get_nodeid ();
  bool fully_resolved = path.get_segments ().empty ();
  if (fully_resolved)
    {
      resolver->insert_resolved_name (path.get_mappings ().get_nodeid (),
				      root_resolved_node_id);
      context->insert_receiver (path.get_mappings ().get_hirid (), root);
      return;
    }

  translated
    = resolve_segments (root_resolved_node_id,
			path.get_mappings ().get_hirid (), path.get_segments (),
			0, translated, path.get_mappings (), path.get_locus ());
}

TyTy::BaseType *
TypeCheckType::resolve_root_path (HIR::TypePath &path, size_t *offset,
				  NodeId *root_resolved_node_id)
{
  TyTy::BaseType *root_tyty = nullptr;
  *offset = 0;
  for (size_t i = 0; i < path.get_num_segments (); i++)
    {
      std::unique_ptr<HIR::TypePathSegment> &seg = path.get_segments ().at (i);

      bool have_more_segments = (path.get_num_segments () - 1 != i);
      bool is_root = *offset == 0;
      NodeId ast_node_id = seg->get_mappings ().get_nodeid ();

      // then lookup the reference_node_id
      NodeId ref_node_id = UNKNOWN_NODEID;
      if (resolver->lookup_resolved_name (ast_node_id, &ref_node_id))
	{
	  // these ref_node_ids will resolve to a pattern declaration but we
	  // are interested in the definition that this refers to get the
	  // parent id
	  Definition def;
	  if (!resolver->lookup_definition (ref_node_id, &def))
	    {
	      rust_error_at (path.get_locus (),
			     "unknown reference for resolved name");
	      return new TyTy::ErrorType (path.get_mappings ().get_hirid ());
	    }
	  ref_node_id = def.parent;
	}
      else
	{
	  resolver->lookup_resolved_type (ast_node_id, &ref_node_id);
	}

      // ref_node_id is the NodeId that the segments refers to.
      if (ref_node_id == UNKNOWN_NODEID)
	{
	  if (is_root)
	    {
	      rust_error_at (seg->get_locus (),
			     "failed to type resolve root segment");
	      return new TyTy::ErrorType (path.get_mappings ().get_hirid ());
	    }
	  return root_tyty;
	}

      // node back to HIR
      HirId ref;
      if (!mappings->lookup_node_to_hir (path.get_mappings ().get_crate_num (),
					 ref_node_id, &ref))
	{
	  if (is_root)
	    {
	      rust_error_at (seg->get_locus (), "789 reverse lookup failure");
	      rust_debug_loc (
		seg->get_locus (),
		"failure with [%s] mappings [%s] ref_node_id [%u]",
		seg->as_string ().c_str (),
		seg->get_mappings ().as_string ().c_str (), ref_node_id);

	      return new TyTy::ErrorType (path.get_mappings ().get_hirid ());
	    }

	  return root_tyty;
	}

      auto seg_is_module
	= (nullptr
	   != mappings->lookup_module (path.get_mappings ().get_crate_num (),
				       ref));

      if (seg_is_module)
	{
	  // A::B::C::this_is_a_module::D::E::F
	  //          ^^^^^^^^^^^^^^^^
	  //          Currently handling this.
	  if (have_more_segments)
	    {
	      (*offset)++;
	      continue;
	    }

	  // In the case of :
	  // A::B::C::this_is_a_module
	  //          ^^^^^^^^^^^^^^^^
	  // This is an error, we are not expecting a module.
	  rust_error_at (seg->get_locus (), "expected value");
	  return new TyTy::ErrorType (path.get_mappings ().get_hirid ());
	}

      TyTy::BaseType *lookup = nullptr;
      if (!context->lookup_type (ref, &lookup))
	{
	  if (is_root)
	    {
	      rust_error_at (seg->get_locus (),
			     "failed to resolve root segment");
	      return new TyTy::ErrorType (path.get_mappings ().get_hirid ());
	    }
	  return root_tyty;
	}

      // if we have a previous segment type
      if (root_tyty != nullptr)
	{
	  // if this next segment needs substitution we must apply the
	  // previous type arguments
	  //
	  // such as: GenericStruct::<_>::new(123, 456)
	  if (lookup->needs_generic_substitutions ())
	    {
	      if (!root_tyty->needs_generic_substitutions ())
		{
		  auto used_args_in_prev_segment
		    = GetUsedSubstArgs::From (root_tyty);
		  lookup
		    = SubstMapperInternal::Resolve (lookup,
						    used_args_in_prev_segment);
		}
	    }
	}

      // turbo-fish segment path::<ty>
      if (seg->is_generic_segment ())
	{
	  HIR::TypePathSegmentGeneric *generic_segment
	    = static_cast<HIR::TypePathSegmentGeneric *> (seg.get ());

	  if (!lookup->can_substitute ())
	    {
	      rust_error_at (seg->get_locus (),
			     "substitutions not supported for %s",
			     lookup->as_string ().c_str ());
	      return new TyTy::ErrorType (lookup->get_ref ());
	    }
	  lookup = SubstMapper::Resolve (lookup, path.get_locus (),
					 &generic_segment->get_generic_args ());
	}

      *root_resolved_node_id = ref_node_id;
      *offset = *offset + 1;
      root_tyty = lookup;
    }

  return root_tyty;
}

TyTy::BaseType *
TypeCheckType::resolve_segments (
  NodeId root_resolved_node_id, HirId expr_id,
  std::vector<std::unique_ptr<HIR::TypePathSegment>> &segments, size_t offset,
  TyTy::BaseType *tyseg, const Analysis::NodeMapping &expr_mappings,
  Location expr_locus)
{
  NodeId resolved_node_id = root_resolved_node_id;
  TyTy::BaseType *prev_segment = tyseg;
  for (size_t i = offset; i < segments.size (); i++)
    {
      std::unique_ptr<HIR::TypePathSegment> &seg = segments.at (i);

      bool reciever_is_generic
	= prev_segment->get_kind () == TyTy::TypeKind::PARAM;
      bool probe_bounds = true;
      bool probe_impls = !reciever_is_generic;
      bool ignore_mandatory_trait_items = !reciever_is_generic;

      // probe the path is done in two parts one where we search impls if no
      // candidate is found then we search extensions from traits
      auto candidates
	= PathProbeType::Probe (prev_segment, seg->get_ident_segment (),
				probe_impls, false,
				ignore_mandatory_trait_items);
      if (candidates.size () == 0)
	{
	  candidates
	    = PathProbeType::Probe (prev_segment, seg->get_ident_segment (),
				    false, probe_bounds,
				    ignore_mandatory_trait_items);

	  if (candidates.size () == 0)
	    {
	      rust_error_at (
		seg->get_locus (),
		"failed to resolve path segment using an impl Probe");
	      return new TyTy::ErrorType (expr_id);
	    }
	}

      if (candidates.size () > 1)
	{
	  ReportMultipleCandidateError::Report (candidates,
						seg->get_ident_segment (),
						seg->get_locus ());
	  return new TyTy::ErrorType (expr_id);
	}

      auto &candidate = candidates.at (0);
      prev_segment = tyseg;
      tyseg = candidate.ty;

      if (candidate.is_impl_candidate ())
	{
	  resolved_node_id
	    = candidate.item.impl.impl_item->get_impl_mappings ().get_nodeid ();
	}
      else
	{
	  resolved_node_id
	    = candidate.item.trait.item_ref->get_mappings ().get_nodeid ();

	  // lookup the associated-impl-trait
	  HIR::ImplBlock *impl = candidate.item.trait.impl;
	  if (impl != nullptr)
	    {
	      AssociatedImplTrait *lookup_associated = nullptr;
	      bool found_impl_trait = context->lookup_associated_trait_impl (
		impl->get_mappings ().get_hirid (), &lookup_associated);
	      rust_assert (found_impl_trait);

	      lookup_associated->setup_associated_types ();

	      // we need a new ty_ref_id for this trait item
	      tyseg = tyseg->clone ();
	      tyseg->set_ty_ref (mappings->get_next_hir_id ());
	    }
	}

      if (seg->is_generic_segment ())
	{
	  HIR::TypePathSegmentGeneric *generic_segment
	    = static_cast<HIR::TypePathSegmentGeneric *> (seg.get ());

	  if (!tyseg->can_substitute ())
	    {
	      rust_error_at (expr_locus, "substitutions not supported for %s",
			     tyseg->as_string ().c_str ());
	      return new TyTy::ErrorType (expr_id);
	    }

	  tyseg = SubstMapper::Resolve (tyseg, expr_locus,
					&generic_segment->get_generic_args ());
	  if (tyseg->get_kind () == TyTy::TypeKind::ERROR)
	    return new TyTy::ErrorType (expr_id);
	}
    }

  context->insert_receiver (expr_mappings.get_hirid (), prev_segment);
  if (tyseg->needs_generic_substitutions ())
    {
      Location locus = segments.back ()->get_locus ();
      if (!prev_segment->needs_generic_substitutions ())
	{
	  auto used_args_in_prev_segment
	    = GetUsedSubstArgs::From (prev_segment);
	  if (!used_args_in_prev_segment.is_error ())
	    tyseg
	      = SubstMapperInternal::Resolve (tyseg, used_args_in_prev_segment);
	}
      else
	{
	  tyseg = SubstMapper::InferSubst (tyseg, locus);
	}

      if (tyseg->get_kind () == TyTy::TypeKind::ERROR)
	return new TyTy::ErrorType (expr_id);
    }

  rust_assert (resolved_node_id != UNKNOWN_NODEID);

  // lookup if the name resolver was able to canonically resolve this or not
  NodeId path_resolved_id = UNKNOWN_NODEID;
  if (resolver->lookup_resolved_name (expr_mappings.get_nodeid (),
				      &path_resolved_id))
    {
      rust_assert (path_resolved_id == resolved_node_id);
    }
  // check the type scope
  else if (resolver->lookup_resolved_type (expr_mappings.get_nodeid (),
					   &path_resolved_id))
    {
      rust_assert (path_resolved_id == resolved_node_id);
    }
  else
    {
      resolver->insert_resolved_name (expr_mappings.get_nodeid (),
				      resolved_node_id);
    }

  return tyseg;
}

void
TypeCheckType::visit (HIR::TraitObjectTypeOneBound &type)
{
  std::vector<TyTy::TypeBoundPredicate> specified_bounds;

  HIR::TraitBound &trait_bound = type.get_trait_bound ();
  TraitReference *trait = resolve_trait_path (trait_bound.get_path ());
  TyTy::TypeBoundPredicate predicate (trait->get_mappings ().get_defid (),
				      trait_bound.get_locus ());

  if (predicate.is_object_safe (true, type.get_locus ()))
    {
      specified_bounds.push_back (std::move (predicate));
      translated
	= new TyTy::DynamicObjectType (type.get_mappings ().get_hirid (),
				       std::move (specified_bounds));
    }
}

} // namespace Resolver
} // namespace Rust
