#include "callgraph.h"
#include "uses.h"

#include "list.h"
#include "dict.h"
#include "portability.h"
#include "log.h"

#include "../visit.h"

#include <stdlib.h>
#include <string.h>
#include <assert.h>

KeyHash hash_node(const Node**);
bool compare_node(const Node**, const Node**);

KeyHash hash_cgedge(CGEdge* n) {
    return hash_murmur(n, sizeof(CGEdge));

}
bool compare_cgedge(CGEdge* a, CGEdge* b) {
    return (a)->src_fn == (b)->src_fn && (a)->instr == (b)->instr;
}

static CGNode* analyze_fn(CallGraph* graph, const Node* fn);

typedef struct {
    Visitor visitor;
    CallGraph* graph;
    CGNode* root;
} CGVisitor;

static const Node* ignore_immediate_fn_addr(const Node* node) {
    if (node->tag == FnAddr_TAG) {
        return node->payload.fn_addr.fn;
    }
    return node;
}

static CGNode* analyze_fn(CallGraph* graph, const Node* fn);

static void visit_callsite(CGVisitor* visitor, const Node* callee, const Node* instr) {
    assert(visitor->root);
    assert(callee->tag == Function_TAG);
    CGNode* target = analyze_fn(visitor->graph, callee);
    // Immediate recursion
    if (target == visitor->root)
        visitor->root->is_recursive = true;
    CGEdge edge = {
        .src_fn = visitor->root,
        .dst_fn = target,
        .instr = instr,
    };
    insert_set_get_result(CGEdge, visitor->root->callees, edge);
    insert_set_get_result(CGEdge, target->callers, edge);
}

static void search_for_callsites(CGVisitor* visitor, const Node* node) {
    if (is_abstraction(node))
        search_for_callsites(visitor, get_abstraction_body(node));
    switch (node->tag) {
        case Call_TAG: {
            assert(visitor->root && "calls can only occur in functions");
            const Node* callee = node->payload.call.callee;
            callee = ignore_immediate_fn_addr(callee);
            if (callee->tag == Function_TAG)
                visit_callsite(visitor, callee, node);
            else
                visitor->root->calls_indirect = true;
            break;
        }
        case TailCall_TAG: {
            assert(visitor->root && "tail calls can only occur in functions");
            const Node* callee = node->payload.tail_call.callee;
            callee = ignore_immediate_fn_addr(callee);
            if (callee->tag == Function_TAG)
                visit_callsite(visitor, callee, node);
            else
                visitor->root->calls_indirect = true;
            break;
        }
        default: break;
    }
    visit_node_operands(&visitor->visitor, ~NcMem, node);
}

static CGNode* analyze_fn(CallGraph* graph, const Node* fn) {
    assert(fn && fn->tag == Function_TAG);
    CGNode** found = find_value_dict(const Node*, CGNode*, graph->fn2cgn, fn);
    if (found)
        return *found;
    CGNode* new = calloc(1, sizeof(CGNode));
    new->fn = fn;
    new->callees = new_set(CGEdge, (HashFn) hash_cgedge, (CmpFn) compare_cgedge);
    new->callers = new_set(CGEdge, (HashFn) hash_cgedge, (CmpFn) compare_cgedge);
    new->tarjan.index = -1;
    insert_dict_and_get_key(const Node*, CGNode*, graph->fn2cgn, fn, new);

    CGVisitor v = {
        .visitor = {
            .visit_node_fn = (VisitNodeFn) search_for_callsites
        },
        .graph = graph,
        .root = new,
    };

    if (get_abstraction_body(fn)) {
        visit_function_rpo(&v.visitor, fn);
    }

    return new;
}

#ifdef _MSC_VER
// MSVC is the world's worst C11 compiler.
// Standard headers pollute the global namespace, they do have an path to not do so, but they rely on __STDC__ being defined, which it isn't as of the latest Visual Studio 2022 release.
// Of course the Visual Studio docs say __STDC__ should be defined, but after wasting a day to this, I can positively say that's a big fat lie.
// https://docs.microsoft.com/en-us/cpp/preprocessor/predefined-macros?view=msvc-170
// I could, of course, simply avoid this issue by renaming. That would work and would be practical, and would be the sane thing to do.
// But it's not the right thing :tm: to do, and out of spite, this hack will remain in place until this issue is fixed.
// (Visual Studio 17.3.2, build tools 14.33.31629, windows 10 SDK 10.0.22621.0)
#undef min
#endif

static int min(int a, int b) { return a < b ? a : b; }

