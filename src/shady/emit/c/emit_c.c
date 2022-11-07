#include "emit_c.h"

#include "portability.h"
#include "dict.h"
#include "log.h"

#include "../../type.h"
#include "../../ir_private.h"

#include <assert.h>
#include <stdlib.h>

#pragma GCC diagnostic error "-Wswitch"

static void emit_terminator(Emitter* emitter, Printer* p, const Node* terminator);

CValue to_cvalue(SHADY_UNUSED Emitter* e, CTerm term) {
    if (term.value)
        return term.value;
    if (term.var)
        return format_string(e->arena, "(&%s)", term.var);
    assert(false);
}

CAddr deref_term(Emitter* e, CTerm term) {
    if (term.value)
        return format_string(e->arena, "(*%s)", term.value);
    if (term.var)
        return term.var;
    assert(false);
}

String emit_fn_head(Emitter* emitter, const Node* fn) {
    assert(fn->tag == Function_TAG);
    Nodes dom = fn->payload.fun.params;
    Nodes codom = fn->payload.fun.return_types;

    Growy* paramg = new_growy();
    Printer* paramp = open_growy_as_printer(paramg);
    if (dom.count == 0)
        print(paramp, "void");
    else for (size_t i = 0; i < dom.count; i++) {
        print(paramp, emit_type(emitter, dom.nodes[i]->type, format_string(emitter->arena, "%s_%d", dom.nodes[i]->payload.var.name, dom.nodes[i]->payload.var.id)));
        if (i + 1 < dom.count) {
            print(paramp, ", ");
        }
    }
    growy_append_bytes(paramg, 1, (char[]) { 0 });
    const char* parameters = printer_growy_unwrap(paramp);
    String center = format_string(emitter->arena, "%s(%s)", fn->payload.fun.name, parameters);
    free_tmp_str(parameters);

    return emit_type(emitter, wrap_multiple_yield_types(emitter->arena, codom), center);
}

#include <ctype.h>

static enum { ObjectsList, StringLit, CharsLit } array_insides_helper(Emitter* e, Printer* p, Growy* g, const Node* array) {
    const Type* t = array->payload.arr_lit.element_type;
    Nodes c = array->payload.arr_lit.contents;
    if (t->tag == Int_TAG && t->payload.int_type.width == 8) {
        uint8_t* tmp = malloc(sizeof(uint8_t) * c.count);
        bool ends_zero = false;
        for (size_t i = 0; i < c.count; i++) {
            tmp[i] = extract_int_literal_value(c.nodes[i], false);
            if (tmp[i] == 0) {
                if (i == c.count - 1)
                    ends_zero = true;
            }
        }
        bool is_stringy = ends_zero;
        for (size_t i = 0; i < c.count; i++) {
            // ignore the last char in a string
            if (is_stringy && i == c.count - 1)
                break;
            if (isprint(tmp[i]))
                print(p, "%c", tmp[i]);
            else
                print(p, "\\x%02x", tmp[i]);
        }
        free(tmp);
        return is_stringy ? StringLit : CharsLit;
    } else {
        for (size_t i = 0; i < c.count; i++) {
            print(p, to_cvalue(e, emit_value(e, c.nodes[i])));
            if (i + 1 < c.count)
                print(p, ", ");
        }
        growy_append_bytes(g, 1, "\0");
        return ObjectsList;
    }
}

