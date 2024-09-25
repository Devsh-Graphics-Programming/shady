#include "shady/ir.h"
#include "shady/pass.h"

#include "../rewrite.h"
#include "../ir_private.h"
#include "../analysis/cfg.h"
#include "../analysis/scheduler.h"
#include "../analysis/looptree.h"
#include "../analysis/uses.h"
#include "../analysis/leak.h"
#include "../analysis/free_frontier.h"

#include "portability.h"
#include "log.h"
#include "dict.h"

typedef struct Context_ {
    Rewriter rewriter;
    const CompilerConfig* config;
    const Node* current_fn;
    CFG* cfg;
    Scheduler* scheduler;
    LoopTree* loop_tree;
    struct Dict* lifted_arguments;
} Context;

static bool is_child(const LTNode* maybe_parent, const LTNode* child) {
    const LTNode* n = child;
    while (n) {
        if (n == maybe_parent)
            return true;
        n = n->parent;
    }
    return NULL;
}

static const LTNode* get_loop(const LTNode* n) {
    const LTNode* p = n->parent;
    if (p && p->type == LF_HEAD)
        return p;
    return NULL;
}

static String loop_name(const LTNode* n) {
    if (n && n->type == LF_HEAD && shd_list_count(n->cf_nodes) > 0) {
        return get_abstraction_name(shd_read_list(CFNode*, n->cf_nodes)[0]->node);
    }
    return "";
}

void find_liftable_loop_values(Context* ctx, const Node* old, Nodes* nparams, Nodes* lparams, Nodes* nargs) {
    IrArena* a = ctx->rewriter.dst_arena;
    assert(old->tag == BasicBlock_TAG);

    const LTNode* bb_loop = get_loop(looptree_lookup(ctx->loop_tree, old));

    *nparams = empty(a);
    *lparams = empty(a);
    *nargs = empty(a);

    struct Dict* fvs = free_frontier(ctx->scheduler, ctx->cfg, old);
    const Node* fv;
    for (size_t i = 0; dict_iter(fvs, &i, &fv, NULL);) {
        const CFNode* defining_cf_node = schedule_instruction(ctx->scheduler, fv);
        assert(defining_cf_node);
        const LTNode* defining_loop = get_loop(looptree_lookup(ctx->loop_tree, defining_cf_node->node));
        if (!is_child(defining_loop, bb_loop)) {
            // that's it, that variable is leaking !
            log_string(DEBUGV, "lcssa: ");
            log_node(DEBUGV, fv);
            log_string(DEBUGV, " (%%%d) is used outside of the loop that defines it %s %s\n", fv->id, loop_name(defining_loop), loop_name(bb_loop));
            const Node* narg = rewrite_node(&ctx->rewriter, fv);
            const Node* nparam = param(a, narg->type, "lcssa_phi");
            *nparams = append_nodes(a, *nparams, nparam);
            *lparams = append_nodes(a, *lparams, fv);
            *nargs = append_nodes(a, *nargs, narg);
        }
    }
    destroy_dict(fvs);

    if (nparams->count > 0)
        insert_dict(const Node*, Nodes, ctx->lifted_arguments, old, *nparams);
}

