#include "emit_spv.h"

#include "../shady/type.h"

#include "list.h"
#include "dict.h"
#include "log.h"
#include "portability.h"

#include <assert.h>

BBBuilder spv_find_basic_block_builder(Emitter* emitter, const Node* bb) {
    // assert(is_basic_block(bb));
    BBBuilder* found = find_value_dict(const Node*, BBBuilder, emitter->bb_builders, bb);
    assert(found);
    return *found;
}

static void add_branch_phis(Emitter* emitter, BBBuilder bb_builder, const Node* jump) {
    assert(jump->tag == Jump_TAG);
    const Node* dst = jump->payload.jump.target;
    Nodes args = jump->payload.jump.args;
    // because it's forbidden to jump back into the entry block of a function
    // (which is actually a Function in this IR, not a BasicBlock)
    // we assert that the destination must be an actual BasicBlock
    assert(is_basic_block(dst));
    BBBuilder dst_builder = spv_find_basic_block_builder(emitter, dst);
    struct List* phis = spbv_get_phis(dst_builder);
    assert(entries_count_list(phis) == args.count);
    for (size_t i = 0; i < args.count; i++) {
        SpvbPhi* phi = read_list(SpvbPhi*, phis)[i];
        spvb_add_phi_source(phi, get_block_builder_id(bb_builder), emit_value(emitter, bb_builder, args.nodes[i]));
    }
}

static void emit_if(Emitter* emitter, FnBuilder fn_builder, BBBuilder bb_builder, MergeTargets merge_targets, If if_instr) {
    Nodes yield_types = if_instr.yield_types;
    Nodes results = get_abstraction_params(if_instr.tail);
    SpvId join_bb_id = spvb_fresh_id(emitter->file_builder);

    SpvId true_id = spvb_fresh_id(emitter->file_builder);
    SpvId false_id = if_instr.if_false ? spvb_fresh_id(emitter->file_builder) : join_bb_id;

    spvb_selection_merge(bb_builder, join_bb_id, 0);
    SpvId condition = emit_value(emitter, bb_builder, if_instr.condition);
    spvb_branch_conditional(bb_builder, condition, true_id, false_id);

    // When 'join' is codegen'd, these will be filled with the values given to it
    BBBuilder join_bb = spvb_begin_bb(fn_builder, join_bb_id);
    LARRAY(SpvbPhi*, join_phis, yield_types.count);
    for (size_t i = 0; i < yield_types.count; i++) {
        assert(if_instr.if_false && "Ifs with yield types need false branches !");
        SpvId phi_id = spvb_fresh_id(emitter->file_builder);
        SpvId type = emit_type(emitter, yield_types.nodes[i]);
        SpvbPhi* phi = spvb_add_phi(join_bb, type, phi_id);
        join_phis[i] = phi;
        register_result(emitter, results.nodes[i], phi_id);
    }

    MergeTargets merge_targets_branches = merge_targets;
    merge_targets_branches.join_target = join_bb_id;
    merge_targets_branches.join_phis = join_phis;

    BBBuilder true_bb = spvb_begin_bb(fn_builder, true_id);
    spvb_add_bb(fn_builder, true_bb);
    emit_terminator(emitter, fn_builder, true_bb, merge_targets_branches, get_abstraction_body(if_instr.if_true));
    if (if_instr.if_false) {
        BBBuilder false_bb = spvb_begin_bb(fn_builder, false_id);
        spvb_add_bb(fn_builder, false_bb);
        emit_terminator(emitter, fn_builder, false_bb, merge_targets_branches, get_abstraction_body(if_instr.if_false));
    }

    spvb_add_bb(fn_builder, join_bb);
    bb_builder = join_bb;

    emit_terminator(emitter, fn_builder, bb_builder, merge_targets, get_abstraction_body(if_instr.tail));
}