CTerm emit_value(Emitter* emitter, const Node* value) {
    CTerm* found = lookup_existing_term(emitter, value);
    if (found) return *found;

    String emitted = NULL;

    switch (is_value(value)) {
        case NotAValue: assert(false);
        case Value_Unbound_TAG:
        case Value_UntypedNumber_TAG: error("lower me");
        case Value_Variable_TAG: error("variables need to be emitted beforehand");
        case Value_IntLiteral_TAG: emitted = format_string(emitter->arena, "%d", value->payload.int_literal.value.u64); break;
        case Value_True_TAG: return term_from_cvalue("true");
        case Value_False_TAG: return term_from_cvalue("false");
        case Value_Tuple_TAG: {
            Growy* g = new_growy();
            Printer* p = open_growy_as_printer(g);

            Nodes elements = value->payload.tuple.contents;
            for (size_t i = 0; i < elements.count; i++) {
                print(p, "%s, ", to_cvalue(emitter, emit_value(emitter, elements.nodes[i])));
            }

            emitted = format_string(emitter->arena, "((%s) { %s })", emit_type(emitter, value->type, NULL), growy_data(g));

            growy_destroy(g);
            destroy_printer(p);
            break;
        }
        case Value_StringLiteral_TAG: break;
        case Value_ArrayLiteral_TAG: {
            Growy* g = new_growy();
            Printer* p = open_growy_as_printer(g);
            switch (array_insides_helper(emitter, p, g, value)) {
                case ObjectsList:
                    emitted = format_string(emitter->arena, "((%s) { %s })", emit_type(emitter, value->payload.arr_lit.element_type, NULL), growy_data(g));
                    break;
                case StringLit:
                    emitted = format_string(emitter->arena, "((%s) { \"s\" })", emit_type(emitter, value->payload.arr_lit.element_type, NULL), growy_data(g));
                    break;
                case CharsLit:
                    emitted = format_string(emitter->arena, "((%s) { '%s' })", emit_type(emitter, value->payload.arr_lit.element_type, NULL), growy_data(g));
                    break;
            }
            growy_destroy(g);
            destroy_printer(p);
            break;
        }
        case Value_FnAddr_TAG: {
            emitted = get_decl_name(value->payload.fn_addr.fn);
            emitted = format_string(emitter->arena, "&%s", emitted);
            break;
        }
        case Value_RefDecl_TAG: {
            emit_decl(emitter, value->payload.ref_decl.decl);
            return *lookup_existing_term(emitter, value->payload.ref_decl.decl);
        }
    }

    assert(emitted);
    return term_from_cvalue(emitted);
}

static void emit_terminator(Emitter* emitter, Printer* p, const Node* terminator) {
    switch (is_terminator(terminator)) {
        case NotATerminator: assert(false);
        case LetMut_TAG:
        case Join_TAG: error("this must be lowered away!");
        case Jump_TAG:
        case Branch_TAG:
        case Switch_TAG:
        case TailCall_TAG: error("TODO");
        case Let_TAG: {
            const Node* instruction = terminator->payload.let.instruction;
            const Node* tail = terminator->payload.let.tail;
            assert(is_anonymous_lambda(tail));

            // we declare N local variables in order to store the result of the instruction
            Nodes yield_types = unwrap_multiple_yield_types(emitter->arena, instruction->type);
            const Nodes tail_params = tail->payload.anon_lam.params;
            assert(tail_params.count == yield_types.count);

            LARRAY(CTerm, results, yield_types.count);
            LARRAY(bool, need_binding, yield_types.count);
            InstructionOutputs ioutputs = {
                .count = yield_types.count,
                .results = results,
                .needs_binding = need_binding,
            };
            emit_instruction(emitter, p, instruction, ioutputs);

            for (size_t i = 0; i < yield_types.count; i++) {
                if (!need_binding[i]) {
                    register_emitted(emitter, tail_params.nodes[i], results[i]);
                    continue;
                }
                String bind_to = format_string(emitter->arena, "%s_%d", tail_params.nodes[i]->payload.var.name, fresh_id(emitter->arena));
                String decl = c_emit_type(emitter, yield_types.nodes[i], format_string(emitter->arena, "const %s", bind_to));
                print(p, "\nregister %s = %s;", decl, to_cvalue(emitter, results[i]));
                register_emitted(emitter, tail_params.nodes[i], term_from_cvalue(bind_to));
            }
            emit_terminator(emitter, p, tail->payload.anon_lam.body);
            break;
        }
        case LetIndirect_TAG: {
            const Node* instruction = terminator->payload.let_indirect.instruction;
            Nodes yield_types = unwrap_multiple_yield_types(emitter->arena, instruction->type);

            // TODO implement properly
            // TODO support Control ?
            //const Node* tail = terminator->payload.let_indirect.tail;
            //print(p, "\ngoto %s;", emit_decl(emitter, tail));
            break;
        }
        case Terminator_Return_TAG: {
            Nodes args = terminator->payload.fn_ret.args;
            if (args.count == 0) {
                print(p, "\nreturn;");
            } else if (args.count == 1) {
                print(p, "\nreturn %s;", to_cvalue(emitter, emit_value(emitter, args.nodes[0])));
            } else {
                String packed = unique_name(emitter->arena, "pack_return");
                LARRAY(CValue, values, args.count);
                for (size_t i = 0; i < args.count; i++)
                    values[i] = to_cvalue(emitter, emit_value(emitter, args.nodes[i]));
                emit_pack_code(p, strings(emitter->arena, args.count, values), packed);
                print(p, "\nreturn %s;", packed);
            }
            break;
        }
        case MergeSelection_TAG: {
            Nodes args = terminator->payload.merge_selection.args;
            Phis phis = emitter->phis.selection;
            assert(phis.count == args.count);
            for (size_t i = 0; i < phis.count; i++)
                print(p, "\n%s = %s;", phis.strings[i], to_cvalue(emitter, emit_value(emitter, args.nodes[i])));

            break;
        }
        case MergeContinue_TAG: {
            Nodes args = terminator->payload.merge_continue.args;
            Phis phis = emitter->phis.loop_continue;
            assert(phis.count == args.count);
            for (size_t i = 0; i < phis.count; i++)
                print(p, "\n%s = %s;", phis.strings[i], to_cvalue(emitter, emit_value(emitter, args.nodes[i])));
            print(p, "\ncontinue;");
            break;
        }
        case MergeBreak_TAG: {
            Nodes args = terminator->payload.merge_break.args;
            Phis phis = emitter->phis.loop_break;
            assert(phis.count == args.count);
            for (size_t i = 0; i < phis.count; i++)
                print(p, "\n%s = %s;", phis.strings[i], to_cvalue(emitter, emit_value(emitter, args.nodes[i])));
            print(p, "\nbreak;");
            break;
        }
        case Terminator_Unreachable_TAG: {
            assert(emitter->config.dialect == C);
            print(p, "\n__builtin_unreachable();");
            break;
        }
    }
}