const Node* process_abstraction_body(Context* ctx, const Node* old, const Node* body) {
    IrArena* a = ctx->rewriter.dst_arena;
    Context ctx2 = *ctx;
    ctx = &ctx2;

    if (!ctx->cfg) {
        error_print("LCSSA: Trying to process an abstraction that's not part of a function ('%s')!", get_abstraction_name(old));
        log_module(ERROR, ctx->config, ctx->rewriter.src_module);
        error_die();
    }
    const CFNode* n = cfg_lookup(ctx->cfg, old);

    size_t children_count = 0;
    LARRAY(const Node*, old_children, shd_list_count(n->dominates));
    for (size_t i = 0; i < shd_list_count(n->dominates); i++) {
        CFNode* c = shd_read_list(CFNode*, n->dominates)[i];
        if (is_cfnode_structural_target(c))
            continue;
        old_children[children_count++] = c->node;
    }

    LARRAY(Node*, new_children, children_count);
    LARRAY(Nodes, lifted_params, children_count);
    LARRAY(Nodes, new_params, children_count);
    for (size_t i = 0; i < children_count; i++) {
        Nodes nargs;
        find_liftable_loop_values(ctx, old_children[i], &new_params[i], &lifted_params[i], &nargs);
        Nodes nparams = recreate_params(&ctx->rewriter, get_abstraction_params(old_children[i]));
        new_children[i] = basic_block(a, concat_nodes(a, nparams, new_params[i]), get_abstraction_name(old_children[i]));
        register_processed(&ctx->rewriter, old_children[i], new_children[i]);
        register_processed_list(&ctx->rewriter, get_abstraction_params(old_children[i]), nparams);
        insert_dict(const Node*, Nodes, ctx->lifted_arguments, old_children[i], nargs);
    }

    const Node* new = rewrite_node(&ctx->rewriter, body);

    ctx->rewriter.map = clone_dict(ctx->rewriter.map);

    for (size_t i = 0; i < children_count; i++) {
        for (size_t j = 0; j < lifted_params[i].count; j++) {
            remove_dict(const Node*, ctx->rewriter.map, lifted_params[i].nodes[j]);
        }
        register_processed_list(&ctx->rewriter, lifted_params[i], new_params[i]);
        new_children[i]->payload.basic_block.body = process_abstraction_body(ctx, old_children[i], get_abstraction_body(old_children[i]));
    }

    destroy_dict(ctx->rewriter.map);

    return new;
}

const Node* process_node(Context* ctx, const Node* old) {
    Rewriter* r = &ctx->rewriter;
    IrArena* a = r->dst_arena;

    switch (old->tag) {
        case NominalType_TAG:
        case GlobalVariable_TAG:
        case Constant_TAG: {
            Context not_a_fn_ctx = *ctx;
            ctx = &not_a_fn_ctx;
            ctx->cfg = NULL;
            return recreate_node_identity(&ctx->rewriter, old);
        }
        case Function_TAG: {
            Context fn_ctx = *ctx;
            ctx = &fn_ctx;

            ctx->current_fn = old;
            ctx->cfg = build_fn_cfg(old);
            ctx->scheduler = new_scheduler(ctx->cfg);
            ctx->loop_tree = build_loop_tree(ctx->cfg);

            Node* new = recreate_decl_header_identity(&ctx->rewriter, old);
            new->payload.fun.body = process_abstraction_body(ctx, old, get_abstraction_body(old));

            destroy_loop_tree(ctx->loop_tree);
            destroy_scheduler(ctx->scheduler);
            destroy_cfg(ctx->cfg);
            return new;
        }
        case Jump_TAG: {
            Jump payload = old->payload.jump;
            Nodes nargs = rewrite_nodes(&ctx->rewriter, old->payload.jump.args);
            Nodes* lifted_args = find_value_dict(const Node*, Nodes, ctx->lifted_arguments, old->payload.jump.target);
            if (lifted_args) {
                nargs = concat_nodes(a, nargs, *lifted_args);
            }
            return jump(a, (Jump) {
                .target = rewrite_node(&ctx->rewriter, old->payload.jump.target),
                .args = nargs,
                .mem = rewrite_node(r, payload.mem),
            });
        }
        case BasicBlock_TAG: {
            assert(false);
        }
        default: break;
    }

    return recreate_node_identity(&ctx->rewriter, old);
}

KeyHash hash_node(Node**);
bool compare_node(Node**, Node**);

Module* lcssa(const CompilerConfig* config, Module* src) {
    ArenaConfig aconfig = *get_arena_config(get_module_arena(src));
    IrArena* a = new_ir_arena(&aconfig);
    Module* dst = new_module(a, get_module_name(src));

    Context ctx = {
        .rewriter = create_node_rewriter(src, dst, (RewriteNodeFn) process_node),
        .config = config,
        .current_fn = NULL,
        .lifted_arguments = new_dict(const Node*, Nodes, (HashFn) hash_node, (CmpFn) compare_node)
    };

    rewrite_module(&ctx.rewriter);
    destroy_rewriter(&ctx.rewriter);
    destroy_dict(ctx.lifted_arguments);
    return dst;
}
