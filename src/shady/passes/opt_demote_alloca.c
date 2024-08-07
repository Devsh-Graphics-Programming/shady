#include "pass.h"

#include "../visit.h"
#include "../type.h"
#include "../ir_private.h"
#include "../transform/ir_gen_helpers.h"
#include "../analysis/uses.h"
#include "../analysis/leak.h"

#include "log.h"
#include "portability.h"
#include "list.h"
#include "dict.h"
#include "util.h"

#include <assert.h>

typedef struct Context_ {
    Rewriter rewriter;
    bool disable_lowering;

    const UsesMap* uses;
    const CompilerConfig* config;
    Arena* arena;
    struct Dict* alloca_info;
    bool todo;
} Context;

typedef struct {
    const Type* type;
    bool leaks;
    bool read_from;
    bool non_logical_use;
    const Node* new;
} AllocaInfo;

typedef struct {
    AllocaInfo* src_alloca;
} PtrSourceKnowledge;

static void visit_ptr_uses(const Node* ptr_value, const Type* slice_type, AllocaInfo* k, const UsesMap* map) {
    const Type* ptr_type = ptr_value->type;
    bool ptr_u = deconstruct_qualified_type(&ptr_type);
    assert(ptr_type->tag == PtrType_TAG);

    const Use* use = get_first_use(map, ptr_value);
    for (;use; use = use->next_use) {
        if (is_abstraction(use->user) && use->operand_class == NcParam)
            continue;
        else if (use->user->tag == Load_TAG) {
            //if (get_pointer_type_element(ptr_type) != slice_type)
            //    k->reinterpreted = true;
            k->read_from = true;
            continue; // loads don't leak the address.
        } else if (use->user->tag == Store_TAG) {
            //if (get_pointer_type_element(ptr_type) != slice_type)
            //    k->reinterpreted = true;
            // stores leak the value if it's stored
            if (ptr_value == use->user->payload.store.value)
                k->leaks = true;
            continue;
        } else if (use->user->tag == PrimOp_TAG) {
            PrimOp payload = use->user->payload.prim_op;
            switch (payload.op) {
                case reinterpret_op: {
                    k->non_logical_use = true;
                    continue;
                }
                case convert_op: {
                    if (first(payload.type_arguments)->tag == PtrType_TAG) {
                        k->non_logical_use = true;
                    } else {
                        k->leaks = true;
                    }
                    continue;
                }
                /*case reinterpret_op: {
                    debugvv_print("demote_alloca leak analysis: following reinterpret instr: ");
                    log_node(DEBUGVV, use->user);
                    debugvv_print(".\n");
                    visit_ptr_uses(use->user, slice_type, k, map);
                    continue;
                }
                case convert_op: {
                    if (first(payload.type_arguments)->tag == PtrType_TAG) {
                        // this is a ptr-ptr conversion, which means it's a Generic-non generic conversion
                        // these are fine, just track them
                        debugvv_print("demote_alloca leak analysis: following conversion instr: ");
                        log_node(DEBUGVV, use->user);
                        debugvv_print(".\n");
                        visit_ptr_uses(use->user, slice_type, k, map);
                        continue;
                    }
                    k->leaks = true;
                    continue;
                }*/
                default: break;
            }
            if (has_primop_got_side_effects(payload.op))
                k->leaks = true;
        } else if (use->user->tag == Lea_TAG) {
            // TODO: follow where those derived pointers are used and establish whether they leak themselves
            // use slice_type to keep track of the expected type for the relevant sub-object
            k->leaks = true;
            continue;
        } else if (use->user->tag == Composite_TAG) {
            // todo...
            // note: if a composite literal containing our POI (pointer-of-interest) is extracted from, folding ops simplify this to the original POI
            // so we don't need to be so clever here I think
            k->leaks = true;
        } else {
            k->leaks = true;
        }
    }
}

PtrSourceKnowledge get_ptr_source_knowledge(Context* ctx, const Node* ptr) {
    PtrSourceKnowledge k = { 0 };
    while (ptr) {
        assert(is_value(ptr));
        const Node* instr = ptr;
        switch (instr->tag) {
            case StackAlloc_TAG:
            case LocalAlloc_TAG: {
                k.src_alloca = *find_value_dict(const Node*, AllocaInfo*, ctx->alloca_info, instr);
                return k;
            }
            case PrimOp_TAG: {
                PrimOp payload = instr->payload.prim_op;
                switch (payload.op) {
                    case convert_op:
                    case reinterpret_op: {
                        ptr = first(payload.operands);
                        continue;
                    }
                    // TODO: lea and co
                    default:
                        break;
                }
            }
            default: break;
        }

        ptr = NULL;
    }
    return k;
}