void emit_lambda_body_at(Emitter* emitter, Printer* p, const Node* body, const Nodes* bbs) {
    assert(is_terminator(body));
    print(p, "{");
    indent(p);

    emit_terminator(emitter, p, body);

    if (bbs && bbs->count > 0) {
        assert(emitter->config.dialect != GLSL);
        error("TODO");
    }

    deindent(p);
    print(p, "\n}");
}

String emit_lambda_body(Emitter* emitter, const Node* body, const Nodes* bbs) {
    Growy* g = new_growy();
    Printer* p = open_growy_as_printer(g);
    emit_lambda_body_at(emitter, p, body, bbs);
    growy_append_bytes(g, 1, (char[]) { 0 });
    return printer_growy_unwrap(p);
}

void emit_decl(Emitter* emitter, const Node* decl) {
    assert(is_declaration(decl));

    CTerm* found = lookup_existing_term(emitter, decl);
    if (found) return;

    CTerm* found2 = lookup_existing_type(emitter, decl);
    if (found2) return;

    const char* name = get_decl_name(decl);
    const Type* decl_type = decl->type;
    const char* decl_center = name;
    CTerm emit_as;

    switch (decl->tag) {
        case GlobalVariable_TAG: {
            decl_type = decl->payload.global_variable.type;
            // we emit the global variable as a CVar, so we can refer to it's 'address' without explicit ptrs
            emit_as = term_from_cvar(name);

            register_emitted(emitter, decl, emit_as);
            if (decl->payload.global_variable.init)
                print(emitter->fn_defs, "\n%s = %s;", emit_type(emitter, decl_type, decl_center), to_cvalue(emitter, emit_value(emitter, decl->payload.global_variable.init)));
            break;
        }
        case Function_TAG: {
            emit_as = term_from_cvalue(name);
            register_emitted(emitter, decl, emit_as);
            const Node* body = decl->payload.fun.body;
            if (body) {
                for (size_t i = 0; i < decl->payload.fun.params.count; i++) {
                    const char* param_name = format_string(emitter->arena, "%s_%d", decl->payload.fun.params.nodes[i]->payload.var.name, decl->payload.fun.params.nodes[i]->payload.var.id);
                    register_emitted(emitter, decl->payload.fun.params.nodes[i], term_from_cvalue(param_name));
                }

                String fn_body = emit_lambda_body(emitter, body, NULL);
                print(emitter->fn_defs, "\n%s %s", emit_fn_head(emitter, decl), fn_body);
                free_tmp_str(fn_body);
            }
            break;
        }
        case Constant_TAG: {
            emit_as = term_from_cvalue(name);
            register_emitted(emitter, decl, emit_as);
            decl_center = format_string(emitter->arena, "const %s", decl_center);
            print(emitter->fn_defs, "\n%s = %s;", emit_type(emitter, decl->type, decl_center), to_cvalue(emitter, emit_value(emitter, decl->payload.constant.value)));
            break;
        }
        case NominalType_TAG: {
            CType emitted = decl->payload.nom_type.name;
            register_emitted_type(emitter, decl, emitted);
            print(emitter->type_decls, "\ntypedef %s;", emit_type(emitter, decl->payload.nom_type.body, emitted));
            return;
        }
        default: error("not a decl");
    }

    String declaration = emit_type(emitter, decl_type, decl_center);
    print(emitter->fn_decls, "\n%s;", declaration);
}

