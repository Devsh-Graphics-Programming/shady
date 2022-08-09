#ifndef SHADY_TYPE_H
#define SHADY_TYPE_H

#include "shady/ir.h"

struct TypeTable;
struct TypeTable* new_type_table();
void destroy_type_table(struct TypeTable*);

const Type* noret_type(IrArena* arena);

bool is_subtype(const Type* supertype, const Type* type);
void check_subtype(const Type* supertype, const Type* type);

#define DEFINE_NODE_CHECK_FN_1_1(struct_name, short_name) const Type* check_type_##short_name(IrArena*, struct_name);
#define DEFINE_NODE_CHECK_FN_1_0(struct_name, short_name) const Type* check_type_##short_name(IrArena*);

#define DEFINE_NODE_CHECK_FN_1(struct_name, short_name, has_payload) DEFINE_NODE_CHECK_FN_1_##has_payload(struct_name, short_name)
#define DEFINE_NODE_CHECK_FN_0(struct_name, short_name, _)
#define NODEDEF(_, has_typing_fn, has_payload, struct_name, short_name) DEFINE_NODE_CHECK_FN_##has_typing_fn(struct_name, short_name, has_payload)
NODES()
#undef NODEDEF

const Type* wrap_multiple_yield_types(IrArena* arena, Nodes types);
Nodes unwrap_multiple_yield_types(IrArena* arena, const Type* type);

// TODO: revise naming scheme
//const Type* strip_qualifier(const Type* type, DivergenceQualifier* qual_out);
//const Type* without_qualifier(const Type* type);
//DivergenceQualifier get_qualifier(const Type* type);

const Type* derive_fn_type(IrArena* arena, const Function* fn);
Nodes extract_variable_types(IrArena*, const Nodes*);
Nodes extract_types(IrArena*, Nodes);

/// Ensures an operand has divergence-annotated type and extracts it
const Type* extract_operand_type(IrArena*, const Type*);
bool is_operand_uniform(IrArena*, const Type*);
void deconstruct_operand_type(IrArena*, const Type*, const Type**, bool* uniformity);

bool contains_qualified_type(const Type* type);

#endif
