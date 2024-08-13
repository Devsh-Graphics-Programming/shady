#include "shady/pass.h"
#include "join_point_ops.h"

#include "../type.h"
#include "../transform/ir_gen_helpers.h"

#include "log.h"
#include "portability.h"

#include <assert.h>

typedef uint32_t FnPtr;

typedef struct Context_ {
    Rewriter rewriter;
    bool disable_lowering;

    const Node* return_jp;
} Context;

static const Node* lower_callf_process(Context* ctx, const Node* old) {
    const Node* found = search_processed(&ctx->rewriter, old);
    if (found) return found;
    IrArena* a = ctx->rewriter.dst_arena;
    Module* m = ctx->rewriter.dst_module;
    Rewriter* r = &ctx->rewriter;

    if (old->tag == Function_TAG) {
        Context ctx2 = *ctx;
        ctx2.disable_lowering = lookup_annotation(old, "Leaf");
        ctx2.return_jp = NULL;

        if (!ctx2.disable_lowering && get_abstraction_body(old)) {
            Nodes oparams = get_abstraction_params(old);
            Nodes nparams = recreate_params(&ctx->rewriter, oparams);
            register_processed_list(&ctx->rewriter, oparams, nparams);

            Nodes nannots = rewrite_nodes(&ctx->rewriter, old->payload.fun.annotations);

            Node* prelude = case_(a, empty(a));
            BodyBuilder* bb = begin_body_with_mem(a, get_abstraction_mem(prelude));

            // Supplement an additional parameter for the join point
            const Type* jp_type = join_point_type(a, (JoinPointType) {
                .yield_types = strip_qualifiers(a, rewrite_nodes(&ctx->rewriter, old->payload.fun.return_types))
            });

            if (lookup_annotation_list(old->payload.fun.annotations, "EntryPoint")) {
                ctx2.return_jp = gen_ext_instruction(bb, "shady.internal", ShadyOpDefaultJoinPoint, qualified_type_helper(jp_type, true), empty(a));
            } else {
                const Node* jp_variable = param(a, qualified_type_helper(jp_type, false), "return_jp");
                nparams = append_nodes(a, nparams, jp_variable);
                ctx2.return_jp = jp_variable;
            }

            Node* fun = function(ctx->rewriter.dst_module, nparams, get_abstraction_name(old), nannots, empty(a));
            register_processed(&ctx->rewriter, old, fun);

            register_processed(&ctx2.rewriter, get_abstraction_mem(old), bb_mem(bb));
            set_abstraction_body(prelude, finish_body(bb, rewrite_node(&ctx2.rewriter, old->payload.fun.body)));
            set_abstraction_body(fun, jump_helper(a, prelude, empty(a), get_abstraction_mem(fun)));
            return fun;
        }

        Node* fun = recreate_decl_header_identity(&ctx->rewriter, old);
        if (old->payload.fun.body)
            set_abstraction_body(fun, rewrite_node(&ctx2.rewriter, old->payload.fun.body));
        return fun;
    }

    if (ctx->disable_lowering)
        return recreate_node_identity(&ctx->rewriter, old);

    switch (old->tag) {
        case FnType_TAG: {
            Nodes param_types = rewrite_nodes(r, old->payload.fn_type.param_types);
            Nodes returned_types = rewrite_nodes(&ctx->rewriter, old->payload.fn_type.return_types);
            const Type* jp_type = qualified_type(a, (QualifiedType) {
                    .type = join_point_type(a, (JoinPointType) { .yield_types = strip_qualifiers(a, returned_types) }),
                    .is_uniform = false
            });
            param_types = append_nodes(a, param_types, jp_type);
            return fn_type(a, (FnType) {
                .param_types = param_types,
                .return_types = empty(a),
            });
        }
        case Return_TAG: {
            Nodes nargs = rewrite_nodes(&ctx->rewriter, old->payload.fn_ret.args);

            const Node* return_jp = ctx->return_jp;
            if (return_jp) {
                BodyBuilder* bb = begin_body_with_mem(a, rewrite_node(r, old->payload.fn_ret.mem));
                return_jp = gen_primop_ce(bb, subgroup_assume_uniform_op, 1, (const Node* []) { return_jp });
                // Join up at the return address instead of returning
                return finish_body(bb, join(a, (Join) {
                    .join_point = return_jp,
                    .args = nargs,
                    .mem = bb_mem(bb),
                }));
            } else {
                assert(false);
            }
        }
        // we convert calls to tail-calls within a control - only if the
        // call_indirect(...) to control(jp => save(jp); tailcall(...))
        case Call_TAG: {
            Call payload = old->payload.call;
            const Node* ocallee = payload.callee;
            // if we know the callee and it's a leaf - then we don't change the call
            if (ocallee->tag == FnAddr_TAG && lookup_annotation(ocallee->payload.fn_addr.fn, "Leaf"))
                break;

            const Type* ocallee_type = ocallee->type;
            bool callee_uniform = deconstruct_qualified_type(&ocallee_type);
            ocallee_type = get_pointee_type(a, ocallee_type);
            assert(ocallee_type->tag == FnType_TAG);
            Nodes returned_types = rewrite_nodes(&ctx->rewriter, ocallee_type->payload.fn_type.return_types);

            // Rewrite the callee and its arguments
            const Node* ncallee = rewrite_node(&ctx->rewriter, ocallee);
            Nodes nargs = rewrite_nodes(&ctx->rewriter, payload.args);

            // Create the body of the control that receives the appropriately typed join point
            const Type* jp_type = qualified_type(a, (QualifiedType) {
                    .type = join_point_type(a, (JoinPointType) { .yield_types = strip_qualifiers(a, returned_types) }),
                    .is_uniform = false
            });
            const Node* jp = param(a, jp_type, "fn_return_point");

            // Add that join point as the last argument to the newly made function
            nargs = append_nodes(a, nargs, jp);

            // the body of the control is just an immediate tail-call
            Node* control_case = case_(a, singleton(jp));
            const Node* control_body = tail_call(a, (TailCall) {
                .target = ncallee,
                .args = nargs,
                .mem = get_abstraction_mem(control_case),
            });
            set_abstraction_body(control_case, control_body);
            BodyBuilder* bb = begin_block_with_side_effects(a, rewrite_node(r, payload.mem));
            return yield_values_and_wrap_in_block(bb, gen_control(bb, strip_qualifiers(a, returned_types), control_case));
        }
        default: break;
    }
    return recreate_node_identity(&ctx->rewriter, old);
}

Module* lower_callf(SHADY_UNUSED const CompilerConfig* config, Module* src) {
    ArenaConfig aconfig = *get_arena_config(get_module_arena(src));
    IrArena* a = new_ir_arena(&aconfig);
    Module* dst = new_module(a, get_module_name(src));
    Context ctx = {
        .rewriter = create_node_rewriter(src, dst, (RewriteNodeFn) lower_callf_process),
        .disable_lowering = false,
    };
    rewrite_module(&ctx.rewriter);
    destroy_rewriter(&ctx.rewriter);
    return dst;
}