void register_emitted(Emitter* emitter, const Node* node, CTerm as) {
    assert(as.value || as.var);
    insert_dict(const Node*, CTerm, emitter->emitted_terms, node, as);
}

void register_emitted_type(Emitter* emitter, const Node* node, String as) {
    insert_dict(const Node*, String, emitter->emitted_types, node, as);
}

CTerm* lookup_existing_term(Emitter* emitter, const Node* node) {
    CTerm* found = find_value_dict(const Node*, CTerm, emitter->emitted_terms, node);
    return found;
}

CType* lookup_existing_type(Emitter* emitter, const Type* node) {
    CType* found = find_value_dict(const Node*, CType, emitter->emitted_types, node);
    return found;
}

KeyHash hash_node(Node**);
bool compare_node(Node**, Node**);

void emit_c(CompilerConfig* config, Module* mod, size_t* output_size, char** output) {
    IrArena* arena = get_module_arena(mod);
    Growy* type_decls_g = new_growy();
    Growy* fn_decls_g = new_growy();
    Growy* fn_defs_g = new_growy();

    Emitter emitter = {
        .config = {
            .config = config,
            .dialect = C,
        },
        .arena = arena,
        .type_decls = open_growy_as_printer(type_decls_g),
        .fn_decls = open_growy_as_printer(fn_decls_g),
        .fn_defs = open_growy_as_printer(fn_defs_g),
        .emitted_terms = new_dict(Node*, CTerm, (HashFn) hash_node, (CmpFn) compare_node),
        .emitted_types = new_dict(Node*, String, (HashFn) hash_node, (CmpFn) compare_node),
    };

    Nodes decls = get_module_declarations(mod);
    for (size_t i = 0; i < decls.count; i++)
        emit_decl(&emitter, decls.nodes[i]);

    destroy_printer(emitter.type_decls);
    destroy_printer(emitter.fn_decls);
    destroy_printer(emitter.fn_defs);

    Growy* final = new_growy();
    Printer* finalp = open_growy_as_printer(final);

    print(finalp, "/* file generated by shady */\n");

    if (emitter.config.dialect == C) {
        print(finalp, "\n#include <stdbool.h>");
        print(finalp, "\n#include <stdint.h>");
        print(finalp, "\n#include <stddef.h>");
    }

    print(finalp, "\n/* types: */\n");
    growy_append_bytes(final, growy_size(type_decls_g), growy_data(type_decls_g));

    print(finalp, "\n/* declarations: */\n");
    growy_append_bytes(final, growy_size(fn_decls_g), growy_data(fn_decls_g));

    print(finalp, "\n/* definitions: */\n");
    growy_append_bytes(final, growy_size(fn_defs_g), growy_data(fn_defs_g));

    print(finalp, "\n");

    growy_destroy(type_decls_g);
    growy_destroy(fn_decls_g);
    growy_destroy(fn_defs_g);

    destroy_dict(emitter.emitted_types);
    destroy_dict(emitter.emitted_terms);

    *output_size = growy_size(final);
    *output = growy_deconstruct(final);
    destroy_printer(finalp);
}