static void emit_match(Emitter* emitter, FnBuilder fn_builder, BBBuilder bb_builder, MergeTargets merge_targets, Match match) {
    Nodes yield_types = match.yield_types;
    SpvId join_bb_id = spvb_fresh_id(emitter->file_builder);

    assert(get_unqualified_type(match.inspect->type)->tag == Int_TAG);
    SpvId inspectee = emit_value(emitter, bb_builder, match.inspect);

    SpvId default_id = spvb_fresh_id(emitter->file_builder);

    const Type* inspectee_t = match.inspect->type;
    deconstruct_qualified_type(&inspectee_t);
    assert(inspectee_t->tag == Int_TAG);
    size_t literal_width = inspectee_t->payload.int_type.width == IntTy64 ? 2 : 1;
    size_t literal_case_entry_size = literal_width + 1;
    LARRAY(uint32_t, literals_and_cases, match.cases.count * literal_case_entry_size);
    for (size_t i = 0; i < match.cases.count; i++) {
        uint64_t value = (uint64_t) get_int_literal_value(*resolve_to_int_literal(match.literals.nodes[i]), false);
        if (inspectee_t->payload.int_type.width == IntTy64) {
            literals_and_cases[i * literal_case_entry_size + 0] = (SpvId) (uint32_t) (value & 0xFFFFFFFF);
            literals_and_cases[i * literal_case_entry_size + 1] = (SpvId) (uint32_t) (value >> 32);
        } else {
            literals_and_cases[i * literal_case_entry_size + 0] = (SpvId) (uint32_t) value;
        }
        literals_and_cases[i * literal_case_entry_size + literal_width] = spvb_fresh_id(emitter->file_builder);
    }

    spvb_selection_merge(bb_builder, join_bb_id, 0);
    spvb_switch(bb_builder, inspectee, default_id, match.cases.count * literal_case_entry_size, literals_and_cases);

    // When 'join' is codegen'd, these will be filled with the values given to it
    BBBuilder join_bb = spvb_begin_bb(fn_builder, join_bb_id);
    LARRAY(SpvbPhi*, join_phis, yield_types.count);
    Nodes results = get_abstraction_params(match.tail);
    for (size_t i = 0; i < yield_types.count; i++) {
        SpvId phi_id = spvb_fresh_id(emitter->file_builder);
        SpvId type = emit_type(emitter, yield_types.nodes[i]);
        SpvbPhi* phi = spvb_add_phi(join_bb, type, phi_id);
        join_phis[i] = phi;
        register_result(emitter, results.nodes[i], phi_id);
    }

    MergeTargets merge_targets_branches = merge_targets;
    merge_targets_branches.join_target = join_bb_id;
    merge_targets_branches.join_phis = join_phis;

    for (size_t i = 0; i < match.cases.count; i++) {
        BBBuilder case_bb = spvb_begin_bb(fn_builder, literals_and_cases[i * literal_case_entry_size + literal_width]);
        const Node* case_ = match.cases.nodes[i];
        spvb_add_bb(fn_builder, case_bb);
        emit_terminator(emitter, fn_builder, case_bb, merge_targets_branches, get_abstraction_body(case_));
    }
    BBBuilder default_bb = spvb_begin_bb(fn_builder, default_id);
    spvb_add_bb(fn_builder, default_bb);
    emit_terminator(emitter, fn_builder, default_bb, merge_targets_branches, get_abstraction_body(match.default_case));

    spvb_add_bb(fn_builder, join_bb);
    bb_builder = join_bb;

    emit_terminator(emitter, fn_builder, bb_builder, merge_targets, get_abstraction_body(match.tail));
}