static const Node* handle_alloc(Context* ctx, const Node* old, const Type* old_type) {
    IrArena* a = ctx->rewriter.dst_arena;
    Rewriter* r = &ctx->rewriter;

    AllocaInfo* k = arena_alloc(ctx->arena, sizeof(AllocaInfo));
    *k = (AllocaInfo) { .type = rewrite_node(r, old_type) };
    assert(ctx->uses);
    visit_ptr_uses(old, old_type, k, ctx->uses);
    insert_dict(const Node*, AllocaInfo*, ctx->alloca_info, old, k);
    debugv_print("demote_alloca: uses analysis results for ");
    log_node(DEBUGV, old);
    debugv_print(": leaks=%d read_from=%d non_logical_use=%d\n", k->leaks, k->read_from, k->non_logical_use);
    if (!k->leaks) {
        if (!k->read_from && !k->non_logical_use/* this should include killing dead stores! */) {
            ctx->todo |= true;
            const Node* new = undef(a, (Undef) {.type = get_unqualified_type(rewrite_node(r, old->type))});
            k->new = new;
            return new;
        }
        if (!k->non_logical_use && get_arena_config(a)->optimisations.weaken_non_leaking_allocas) {
            ctx->todo |= true;
            const Node* new =  local_alloc(a, (LocalAlloc) {rewrite_node(r, old_type )});
            k->new = new;
            return new;
        }
    }
    const Node* new = recreate_node_identity(r, old);
    k->new = new;
    return new;
}

static const Node* process(Context* ctx, const Node* old) {
    const Node* found = search_processed(&ctx->rewriter, old);
    if (found) return found;

    Rewriter* r = &ctx->rewriter;
    IrArena* a = r->dst_arena;

    switch (old->tag) {
        case Function_TAG: {
            Node* fun = recreate_decl_header_identity(&ctx->rewriter, old);
            Context fun_ctx = *ctx;
            fun_ctx.uses = create_uses_map(old, (NcDeclaration | NcType));
            fun_ctx.disable_lowering = lookup_annotation_with_string_payload(old, "DisableOpt", "demote_alloca");
            if (old->payload.fun.body)
                fun->payload.fun.body = rewrite_node(&fun_ctx.rewriter, old->payload.fun.body);
            destroy_uses_map(fun_ctx.uses);
            return fun;
        }
        case Constant_TAG: {
            Context fun_ctx = *ctx;
            fun_ctx.uses = NULL;
            return recreate_node_identity(&fun_ctx.rewriter, old);
        }
        case Load_TAG: {
            Load payload = old->payload.load;
            PtrSourceKnowledge k = get_ptr_source_knowledge(ctx, payload.ptr);
            if (k.src_alloca) {
                const Type* access_type = get_pointer_type_element(get_unqualified_type(rewrite_node(r, payload.ptr->type)));
                if (is_reinterpret_cast_legal(access_type, k.src_alloca->type)) {
                    if (k.src_alloca->new == rewrite_node(r, payload.ptr))
                        break;
                    ctx->todo |= true;
                    BodyBuilder* bb = begin_body_with_mem(a, rewrite_node(r, payload.mem));
                    const Node* data = gen_load(bb, k.src_alloca->new);
                    data = gen_reinterpret_cast(bb, access_type, data);
                    return yield_value_and_wrap_in_block(bb, data);
                }
            }
            break;
        }
        case Store_TAG: {
            Store payload = old->payload.store;
            PtrSourceKnowledge k = get_ptr_source_knowledge(ctx, payload.ptr);
            if (k.src_alloca) {
                const Type* access_type = get_pointer_type_element(get_unqualified_type(rewrite_node(r, payload.ptr->type)));
                if (is_reinterpret_cast_legal(access_type, k.src_alloca->type)) {
                    if (k.src_alloca->new == rewrite_node(r, payload.ptr))
                        break;
                    ctx->todo |= true;
                    BodyBuilder* bb = begin_body_with_mem(a, rewrite_node(r, payload.mem));
                    const Node* data = gen_reinterpret_cast(bb, access_type, rewrite_node(r, payload.value));
                    gen_store(bb, k.src_alloca->new, data);
                    return yield_values_and_wrap_in_block(bb, empty(a));
                }
            }
            break;
        }
        case PrimOp_TAG: {
            PrimOp payload = old->payload.prim_op;
            switch (payload.op) {

                default:
                    break;
            }
            break;
        }
        case LocalAlloc_TAG: return handle_alloc(ctx, old, old->payload.local_alloc.type);
        case StackAlloc_TAG: return handle_alloc(ctx, old, old->payload.stack_alloc.type);
        default: break;
    }
    return recreate_node_identity(&ctx->rewriter, old);
}

KeyHash hash_node(const Node**);
bool compare_node(const Node**, const Node**);

bool opt_demote_alloca(SHADY_UNUSED const CompilerConfig* config, Module** m) {
    Module* src = *m;
    IrArena* a = get_module_arena(src);
    Module* dst = new_module(a, get_module_name(src));
    Context ctx = {
        .rewriter = create_node_rewriter(src, dst, (RewriteNodeFn) process),
        .config = config,
        .arena = new_arena(),
        .alloca_info = new_dict(const Node*, AllocaInfo*, (HashFn) hash_node, (CmpFn) compare_node),
        .todo = false
    };
    rewrite_module(&ctx.rewriter);
    destroy_rewriter(&ctx.rewriter);
    destroy_dict(ctx.alloca_info);
    destroy_arena(ctx.arena);
    *m = dst;
    return ctx.todo;
}