// https://en.wikipedia.org/wiki/Tarjan%27s_strongly_connected_components_algorithm
static void strongconnect(CGNode* v, int* index, struct List* stack) {
    debugv_print("strongconnect(%s) \n", v->fn->payload.fun.name);

    v->tarjan.index = *index;
    v->tarjan.lowlink = *index;
    (*index)++;
    shd_list_append(const Node*, stack, v);
    v->tarjan.on_stack = true;

    // Consider successors of v
    {
        size_t iter = 0;
        CGEdge e;
        debugv_print(" has %d successors\n", entries_count_dict(v->callees));
        while (dict_iter(v->callees, &iter, &e, NULL)) {
            debugv_print("  %s\n", e.dst_fn->fn->payload.fun.name);
            if (e.dst_fn->tarjan.index == -1) {
                // Successor w has not yet been visited; recurse on it
                strongconnect(e.dst_fn, index, stack);
                v->tarjan.lowlink = min(v->tarjan.lowlink, e.dst_fn->tarjan.lowlink);
            } else if (e.dst_fn->tarjan.on_stack) {
                // Successor w is in stack S and hence in the current SCC
                // If w is not on stack, then (v, w) is an edge pointing to an SCC already found and must be ignored
                // Note: The next line may look odd - but is correct.
                // It says w.index not w.lowlink; that is deliberate and from the original paper
                v->tarjan.lowlink = min(v->tarjan.lowlink, e.dst_fn->tarjan.index);
            }
        }
    }

    // If v is a root node, pop the stack and generate an SCC
    if (v->tarjan.lowlink == v->tarjan.index) {
        LARRAY(CGNode*, scc, shd_list_count(stack));
        size_t scc_size = 0;
        {
            CGNode* w;
            assert(shd_list_count(stack) > 0);
            do {
                w = shd_list_pop(CGNode*, stack);
                w->tarjan.on_stack = false;
                scc[scc_size++] = w;
            } while (v != w);
        }

        if (scc_size > 1) {
            for (size_t i = 0; i < scc_size; i++) {
                CGNode* w = scc[i];
                debugv_print("Function %s is part of a recursive call chain \n", w->fn->payload.fun.name);
                w->is_recursive = true;
            }
        }
    }
}

static void tarjan(struct Dict* verts) {
    int index = 0;
    struct List* stack = shd_new_list(CGNode*);

    size_t iter = 0;
    CGNode* n;
    while (dict_iter(verts, &iter, NULL, &n)) {
        if (n->tarjan.index == -1)
            strongconnect(n, &index, stack);
    }

    shd_destroy_list(stack);
}

CallGraph* new_callgraph(Module* mod) {
    CallGraph* graph = calloc(sizeof(CallGraph), 1);
    *graph = (CallGraph) {
        .fn2cgn = new_dict(const Node*, CGNode*, (HashFn) hash_node, (CmpFn) compare_node)
    };

    const UsesMap* uses = create_module_uses_map(mod, NcType);

    Nodes decls = get_module_declarations(mod);
    for (size_t i = 0; i < decls.count; i++) {
        const Node* decl = decls.nodes[i];
        if (decl->tag == Function_TAG) {
            CGNode* node = analyze_fn(graph, decl);

            const Use* use = get_first_use(uses, fn_addr_helper(get_module_arena(mod), decl));
            for (;use;use = use->next_use) {
                if (use->user->tag == Call_TAG && strcmp(use->operand_name, "callee") == 0)
                    continue;
                if (use->user->tag == TailCall_TAG && strcmp(use->operand_name, "callee") == 0)
                    continue;
                node->is_address_captured = true;
            }
        } else if (decl->tag == GlobalVariable_TAG && decl->payload.global_variable.init) {
            CGVisitor v = {
                .visitor = {
                    .visit_node_fn = (VisitNodeFn) search_for_callsites
                },
                .graph = graph,
                .root = NULL,
            };
            search_for_callsites(&v, decl->payload.global_variable.init);
        } else if (decl->tag == Constant_TAG && decl->payload.constant.value) {
            CGVisitor v = {
                .visitor = {
                    .visit_node_fn = (VisitNodeFn) search_for_callsites
                },
                .graph = graph,
                .root = NULL,
            };
            search_for_callsites(&v, decl->payload.constant.value);
        }
    }

    destroy_uses_map(uses);

    debugv_print("CallGraph: done with CFG build, contains %d nodes\n", entries_count_dict(graph->fn2cgn));

    tarjan(graph->fn2cgn);

    return graph;
}

void destroy_callgraph(CallGraph* graph) {
    size_t i = 0;
    CGNode* node;
    while (dict_iter(graph->fn2cgn, &i, NULL, &node)) {
        debugv_print("Freeing CG node: %s\n", node->fn->payload.fun.name);
        destroy_dict(node->callers);
        destroy_dict(node->callees);
        free(node);
    }
    destroy_dict(graph->fn2cgn);
    free(graph);
}