static void emit_loop(Emitter* emitter, FnBuilder fn_builder, BBBuilder bb_builder, MergeTargets merge_targets, Loop loop_instr) {
    Nodes yield_types = loop_instr.yield_types;

    const Node* loop_inside = loop_instr.body;
    Nodes loop_params = get_abstraction_params(loop_inside);

    // First we create all the basic blocks we'll need
    SpvId header_id = spvb_fresh_id(emitter->file_builder);
    BBBuilder header_builder = spvb_begin_bb(fn_builder, header_id);
    spvb_name(emitter->file_builder, header_id, "loop_header");

    SpvId body_id = spvb_fresh_id(emitter->file_builder);
    BBBuilder body_builder = spvb_begin_bb(fn_builder, body_id);
    spvb_name(emitter->file_builder, body_id, "loop_body");

    SpvId continue_id = spvb_fresh_id(emitter->file_builder);
    BBBuilder continue_builder = spvb_begin_bb(fn_builder, continue_id);
    spvb_name(emitter->file_builder, continue_id, "loop_continue");

    SpvId next_id = spvb_fresh_id(emitter->file_builder);
    BBBuilder next = spvb_begin_bb(fn_builder, next_id);
    spvb_name(emitter->file_builder, next_id, "loop_next");

    // Wire up the phi nodes for loop exit
    LARRAY(SpvbPhi*, loop_break_phis, yield_types.count);
    Nodes results = get_abstraction_params(loop_instr.tail);
    for (size_t i = 0; i < yield_types.count; i++) {
        SpvId yielded_type = emit_type(emitter, get_unqualified_type(yield_types.nodes[i]));

        SpvId break_phi_id = spvb_fresh_id(emitter->file_builder);
        SpvbPhi* phi = spvb_add_phi(next, yielded_type, break_phi_id);
        loop_break_phis[i] = phi;
        register_result(emitter, results.nodes[i], break_phi_id);
    }

    // Wire up the phi nodes for the loop contents
    LARRAY(SpvbPhi*, loop_continue_phis, loop_params.count);
    for (size_t i = 0; i < loop_params.count; i++) {
        SpvId loop_param_type = emit_type(emitter, get_unqualified_type(loop_params.nodes[i]->type));

        SpvId continue_phi_id = spvb_fresh_id(emitter->file_builder);
        SpvbPhi* continue_phi = spvb_add_phi(continue_builder, loop_param_type, continue_phi_id);
        loop_continue_phis[i] = continue_phi;

        // To get the actual loop parameter, we make a second phi for the nodes that go into the header
        // We already know the two edges into the header so we immediately add the Phi sources for it.
        SpvId loop_param_id = spvb_fresh_id(emitter->file_builder);
        SpvbPhi* loop_param_phi = spvb_add_phi(header_builder, loop_param_type, loop_param_id);
        SpvId param_initial_value = emit_value(emitter, bb_builder, loop_instr.initial_args.nodes[i]);
        spvb_add_phi_source(loop_param_phi, get_block_builder_id(bb_builder), param_initial_value);
        spvb_add_phi_source(loop_param_phi, get_block_builder_id(continue_builder), continue_phi_id);
        register_result(emitter, loop_params.nodes[i], loop_param_id);
    }

    // The current block goes to the header (it can't be the header itself !)
    spvb_branch(bb_builder, header_id);
    spvb_add_bb(fn_builder, header_builder);

    // the header block receives the loop merge annotation
    spvb_loop_merge(header_builder, next_id, continue_id, 0, 0, NULL);
    spvb_branch(header_builder, body_id);
    spvb_add_bb(fn_builder, body_builder);

    // Emission of the body requires extra info for the break/continue merge terminators
    MergeTargets merge_targets_branches = merge_targets;
    merge_targets_branches.continue_target = continue_id;
    merge_targets_branches.continue_phis = loop_continue_phis;
    merge_targets_branches.break_target = next_id;
    merge_targets_branches.break_phis = loop_break_phis;
    emit_terminator(emitter, fn_builder, body_builder, merge_targets_branches, get_abstraction_body(loop_inside));

    // the continue block just jumps back into the header
    spvb_branch(continue_builder, header_id);
    spvb_add_bb(fn_builder, continue_builder);

    // We start the next block
    spvb_add_bb(fn_builder, next);
    bb_builder = next;

    emit_terminator(emitter, fn_builder, bb_builder, merge_targets, get_abstraction_body(loop_instr.tail));
}

void emit_terminator(Emitter* emitter, FnBuilder fn_builder, BBBuilder basic_block_builder, MergeTargets merge_targets, const Node* terminator) {
    switch (is_terminator(terminator)) {
        case Return_TAG: {
            const Nodes* ret_values = &terminator->payload.fn_ret.args;
            switch (ret_values->count) {
                case 0: spvb_return_void(basic_block_builder); return;
                case 1: spvb_return_value(basic_block_builder, emit_value(emitter, basic_block_builder, ret_values->nodes[0])); return;
                default: {
                    LARRAY(SpvId, arr, ret_values->count);
                    for (size_t i = 0; i < ret_values->count; i++)
                        arr[i] = emit_value(emitter, basic_block_builder, ret_values->nodes[i]);
                    SpvId return_that = spvb_composite(basic_block_builder, fn_ret_type_id(fn_builder), ret_values->count, arr);
                    spvb_return_value(basic_block_builder, return_that);
                    return;
                }
            }
        }
        /*case Let_TAG: {
            const Node* instruction = terminator->payload.let.instruction;
            Nodes types = unwrap_multiple_yield_types(emitter->arena, instruction->type);
            LARRAY(SpvId, results, types.count);
            emit_instruction(emitter, fn_builder, basic_block_builder, instruction, types.count, results);

            for (size_t i = 0; i < types.count; i++)
                register_result(emitter, extract_multiple_ret_types_helper(instruction, i), results[i]);
            emit_terminator(emitter, fn_builder, basic_block_builder, merge_targets, terminator->payload.let.in);
            return;
        }*/
        case Unreachable_TAG: {
            spvb_unreachable(basic_block_builder);
            return;
        }
        case Jump_TAG: {
            add_branch_phis(emitter, basic_block_builder, terminator);
            spvb_branch(basic_block_builder, find_reserved_id(emitter, terminator->payload.jump.target));
            return;
        }
        case Branch_TAG: {
            SpvId condition = emit_value(emitter, basic_block_builder, terminator->payload.branch.condition);
            add_branch_phis(emitter, basic_block_builder, terminator->payload.branch.true_jump);
            add_branch_phis(emitter, basic_block_builder, terminator->payload.branch.false_jump);
            spvb_branch_conditional(basic_block_builder, condition, find_reserved_id(emitter, terminator->payload.branch.true_jump->payload.jump.target), find_reserved_id(emitter, terminator->payload.branch.false_jump->payload.jump.target));
            return;
        }
        case Switch_TAG: {
            SpvId inspectee = emit_value(emitter, basic_block_builder, terminator->payload.br_switch.switch_value);
            LARRAY(SpvId, targets, terminator->payload.br_switch.case_jumps.count * 2);
            for (size_t i = 0; i < terminator->payload.br_switch.case_jumps.count; i++) {
                add_branch_phis(emitter, basic_block_builder, terminator->payload.br_switch.case_jumps.nodes[i]);
                error("TODO finish")
            }
            add_branch_phis(emitter, basic_block_builder, terminator->payload.br_switch.default_jump);
            SpvId default_tgt = find_reserved_id(emitter, terminator->payload.br_switch.default_jump->payload.jump.target);

            spvb_switch(basic_block_builder, inspectee, default_tgt, terminator->payload.br_switch.case_jumps.count, targets);
            return;
        }
        case If_TAG: return emit_if(emitter, fn_builder, basic_block_builder, merge_targets, terminator->payload.if_instr);
        case Match_TAG: return emit_match(emitter, fn_builder, basic_block_builder, merge_targets, terminator->payload.match_instr);
        case Loop_TAG: return emit_loop(emitter, fn_builder, basic_block_builder, merge_targets, terminator->payload.loop_instr);
        case MergeSelection_TAG: {
            Nodes args = terminator->payload.merge_selection.args;
            for (size_t i = 0; i < args.count; i++)
                spvb_add_phi_source(merge_targets.join_phis[i], get_block_builder_id(basic_block_builder), emit_value(emitter, basic_block_builder, args.nodes[i]));
            spvb_branch(basic_block_builder, merge_targets.join_target);
            return;
        }
        case MergeContinue_TAG: {
            Nodes args = terminator->payload.merge_continue.args;
            for (size_t i = 0; i < args.count; i++)
                spvb_add_phi_source(merge_targets.continue_phis[i], get_block_builder_id(basic_block_builder), emit_value(emitter, basic_block_builder, args.nodes[i]));
            spvb_branch(basic_block_builder, merge_targets.continue_target);
            return;
        }
        case MergeBreak_TAG: {
            Nodes args = terminator->payload.merge_break.args;
            for (size_t i = 0; i < args.count; i++)
                spvb_add_phi_source(merge_targets.break_phis[i], get_block_builder_id(basic_block_builder), emit_value(emitter, basic_block_builder, args.nodes[i]));
            spvb_branch(basic_block_builder, merge_targets.break_target);
            return;
        }
        case Terminator_Control_TAG:
        case TailCall_TAG:
        case Join_TAG: error("Lower me");
        case NotATerminator: error("TODO: emit terminator %s", node_tags[terminator->tag]);
    }
    SHADY_UNREACHABLE;
}
